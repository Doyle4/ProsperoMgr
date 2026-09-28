#include "job_queue.hpp"
#include "pkg_installer.hpp"

#include <sys/stat.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "https_client.hpp"
#include "elfldr_client.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "pkg_live_cache.hpp"
#include "pkg_stream.hpp"
#include "process_manager.hpp"
#include "ps5_services.hpp"

namespace prospero {

namespace fs = std::filesystem;

namespace {

// Game packages use the public "\x7FCNT..." signature, homebrew tile packages "\x7FFIH...".
// ContentID sits at 0x40 in the PS4 layout, but not always — the header is also scanned.
constexpr int kContentIdOffset = 0x40;
constexpr int kContentIdLen = 36;
constexpr int kHeaderScanBytes = 0x2000;
constexpr auto kPkgStagingTtl = std::chrono::hours(24);

fs::path PkgStagingDir(const Config &cfg) { return fs::path(cfg.tmp_dir) / "pkg_downloads"; }

bool IsManagedPkgFile(const Config &cfg, const std::string &path) {
    fs::path normalized = fs::path(path).lexically_normal();
    return normalized.parent_path() == PkgStagingDir(cfg).lexically_normal() && !normalized.filename().empty();
}

void RemoveManagedPkgFile(const Config &cfg, const std::string &path) {
    if (!IsManagedPkgFile(cfg, path)) return;
    std::error_code ec;
    fs::remove(path, ec);
}

// "IV0000-LAPY20011_00-LAPY200110000000" — fixed 36 chars with separators at
// three known positions, specific enough not to match arbitrary binary.
bool LooksLikeContentId(const char *s) {
    for (int i = 0; i < kContentIdLen; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) return false;
    }
    return s[6] == '-' && s[16] == '_' && s[19] == '-';
}

std::string PkgDisplayName(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return name.empty() ? "PKG" : name;
}

// Cheap sniff of the first bytes so a downloaded HTML page can be reported as
// what it is instead of as a corrupt package.
bool LooksLikeHtml(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    char head[512]{};
    f.read(head, sizeof(head) - 1);
    std::string text(head, static_cast<size_t>(f.gcount()));
    for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text.find("<!doctype html") != std::string::npos || text.find("<html") != std::string::npos ||
           text.find("<head") != std::string::npos;
}

bool ValidatePkgHeader(const std::string &path, std::string *content_id, std::string *err) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err) *err = "PKG path is not a readable regular file";
        return false;
    }
    if (st.st_size < kContentIdOffset + kContentIdLen) {
        if (err) *err = "PKG is incomplete: file is smaller than its header";
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    unsigned char magic[4]{};
    f.read(reinterpret_cast<char *>(magic), sizeof(magic));
    bool game_pkg = magic[1] == 'C' && magic[2] == 'N' && magic[3] == 'T';
    bool tile_pkg = magic[1] == 'F' && magic[2] == 'I' && magic[3] == 'H';
    if (f.gcount() != static_cast<std::streamsize>(sizeof(magic)) || magic[0] != 0x7f ||
        (!game_pkg && !tile_pkg)) {
        // A share link (Google Drive, MediaFire, Gofile, ...) serves its
        // viewer page, not the file, so what lands here is HTML.
        if (err) *err = LooksLikeHtml(path) ? "the link returned a web page instead of a file — share links from "
                                              "Google Drive, MediaFire, Gofile and similar need a direct download "
                                              "URL that ends in .pkg"
                                            : "not a valid PS4/PS5 PKG (unsupported package header)";
        return false;
    }

    // The content ID is only ever displayed — the system reads its own copy
    // out of the package, so not finding one is no reason to refuse the install.
    if (content_id) *content_id = ReadPkgContentId(path);
    return true;
}

std::string NormalizePkgDownloadUrl(const std::string &url) {
    constexpr char kGithubPrefix[] = "https://github.com/";
    if (url.compare(0, sizeof(kGithubPrefix) - 1, kGithubPrefix) != 0) return url;

    size_t blob = url.find("/blob/", sizeof(kGithubPrefix) - 1);
    if (blob == std::string::npos) return url;

    std::string direct = url;
    direct.replace(blob, 6, "/raw/");
    return direct;
}

} // namespace

std::string ReadPkgContentId(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";

    std::vector<char> hdr(kHeaderScanBytes);
    f.read(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    size_t got = static_cast<size_t>(f.gcount());
    if (got < static_cast<size_t>(kContentIdOffset + kContentIdLen)) return "";
    bool game_pkg = hdr[1] == 'C' && hdr[2] == 'N' && hdr[3] == 'T';
    bool tile_pkg = hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == 'H';
    if (static_cast<uint8_t>(hdr[0]) != 0x7F || (!game_pkg && !tile_pkg)) return "";

    size_t at = static_cast<size_t>(kContentIdOffset);
    if (!LooksLikeContentId(hdr.data() + at)) {
        // Not the PS4 layout — find it wherever this package keeps it.
        at = got;
        for (size_t i = 0; i + kContentIdLen <= got; i++) {
            if (LooksLikeContentId(hdr.data() + i)) { at = i; break; }
        }
        if (at == got) return "";
    }

    std::string id(hdr.data() + at, kContentIdLen);
    while (!id.empty() && (id.back() == ' ' || id.back() == '\0')) id.pop_back();
    return id;
}

namespace {

// PKG tables are big-endian; param.sfo inside them is little-endian.
uint32_t ReadBe32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint32_t ReadLe32(const unsigned char *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t ReadLe16(const unsigned char *p) { return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8)); }

bool ReadAt(std::ifstream &f, long long offset, void *out, size_t size) {
    if (offset < 0) return false;
    f.clear();
    f.seekg(offset, std::ios::beg);
    if (!f) return false;
    f.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
    return static_cast<size_t>(f.gcount()) == size;
}

std::string TrimmedField(const char *data, size_t len) {
    while (len > 0 && (data[len - 1] == '\0' || data[len - 1] == ' ')) len--;
    return std::string(data, len);
}

// param.sfo is a flat key/value table: a key-offset table, then a data table.
void ParseParamSfo(const std::vector<unsigned char> &sfo, PkgMetadata *out) {
    if (sfo.size() < 20 || std::memcmp(sfo.data(), "\x00PSF", 4) != 0) return;
    uint32_t keys_at = ReadLe32(sfo.data() + 0x08);
    uint32_t data_at = ReadLe32(sfo.data() + 0x0C);
    uint32_t count = ReadLe32(sfo.data() + 0x10);
    if (keys_at >= sfo.size() || data_at >= sfo.size() || count > 1024) return;

    for (uint32_t i = 0; i < count; i++) {
        size_t entry = 20 + size_t(i) * 16;
        if (entry + 16 > sfo.size()) break;
        const unsigned char *e = sfo.data() + entry;
        size_t key_at = keys_at + ReadLe16(e);
        uint32_t len = ReadLe32(e + 4);
        size_t value_at = data_at + ReadLe32(e + 12);
        if (key_at >= sfo.size() || value_at + len > sfo.size()) continue;

        const char *key = reinterpret_cast<const char *>(sfo.data() + key_at);
        if (!std::memchr(key, '\0', sfo.size() - key_at)) continue;
        std::string value = TrimmedField(reinterpret_cast<const char *>(sfo.data() + value_at), len);
        if (value.empty()) continue;

        if (std::strcmp(key, "TITLE") == 0 && out->title_name.empty()) out->title_name = value;
        else if (std::strcmp(key, "TITLE_ID") == 0 && out->title_id.empty()) out->title_id = value;
        else if (std::strcmp(key, "CATEGORY") == 0 && out->category.empty()) out->category = value;
        else if ((std::strcmp(key, "APP_VER") == 0 || std::strcmp(key, "VERSION") == 0) &&
                 out->version.empty()) {
            out->version = value;
        }
    }
}

// Minimal string lookup — enough for param.json's flat "key": "value" fields.
std::string JsonField(const std::string &text, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) return "";
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) return "";
    size_t open = text.find('"', at);
    if (open == std::string::npos) return "";
    size_t close = text.find('"', open + 1);
    if (close == std::string::npos) return "";
    return text.substr(open + 1, close - open - 1);
}

// "gp" is a patch, "ac"/"al"/"addcont" extra content; everything else is a base app.
std::string CategoryKind(const std::string &category) {
    if (category.rfind("gp", 0) == 0) return "update";
    if (category.rfind("ac", 0) == 0 || category.rfind("al", 0) == 0 || category == "addcont") return "dlc";
    if (category.empty()) return "";
    return "base";
}

// The PS5 container wraps the PS4-style CNT header; find whichever applies.
long long FindCntOffset(std::ifstream &f) {
    unsigned char magic[4]{};
    if (!ReadAt(f, 0, magic, sizeof(magic))) return -1;
    if (std::memcmp(magic, "\x7f" "CNT", 4) == 0) return 0;
    if (std::memcmp(magic, "\x7f" "FIH", 4) != 0) return -1;
    if (ReadAt(f, 0x58, magic, sizeof(magic)) && std::memcmp(magic, "\x7f" "CNT", 4) == 0) return 0x58;
    for (long long at = 0x40; at <= 0x2000; at += 0x10) {
        if (ReadAt(f, at, magic, sizeof(magic)) && std::memcmp(magic, "\x7f" "CNT", 4) == 0) return at;
    }
    return -1;
}

} // namespace

PkgMetadata ReadPkgMetadata(const std::string &path) {
    PkgMetadata out;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return out;

    long long cnt = FindCntOffset(f);
    if (cnt < 0) return out;

    unsigned char header[0x100]{};
    if (!ReadAt(f, cnt, header, sizeof(header))) return out;
    out.content_id = TrimmedField(reinterpret_cast<const char *>(header + kContentIdOffset), kContentIdLen);

    uint32_t count = ReadBe32(header + 0x10);
    uint32_t table_at = ReadBe32(header + 0x18);
    if (count == 0 || count > 2048 || table_at > 0x200000) return out;

    std::vector<unsigned char> table(size_t(count) * 32);
    if (!ReadAt(f, cnt + table_at, table.data(), table.size())) return out;

    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = table.data() + size_t(i) * 32;
        uint32_t type = ReadBe32(e);
        uint32_t data_at = ReadBe32(e + 16);
        uint32_t size = ReadBe32(e + 20);
        // 256 KiB is far above any real param file and keeps a corrupt table cheap.
        if (size == 0 || size > 256 * 1024) continue;

        if (type == 0x1000) { // param.sfo — PS4 titles
            std::vector<unsigned char> sfo(size);
            if (ReadAt(f, cnt + data_at, sfo.data(), sfo.size())) ParseParamSfo(sfo, &out);
        } else if (type == 0x2000) { // param.json — PS5 native titles
            std::string json(size, '\0');
            if (!ReadAt(f, cnt + data_at, json.data(), json.size())) continue;
            if (out.title_name.empty()) out.title_name = JsonField(json, "titleName");
            if (out.title_id.empty()) out.title_id = JsonField(json, "titleId");
            if (out.version.empty()) out.version = JsonField(json, "contentVersion");
            if (out.category.empty()) out.category = JsonField(json, "category");
        }
    }

    if (out.content_id.size() >= 20 && out.title_id.empty()) out.title_id = out.content_id.substr(7, 9);
    out.category = CategoryKind(out.category);
    return out;
}

bool PkgDownloadToTemp(const Config &cfg, const std::string &url, std::string *local_path, std::string *err,
                       const std::function<void(long long, long long)> &progress) {
    std::string dir = PkgStagingDir(cfg).string();
    mkdir(dir.c_str(), 0755);

    std::string download_url = NormalizePkgDownloadUrl(url);
    size_t slash = download_url.find_last_of('/');
    std::string base = slash == std::string::npos ? "download.pkg" : download_url.substr(slash + 1);
    size_t q = base.find('?');
    if (q != std::string::npos) base = base.substr(0, q);
    if (base.empty() || base.size() > 128) base = "download.pkg";
    if (base.size() < 4 || base.compare(base.size() - 4, 4, ".pkg") != 0) base += ".pkg";

    std::string dest = dir + "/" + base;
    if (!HttpsClient::DownloadToFile(download_url, dest, err, progress)) {
        std::remove(dest.c_str());
        return false;
    }

    *local_path = dest;
    return true;
}

// ---- Background job status --------------------------------------------

namespace {
std::mutex g_job_mu;
PkgJobStatus g_job;
bool g_job_running = false;
} // namespace

void CleanupStalePkgFiles(const Config &cfg) {
    std::error_code ec;
    fs::path dir = PkgStagingDir(cfg);
    if (!fs::is_directory(dir, ec)) return;

    auto now = fs::file_time_type::clock::now();
    size_t removed = 0;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        fs::file_status status = entry.symlink_status(ec);
        if (ec) { ec.clear(); continue; }
        if (!fs::is_regular_file(status)) continue;
        auto modified = entry.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (now - modified <= kPkgStagingTtl) continue;
        if (fs::remove(entry.path(), ec)) removed++;
        if (ec) ec.clear();
    }
    if (removed > 0) LogEvent("Removed " + std::to_string(removed) + " expired staged PKG file(s)");
}

PkgJobStatus GetPkgJobStatus() {
    std::lock_guard<std::mutex> lock(g_job_mu);
    return g_job;
}

bool DeleteStagedPkgFile(const Config &cfg, std::string *err) {
    std::lock_guard<std::mutex> lock(g_job_mu);
    if (g_job.staged_path.empty()) {
        if (err) *err = "nothing to delete";
        return false;
    }
    if (!IsManagedPkgFile(cfg, g_job.staged_path)) {
        g_job.staged_path.clear();
        if (err) *err = "nothing to delete";
        return false;
    }
    std::error_code ec;
    fs::remove(g_job.staged_path, ec);
    g_job.staged_path.clear();
    if (ec) {
        if (err) *err = "could not delete the downloaded file: " + ec.message();
        return false;
    }
    return true;
}

bool StartPkgInstallJob(const Config &cfg, const std::string &url, const std::string &path, std::string *err) {
    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        if (g_job_running) {
            if (err) *err = "a PKG install is already in progress";
            return false;
        }
        g_job_running = true;
        g_job = PkgJobStatus{};
        g_job.phase = url.empty() ? PkgJobPhase::kInstalling : PkgJobPhase::kDownloading;
    }

    StartBackgroundThread([cfg, url, path]() {
        try {
            std::string local_path = path;

            // Stream the remote package straight to the installer: nothing is staged,
            // so it only needs room for the installed game.
            if (!url.empty()) {
                std::string stream_uri, stream_err;
                if (StartRemotePkgStream(url, &stream_uri, &stream_err)) {
                    {
                        std::lock_guard<std::mutex> lock(g_job_mu);
                        g_job.phase = PkgJobPhase::kInstalling;
                    }
                    PkgInstallResult streamed = InstallPkgUri(cfg, stream_uri);
                    if (streamed.ok) {
                        std::lock_guard<std::mutex> lock(g_job_mu);
                        g_job.phase = PkgJobPhase::kQueued;
                        g_job.content_id = streamed.content_id;
                        g_job_running = false;
                        Notify("Prospero Manager\nInstall queued");
                        return;
                    }
                    LogEvent("Streamed install rejected (" + streamed.error + "); downloading instead");
                    StopPkgStream(stream_uri);
                } else {
                    LogEvent("Package cannot be streamed (" + stream_err + "); downloading instead");
                }
                {
                    std::lock_guard<std::mutex> lock(g_job_mu);
                    g_job.phase = PkgJobPhase::kDownloading;
                }
            }

            if (!url.empty()) {
                std::string derr;
                // Moving window, not since the job started: DNS, TLS and redirects move
                // no bytes and would drag the average under the real rate.
                auto window_at = std::chrono::steady_clock::now();
                long long window_loaded = 0;
                double speed = 0;
                bool ok = PkgDownloadToTemp(cfg, url, &local_path, &derr, [&](long long loaded, long long total) {
                    auto now = std::chrono::steady_clock::now();
                    double span = std::chrono::duration<double>(now - window_at).count();
                    if (span >= 0.5) {
                        double sample = (loaded - window_loaded) / span;
                        speed = speed > 0 ? (speed * 0.6 + sample * 0.4) : sample;
                        window_at = now;
                        window_loaded = loaded;
                    }
                    std::lock_guard<std::mutex> lock(g_job_mu);
                    g_job.loaded = loaded;
                    g_job.total = total;
                    g_job.speed_bps = speed;
                });
                if (!ok) {
                    std::lock_guard<std::mutex> lock(g_job_mu);
                    g_job.phase = PkgJobPhase::kError;
                    g_job.error = "download failed: " + derr;
                    g_job_running = false;
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_job_mu);
                g_job.phase = PkgJobPhase::kInstalling;
                if (IsManagedPkgFile(cfg, local_path)) g_job.staged_path = local_path;
            }

            std::string content_id;
            std::string validation_error;
            if (!ValidatePkgHeader(local_path, &content_id, &validation_error)) {
                RemoveManagedPkgFile(cfg, local_path);
                std::lock_guard<std::mutex> lock(g_job_mu);
                g_job.phase = PkgJobPhase::kError;
                g_job.error = validation_error;
                g_job_running = false;
                return;
            }

            // Name the package the way its own metadata does — a content ID means
            // nothing to the person watching the install.
            PkgMetadata meta = ReadPkgMetadata(local_path);
            std::string title = !meta.title_name.empty() ? meta.title_name
                                : !content_id.empty()    ? content_id
                                                         : PkgDisplayName(local_path);
            {
                std::lock_guard<std::mutex> lock(g_job_mu);
                g_job.title = title;
            }
            Notify("Prospero Manager\nInstalling: " + title);
            PkgInstallResult r = InstallPkgFile(cfg, local_path);

            {
                std::lock_guard<std::mutex> lock(g_job_mu);
                if (r.ok) {
                    g_job.phase = PkgJobPhase::kQueued;
                    g_job.content_id = r.content_id;
                } else {
                    g_job.phase = PkgJobPhase::kError;
                    g_job.error = r.error;
                }
                g_job_running = false;
            }
            // Sony's notification service must not hold the status lock;
            // progress polling stays responsive while the toast is sent.
            if (r.ok) Notify("Prospero Manager\nInstall queued: " + title);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = PkgJobPhase::kError;
            g_job.error = std::string("PKG worker failed: ") + e.what();
            g_job_running = false;
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = PkgJobPhase::kError;
            g_job.error = "PKG worker failed unexpectedly";
            g_job_running = false;
        }
    });

    return true;
}

namespace {

constexpr uint16_t kPkgHelperPort = 9042;
constexpr const char *kPkgHelperName = "prospero-pkgd.elf";

struct HelperAttempt {
    bool queued = false;
    std::string response;
    std::string error;
};

std::string AppInstVisiblePath(const std::string &path) {
    // The installer service has a different mount namespace from payloads.
    // Its view of payload /data/foo is /user/data/foo.
    if (path.rfind("/data/", 0) == 0) return "/user" + path;
    return path;
}

bool ConnectPkgHelper(int *out_fd) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPkgHelperPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }
    *out_fd = fd;
    return true;
}

void StopPkgHelper() {
    for (const auto &process : ListProcesses()) {
        if (process.name != kPkgHelperName) continue;
        std::string ignored;
        (void)KillProcess(process.pid, &ignored);
    }
}

bool ExchangePkgHelperCommand(const std::string &command, int timeout_seconds, std::string *response,
                              std::string *err, bool *connected = nullptr) {
    if (connected) *connected = false;
    int fd = -1;
    if (!ConnectPkgHelper(&fd)) {
        if (err) *err = "private PKG service is unreachable";
        return false;
    }
    if (connected) *connected = true;
    timeval timeout{timeout_seconds, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::string request = command + "\n";
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t count = send(fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            close(fd);
            if (err) *err = "failed to send package request to private service";
            return false;
        }
        sent += static_cast<size_t>(count);
    }

    response->clear();
    char buffer[256];
    for (;;) {
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            close(fd);
            if (err) *err = "private PKG service timed out";
            return false;
        }
        response->append(buffer, static_cast<size_t>(count));
        if (response->size() > 1024) {
            close(fd);
            if (err) *err = "private PKG service returned an invalid response";
            return false;
        }
    }
    close(fd);
    if (!response->empty()) return true;
    if (err) *err = "private PKG service returned no result";
    return false;
}

bool WaitForPkgHelper(std::chrono::steady_clock::time_point deadline, std::string *status, std::string *err) {
    std::string last_status;
    bool listener_seen = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string response;
        std::string exchange_error;
        bool connected = false;
        if (ExchangePkgHelperCommand("status", 2, &response, &exchange_error, &connected)) {
            listener_seen = true;
            last_status = response;
            if (response.rfind("ready", 0) == 0) {
                if (status) *status = std::move(response);
                return true;
            }
            if (response.rfind("error:", 0) == 0) {
                if (err) *err = std::move(response);
                return false;
            }
        }
        listener_seen = listener_seen || connected;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (err) {
        if (!last_status.empty()) {
            *err = "private PKG service remained in " + last_status + " for 45 seconds";
        } else if (listener_seen) {
            *err = "private PKG service listened but did not answer status within 45 seconds";
        } else {
            *err = "private PKG service did not start listening within 45 seconds";
        }
    }
    return false;
}

HelperAttempt QueueWithPkgHelper(const Config &cfg, const std::string &uri) {
    HelperAttempt attempt;
    std::string status;
    std::string probe_error;
    std::string probe_response;
    bool probe_connected = false;
    if (ExchangePkgHelperCommand("status", 2, &probe_response, &probe_error, &probe_connected)) {
        if (probe_response.rfind("ready", 0) == 0) {
            status = std::move(probe_response);
        } else if (probe_response.rfind("error:", 0) == 0) {
            attempt.error = std::move(probe_response);
            return attempt;
        } else if (!WaitForPkgHelper(std::chrono::steady_clock::now() + std::chrono::seconds(45), &status,
                                     &attempt.error)) {
            return attempt;
        }
    } else if (probe_connected) {
        attempt.error = "an existing private PKG service accepted the status connection but did not answer: " +
                        probe_error;
        return attempt;
    } else {
        std::string helper_path = cfg.bundled_dir + "/" + kPkgHelperName;
        ElfldrClient loader(Config::kElfldrHost, Config::kElfldrPort);
        std::string launch_error;
        if (!loader.LaunchFile(helper_path, &launch_error)) {
            attempt.error = "could not launch private PKG service: " + launch_error;
            return attempt;
        }
        LogEvent("Private PKG service launch sent; waiting for initialization");
        if (!WaitForPkgHelper(std::chrono::steady_clock::now() + std::chrono::seconds(45), &status,
                              &attempt.error)) {
            return attempt;
        }
    }

    std::string exchange_error;
    if (!ExchangePkgHelperCommand(uri, 45, &attempt.response, &exchange_error)) {
        attempt.error = std::move(exchange_error);
        return attempt;
    }
    if (attempt.response.rfind("ok", 0) == 0) {
        attempt.queued = true;
        return attempt;
    }
    attempt.error = attempt.response;
    return attempt;
}

// Parses "install-status:<status>:<hex error>" from status-install:<content_id>.
// False when the helper couldn't answer at all (treated as "don't know yet").
bool QueryHelperInstallStatus(const std::string &content_id, std::string *status, int *error_code) {
    std::string response, error;
    if (!ExchangePkgHelperCommand("status-install:" + content_id, 10, &response, &error)) return false;
    constexpr char kPrefix[] = "install-status:";
    if (response.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    size_t at = sizeof(kPrefix) - 1;
    size_t colon = response.find(':', at);
    if (colon == std::string::npos) return false;
    *status = response.substr(at, colon - at);
    *error_code = static_cast<int>(std::strtol(response.c_str() + colon + 1, nullptr, 16));
    return true;
}

// The console keeps reading from the RAM cache for minutes after "queued" — polls real
// install status instead of guessing from idle time, with a ceiling so it can't hang forever.
void MonitorLiveInstall(const std::string &content_id, const std::string &uri) {
    constexpr int kPollSeconds = 2;
    constexpr int kSettleSeconds = 3; // avoids trusting a stale status right at stream-done
    constexpr int kMaxMinutes = 90;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(kMaxMinutes);
    bool stream_done = false;
    std::chrono::steady_clock::time_point stream_done_at{};

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(kPollSeconds));
        PkgLiveStatus live = PkgLiveCache::GetStatus();
        if (!live.active || live.aborted) break;

        if (!stream_done && live.total > 0 && live.served >= live.total) {
            stream_done = true;
            stream_done_at = std::chrono::steady_clock::now();
            LogEvent("[pkglive] stream fully delivered; waiting for the system to finalize the install");
        }

        std::string status;
        int error_code = 0;
        bool have_status = !content_id.empty() && QueryHelperInstallStatus(content_id, &status, &error_code);
        if (have_status && (status == "error" || error_code != 0)) {
            LogEvent("[pkglive] system reported an install error; releasing the live cache");
            break;
        }
        if (have_status && stream_done && (status == "playable" || status == "completed") &&
            std::chrono::steady_clock::now() - stream_done_at >= std::chrono::seconds(kSettleSeconds)) {
            LogEvent("[pkglive] system install finished; releasing the live cache");
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            LogEvent("[pkglive] gave up waiting for install completion after " + std::to_string(kMaxMinutes) +
                      " minutes; releasing the live cache");
            break;
        }
    }
    StopPkgStream(uri);
    PkgLiveCache::Abort();
    PkgLiveCache::Destroy();
}

// Sony's installer refuses some packages outright, so point at the console's own
// installer rather than leaving the raw code on screen.
std::string PkgRejectionHint(const std::string &helper_error) {
    if (helper_error.find("0x80b2116f") == std::string::npos) return "";
    return " — the console's package service rejected this package. The download was kept, so you can install it "
           "from Settings > System > Debug Settings > Game > Package Installer";
}

} // namespace

PkgInstallResult InstallPkgUri(const Config &cfg, const std::string &uri) {
    PkgInstallResult r;
    HelperAttempt helper = QueueWithPkgHelper(cfg, uri);
    if (!helper.queued) {
        r.error = "the PKG service could not queue the install (" + helper.error + ")" +
                  PkgRejectionHint(helper.error);
        return r;
    }
    r.ok = true;
    size_t colon = helper.response.find(':');
    if (colon != std::string::npos && colon + 1 < helper.response.size()) {
        r.content_id = helper.response.substr(colon + 1);
    }
    LogEvent("PKG accepted as a streamed URL");
    return r;
}

PkgInstallResult InstallPkgFile(const Config &cfg, const std::string &path) {
    PkgInstallResult r;

    std::string content_id;
    if (!ValidatePkgHeader(path, &content_id, &r.error)) return r;

    // Offer the package over the local range server first. If the installer takes a
    // URI, a remote package can later stream instead of being staged whole.
    std::string uri, stream_err;
    HelperAttempt helper;
    bool streamed = StartPkgStream(path, &uri, &stream_err);
    if (streamed) {
        helper = QueueWithPkgHelper(cfg, uri);
        if (!helper.queued) {
            LogEvent("PKG stream URI rejected (" + helper.error + "); using the file path instead");
            StopPkgStream(uri);
            streamed = false;
        }
    }
    if (!streamed) helper = QueueWithPkgHelper(cfg, AppInstVisiblePath(path));

    if (!helper.queued) {
        r.error = "the PKG service could not queue the install (" + helper.error + ")" + PkgRejectionHint(helper.error);
        return r;
    }

    r.ok = true;
    r.content_id = content_id;
    size_t colon = helper.response.find(':');
    if (colon != std::string::npos && colon + 1 < helper.response.size()) {
        std::string reported = helper.response.substr(colon + 1);
        if (!reported.empty()) r.content_id = std::move(reported);
    }
    LogEvent(streamed ? "PKG accepted from the local stream server"
                      : "PKG accepted by the private elevated package service");
    return r;
}

bool StartPkgInstallJobFromLiveSession(const Config &cfg, const std::string &session_id, std::string *err) {
    {
        std::lock_guard<std::mutex> lock(g_job_mu);
        if (g_job_running) {
            if (err) *err = "a PKG install is already in progress";
            return false;
        }
        g_job_running = true;
        g_job = PkgJobStatus{};
        g_job.phase = PkgJobPhase::kInstalling;
    }

    StartBackgroundThread([cfg, session_id]() {
        try {
            std::string uri, stream_err;
            if (!StartLivePkgStream(session_id, &uri, &stream_err)) {
                std::lock_guard<std::mutex> lock(g_job_mu);
                g_job.phase = PkgJobPhase::kError;
                g_job.error = "could not start the live stream: " + stream_err;
                g_job_running = false;
                return;
            }

            PkgInstallResult r = InstallPkgUri(cfg, uri);
            {
                std::lock_guard<std::mutex> lock(g_job_mu);
                if (r.ok) {
                    g_job.phase = PkgJobPhase::kQueued;
                    g_job.content_id = r.content_id;
                } else {
                    g_job.phase = PkgJobPhase::kError;
                    g_job.error = r.error;
                }
                g_job_running = false;
            }
            if (!r.ok) {
                StopPkgStream(uri);
                PkgLiveCache::Abort();
                PkgLiveCache::Destroy();
                return;
            }
            // Sony's notification service must not hold the status lock; progress
            // polling stays responsive while the toast is sent.
            Notify("Prospero Manager\nInstall queued");
            MonitorLiveInstall(r.content_id, uri);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = PkgJobPhase::kError;
            g_job.error = std::string("PKG worker failed: ") + e.what();
            g_job_running = false;
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_job_mu);
            g_job.phase = PkgJobPhase::kError;
            g_job.error = "PKG worker failed unexpectedly";
            g_job_running = false;
        }
    });

    return true;
}

void CancelLivePkgInstall() {
    PkgLiveCache::Abort();
    PkgLiveCache::Destroy();
}

void ResetPkgInstallHelper() { StopPkgHelper(); }

} // namespace prospero
