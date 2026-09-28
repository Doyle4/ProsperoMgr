#include "process_manager.hpp"

#include <signal.h>
#include <unistd.h>

#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace prospero {

std::vector<ProcessInfo> ListProcesses() {
    std::vector<ProcessInfo> out;

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t len = 0;
    if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0) return out;

    std::vector<char> buf(len);
    if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) != 0) return out;

    for (char *p = buf.data(); p < buf.data() + len;) {
        auto *ki = reinterpret_cast<struct kinfo_proc *>(p);
        if (ki->ki_structsize == 0) break;

        ProcessInfo info;
        info.pid = ki->ki_pid;
        info.ppid = ki->ki_ppid;
        info.name = ki->ki_comm;
        out.push_back(std::move(info));

        p += ki->ki_structsize;
    }

    return out;
}

bool KillProcess(int32_t pid, std::string *err) {
    if (kill(static_cast<pid_t>(pid), SIGKILL) != 0) {
        if (err) *err = "kill() failed for pid " + std::to_string(pid);
        return false;
    }
    return true;
}

std::string GetBootId() {
    struct timeval boottime {};
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, nullptr, 0) != 0 || len < sizeof(boottime)) return "";
    // Both fields, not just tv_sec — a fast reboot can land on the same second and
    // this is meant to be an exact boot identity.
    return std::to_string(static_cast<long long>(boottime.tv_sec)) + "." +
           std::to_string(static_cast<long long>(boottime.tv_usec));
}

namespace {
std::string StemPayloadName(const std::string &name) {
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char *ext : {".elf", ".bin", ".self"}) {
        size_t ext_len = std::strlen(ext);
        if (lowered.size() >= ext_len && lowered.compare(lowered.size() - ext_len, ext_len, ext) == 0) {
            lowered.resize(lowered.size() - ext_len);
            break;
        }
    }
    return lowered;
}
} // namespace

bool IsPayloadResident(const std::string &payload_name, const std::vector<ProcessInfo> &processes) {
    std::string wanted = StemPayloadName(payload_name);
    if (wanted.empty()) return false;
    for (const auto &p : processes) {
        std::string stem = StemPayloadName(p.name);
        // Exact match, or a truncated prefix from ki_comm's cap. Length-gated so a
        // short fragment can't false-match.
        if (stem.size() >= 3 && (stem == wanted || wanted.compare(0, stem.size(), stem) == 0)) return true;
    }
    return false;
}

} // namespace prospero
