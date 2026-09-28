#include "pkg_live_cache.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.hpp"

namespace prospero {
namespace PkgLiveCache {

namespace {

constexpr uint64_t kSeg = kPkgLiveSegSize;
constexpr int kSlots = 64;         // 64 MB RAM ceiling per session.
constexpr int kTimeoutSec = 300;   // Bound on any single blocking wait.
constexpr int kSeekCoalesceMs = 2000;

struct Live {
    std::mutex mu;
    std::condition_variable changed; // any arrival/finish/abort wakes waiters

    bool active = false;
    bool upload_complete = false;
    bool aborted = false;
    int refs = 0;
    bool free_pending = false;

    std::string session_id;
    std::string filename;
    uint64_t total = 0;
    uint64_t nsegs = 0;

    std::vector<uint8_t> present;    // per-segment present flag
    std::vector<int> slot_of;        // per-segment RAM slot index, -1 = absent
    std::vector<uint8_t> slots;      // nslots * kSeg bytes
    int nslots = 0;
    std::vector<uint64_t> last_use;  // per-slot LRU stamp
    std::vector<int> slot_seg;       // per-slot file segment, -1 = free
    std::vector<int> waiters;        // per-segment parked-reader count (eviction shield)
    std::vector<uint8_t> acked_ever; // per-segment stored-at-least-once (finish gate)
    std::vector<uint64_t> seek_sent; // per-segment last seek-emission stamp (ms)

    uint64_t present_count = 0;
    uint64_t served = 0;   // bytes delivered to readers (status only)
    uint64_t run_hi = 0;   // offset past reader's last served read (eviction HWM)
    uint64_t peak_seg = 0; // highest segment the reader has sought (far-seek HWM)
    uint64_t tick = 0;

    PkgLiveSeekFn seek_fn;
    PkgLiveDoneFn done_fn;
};

Live g_lv;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

// 1s tick so a wedged write can log progress periodically; returns true when the
// absolute deadline has passed.
bool TickWait(std::unique_lock<std::mutex> &lock, std::chrono::steady_clock::time_point deadline) {
    auto tick = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    g_lv.changed.wait_until(lock, tick);
    return std::chrono::steady_clock::now() >= deadline;
}

void FreeLocked() {
    g_lv.present.clear();
    g_lv.slot_of.clear();
    g_lv.slots.clear();
    g_lv.last_use.clear();
    g_lv.slot_seg.clear();
    g_lv.waiters.clear();
    g_lv.acked_ever.clear();
    g_lv.seek_sent.clear();
    g_lv.free_pending = false;
}

uint64_t SegLenLocked(uint64_t seg) {
    uint64_t start = seg * kSeg;
    if (start >= g_lv.total) return 0;
    uint64_t left = g_lv.total - start;
    return left < kSeg ? left : kSeg;
}

// Frees a slot for segment `seg`, LRU among whatever's evictable right now — served-past,
// urgently waited-on, or (once the reader has sought ahead) any acknowledged slot.
int AllocSlotLocked(uint64_t seg) {
    if (seg < g_lv.nsegs && g_lv.slot_of[seg] >= 0) return g_lv.slot_of[seg];

    int free_slot = -1;
    for (int i = 0; i < g_lv.nslots; i++) {
        if (g_lv.slot_seg[i] < 0) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        bool incoming_waited = seg < g_lv.nsegs && g_lv.waiters[seg] > 0;
        bool far = seg >= g_lv.peak_seg;
        uint64_t oldest = 0;
        int oldest_seg = -1;
        bool first = true;
        for (uint64_t s = 1; s < g_lv.nsegs; s++) {
            if (!g_lv.present[s] || g_lv.slot_of[s] < 0) continue;
            if (g_lv.waiters[s] > 0) continue;
            bool evictable = false;
            if (g_lv.run_hi >= (s + 1) * kSeg) {
                evictable = true; // A: served-past
            } else if (incoming_waited && g_lv.acked_ever[s]) {
                evictable = true; // B: urgent read
            } else if (g_lv.peak_seg > 0 && g_lv.acked_ever[s] && (far || s != seg)) {
                evictable = true; // C: demand page
            }
            if (!evictable) continue;
            uint64_t stamp = g_lv.last_use[static_cast<size_t>(g_lv.slot_of[s])];
            if (first || stamp < oldest) {
                oldest = stamp;
                oldest_seg = static_cast<int>(s);
                first = false;
            }
        }
        if (oldest_seg < 0) return -1;
        free_slot = g_lv.slot_of[static_cast<size_t>(oldest_seg)];
        g_lv.present[static_cast<size_t>(oldest_seg)] = 0;
        g_lv.slot_of[static_cast<size_t>(oldest_seg)] = -1;
    }
    if (seg < g_lv.nsegs) {
        g_lv.slot_of[seg] = free_slot;
        g_lv.slot_seg[static_cast<size_t>(free_slot)] = static_cast<int>(seg);
    }
    return free_slot;
}

// Stores one whole segment under the lock. 1 = message doesn't overlap it, -1 = protocol
// error, -2 = nothing evictable right now (busy).
int StoreSegLocked(uint64_t seg, const void *data, uint64_t offset, size_t len) {
    uint64_t s_start = seg * kSeg;
    uint64_t s_len = SegLenLocked(seg);
    uint64_t last_end = offset + len;
    uint64_t w_start = std::max(offset, s_start);
    uint64_t w_end = std::min(last_end, s_start + s_len);
    if (w_end <= w_start) return 1;
    if (w_end - w_start != s_len) return -1; // only the short final segment may be partial

    int slot = AllocSlotLocked(seg);
    if (slot < 0) return -2;
    std::memcpy(g_lv.slots.data() + static_cast<size_t>(slot) * kSeg,
                static_cast<const uint8_t *>(data) + (w_start - offset), static_cast<size_t>(s_len));
    if (!g_lv.present[seg]) {
        g_lv.present[seg] = 1;
        g_lv.present_count++;
    }
    g_lv.acked_ever[seg] = 1;
    g_lv.last_use[static_cast<size_t>(slot)] = ++g_lv.tick;
    return slot;
}

} // namespace

int Create(const std::string &filename, uint64_t total_size, std::string *out_session_id) {
    if (filename.empty() || total_size == 0) return -1;
    size_t slash = filename.find_last_of('/');
    std::string base = slash == std::string::npos ? filename : filename.substr(slash + 1);
    if (base.empty() || base == "." || base == "..") return -1;

    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (g_lv.active) return -2;
    if (g_lv.refs > 0) return -2;

    uint64_t nsegs = (total_size + kSeg - 1) / kSeg;
    int nslots = kSlots;
    if (static_cast<uint64_t>(nslots) > nsegs + 4) nslots = static_cast<int>(nsegs + 4);
    if (nslots < 4) nslots = 4;

    g_lv.present.assign(nsegs, 0);
    g_lv.slot_of.assign(nsegs, -1);
    g_lv.slots.assign(static_cast<size_t>(nslots) * kSeg, 0);
    g_lv.last_use.assign(static_cast<size_t>(nslots), 0);
    g_lv.slot_seg.assign(static_cast<size_t>(nslots), -1);
    g_lv.waiters.assign(nsegs, 0);
    g_lv.acked_ever.assign(nsegs, 0);
    g_lv.seek_sent.assign(nsegs, 0);
    g_lv.nslots = nslots;

    char sid[24];
    static unsigned counter = 0;
    counter++;
    uint64_t seed = NowMs() ^ (static_cast<uint64_t>(counter) * 2654435761ull);
    std::snprintf(sid, sizeof(sid), "%016llx", static_cast<unsigned long long>(seed));
    g_lv.session_id = sid;
    g_lv.filename = base;
    g_lv.total = total_size;
    g_lv.nsegs = nsegs;
    g_lv.present_count = 0;
    g_lv.served = 0;
    g_lv.run_hi = 0;
    g_lv.peak_seg = 0;
    g_lv.upload_complete = false;
    g_lv.aborted = false;
    g_lv.refs = 0;
    g_lv.free_pending = false;
    g_lv.active = true;

    if (out_session_id) *out_session_id = g_lv.session_id;
    LogEvent("[pkglive] session '" + g_lv.session_id + "' ready: file='" + base + "' total=" +
             std::to_string(total_size) + " segs=" + std::to_string(nsegs) + " slots=" + std::to_string(nslots));
    return 0;
}

bool SessionActive() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    return g_lv.active;
}

bool CheckId(const std::string &session_id) {
    if (session_id.empty()) return false;
    std::lock_guard<std::mutex> lock(g_lv.mu);
    return g_lv.active && g_lv.session_id == session_id;
}

void SetSeekFn(PkgLiveSeekFn fn) {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    g_lv.seek_fn = std::move(fn);
}

void SetDoneFn(PkgLiveDoneFn fn) {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    g_lv.done_fn = std::move(fn);
}

int Write(uint64_t offset, const void *data, size_t len) {
    if (!data && len > 0) return -1;
    std::unique_lock<std::mutex> lock(g_lv.mu);
    if (!g_lv.active || g_lv.aborted) return -1;
    if (len == 0) return 0;
    if (offset + len > g_lv.total || offset % kSeg != 0) return -1;

    uint64_t first_seg = offset / kSeg;
    uint64_t last_seg = (offset + len - 1) / kSeg;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutSec);

    for (uint64_t s = first_seg; s <= last_seg; s++) {
        for (;;) {
            if (g_lv.aborted) return -1;
            int r = StoreSegLocked(s, data, offset, len);
            if (r == 1 || r >= 0) break;
            if (r == -1) return -1;
            if (TickWait(lock, deadline)) {
                LogEvent("[pkglive] write seg " + std::to_string(s) + " timed out waiting for readers");
                return -1;
            }
        }
    }
    g_lv.changed.notify_all();
    return 0;
}

int TryWrite(uint64_t offset, const void *data, size_t len) {
    if (!data && len > 0) return -1;
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (!g_lv.active || g_lv.aborted) return -1;
    if (len == 0) return 0;
    if (offset + len > g_lv.total || offset % kSeg != 0) return -1;

    uint64_t first_seg = offset / kSeg;
    uint64_t last_seg = (offset + len - 1) / kSeg;
    for (uint64_t s = first_seg; s <= last_seg; s++) {
        int r = StoreSegLocked(s, data, offset, len);
        if (r == 1) continue;
        if (r == -1) return -1;
        if (r == -2) return -2; // earlier segs of this call (if any) stay stored, retry is idempotent
    }
    g_lv.changed.notify_all();
    return 0;
}

int WaiterCount() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (!g_lv.active) return 0;
    int n = 0;
    for (int w : g_lv.waiters) n += w;
    return n;
}

int Finish() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    uint64_t acked = 0;
    for (uint8_t a : g_lv.acked_ever) acked += a;
    if (!g_lv.active || g_lv.aborted || acked != g_lv.nsegs) return -1;
    g_lv.upload_complete = true;
    LogEvent("[pkglive] upload byte-complete: '" + g_lv.filename + "' (" + std::to_string(g_lv.total) + " bytes)");
    g_lv.changed.notify_all();
    return 0;
}

void Abort() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (g_lv.active && !g_lv.aborted) {
        g_lv.aborted = true;
        LogEvent("[pkglive] session aborted");
    }
    g_lv.changed.notify_all();
}

void Destroy() {
    PkgLiveDoneFn done_fn;
    {
        std::lock_guard<std::mutex> lock(g_lv.mu);
        if (!g_lv.active) return;
        g_lv.active = false;
        g_lv.aborted = true;
        g_lv.changed.notify_all();
        if (g_lv.refs == 0) {
            FreeLocked();
            LogEvent("[pkglive] session destroyed");
        } else {
            g_lv.free_pending = true;
        }
        g_lv.upload_complete = false;
        g_lv.session_id.clear();
        g_lv.filename.clear();
        g_lv.total = 0;
        g_lv.nsegs = 0;
        g_lv.present_count = 0;
        g_lv.served = 0;
        done_fn = g_lv.done_fn;
    }
    // Closes the browser's connection right away instead of leaving it to time out.
    if (done_fn) done_fn();
}

int WaitHeader(int timeout_sec) {
    std::unique_lock<std::mutex> lock(g_lv.mu);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec > 0 ? timeout_sec : kTimeoutSec);
    for (;;) {
        if (!g_lv.active || g_lv.aborted) return -1;
        if (g_lv.nsegs > 0 && g_lv.present[0]) return 0;
        if (TickWait(lock, deadline)) return -1;
    }
}

size_t GetHeader(uint8_t *out, size_t len) {
    if (!out || len == 0) return 0;
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (!g_lv.active || g_lv.nsegs == 0 || !g_lv.present[0]) return 0;
    size_t full = static_cast<size_t>(SegLenLocked(0));
    size_t got = std::min(len, full);
    std::memcpy(out, g_lv.slots.data() + static_cast<size_t>(g_lv.slot_of[0]) * kSeg, got);
    return got;
}

namespace {
// Emits a seek request for `seg`, coalesced to at most once every kSeekCoalesceMs.
// Caller holds the lock; the sink itself must be invoked without it.
bool EmitSeekLocked(uint64_t seg, PkgLiveSeekFn *out_fn) {
    uint64_t now = NowMs();
    if (seg < g_lv.nsegs && now - g_lv.seek_sent[seg] > static_cast<uint64_t>(kSeekCoalesceMs)) {
        g_lv.seek_sent[seg] = now;
        *out_fn = g_lv.seek_fn;
        return true;
    }
    return false;
}
} // namespace

long Read(uint64_t off, void *buf, size_t count) {
    if (!buf) return -1;
    if (count == 0) return 0;
    std::unique_lock<std::mutex> lock(g_lv.mu);
    if (!g_lv.active) return -1;
    if (off >= g_lv.total) return 0;
    if (count > g_lv.total - off) count = static_cast<size_t>(g_lv.total - off);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutSec);
    auto *dst = static_cast<uint8_t *>(buf);
    size_t got = 0;
    uint64_t read_start = off;
    uint64_t served_end = off + count;

    for (;;) {
        if (g_lv.aborted) return -1;
        uint64_t end = off + (count - got);

        uint64_t miss = end;
        for (uint64_t o = off; o < end;) {
            uint64_t s = o / kSeg;
            if (s >= g_lv.nsegs || !g_lv.present[s]) { miss = o; break; }
            uint64_t s_end = std::min<uint64_t>((s + 1) * kSeg, g_lv.total);
            o = s_end > o ? s_end : o + 1;
            if (o >= end) { miss = end; break; }
        }

        if (miss >= end) {
            while (got < count) {
                uint64_t s = off / kSeg;
                int slot = s < g_lv.nsegs ? g_lv.slot_of[s] : -1;
                if (slot < 0) break; // raced teardown: rescan
                uint64_t s_start = s * kSeg;
                size_t so = static_cast<size_t>(off - s_start);
                size_t m = std::min(static_cast<size_t>(kSeg - so), count - got);
                uint64_t seg_left = SegLenLocked(s);
                if (so + m > seg_left) m = static_cast<size_t>(seg_left - so);
                if (m == 0) break;
                std::memcpy(dst + got, g_lv.slots.data() + static_cast<size_t>(slot) * kSeg + so, m);
                g_lv.last_use[static_cast<size_t>(slot)] = ++g_lv.tick;
                g_lv.served += m;
                off += m;
                got += m;
            }
            g_lv.changed.notify_all(); // writer space freed
            if (got >= count) {
                if (read_start <= g_lv.run_hi + kSeg && served_end > g_lv.run_hi) g_lv.run_hi = served_end;
                return static_cast<long>(got);
            }
            continue; // raced teardown: rescan
        }

        uint64_t need_seg = miss / kSeg;
        if (need_seg > g_lv.peak_seg && need_seg < g_lv.nsegs) g_lv.peak_seg = need_seg;

        PkgLiveSeekFn fn;
        bool emit = false;
        if (need_seg < g_lv.nsegs) g_lv.waiters[need_seg]++;
        // Segment 0 is the pinned header: always delivered first and never evicted, so
        // just wait for it rather than seeking; every other segment is the browser's job.
        if (need_seg > 0) emit = EmitSeekLocked(need_seg, &fn);
        if (emit && fn) {
            lock.unlock();
            fn(need_seg);
            lock.lock();
            if (!g_lv.active || g_lv.aborted) {
                if (need_seg < g_lv.nsegs && g_lv.waiters[need_seg] > 0) g_lv.waiters[need_seg]--;
                return -1;
            }
            if (need_seg < g_lv.nsegs && g_lv.present[need_seg]) {
                if (g_lv.waiters[need_seg] > 0) g_lv.waiters[need_seg]--;
                continue;
            }
        }
        bool expired = TickWait(lock, deadline);
        if (need_seg < g_lv.nsegs && g_lv.waiters[need_seg] > 0) g_lv.waiters[need_seg]--;
        if (expired) {
            LogEvent("[pkglive] read timed out at off=" + std::to_string(off));
            return -1;
        }
    }
}

PkgLiveStatus GetStatus() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    PkgLiveStatus st;
    st.active = g_lv.active;
    st.aborted = g_lv.aborted;
    if (!g_lv.active) return st;
    st.complete = g_lv.upload_complete;
    st.session_id = g_lv.session_id;
    st.filename = g_lv.filename;
    st.total = g_lv.total;
    st.served = g_lv.served;
    for (uint64_t s = 0; s < g_lv.nsegs; s++)
        if (g_lv.acked_ever[s]) st.received += SegLenLocked(s);
    return st;
}

uint64_t GetTotal() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    return g_lv.active ? g_lv.total : 0;
}

uint64_t GetResumeOffset() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (!g_lv.active) return 0;
    uint64_t s = 0;
    while (s < g_lv.nsegs && g_lv.acked_ever[s]) s++;
    return std::min(s * kSeg, g_lv.total);
}

int Attach() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (!g_lv.active) return -1;
    g_lv.refs++;
    return 0;
}

void Detach() {
    std::lock_guard<std::mutex> lock(g_lv.mu);
    if (g_lv.refs > 0) g_lv.refs--;
    if (g_lv.refs == 0 && g_lv.free_pending) {
        FreeLocked();
        LogEvent("[pkglive] session destroyed (deferred)");
    }
}

} // namespace PkgLiveCache
} // namespace prospero
