#pragma once
// Lists running processes and kills one by PID. Real listing only works on
// the PS5/FreeBSD target (sysctl KERN_PROC); host builds return an empty list.

#include <cstdint>
#include <string>
#include <vector>

namespace prospero {

struct ProcessInfo {
    int32_t pid = 0;
    int32_t ppid = 0;
    std::string name;
};

std::vector<ProcessInfo> ListProcesses();

// Sends SIGKILL. Returns false + fills `err` on failure.
bool KillProcess(int32_t pid, std::string *err);

// Stable identity for the current PS5 boot (kern.boottime) — lets state survive a
// redeployed ELF but not a real reboot. Empty if unavailable; treat that as unknown, don't guess.
std::string GetBootId();

// Same loose match PayloadsView.jsx does client-side, for the Autoloader. ki_comm (FreeBSD's
// process name) truncates ~16-19 chars, so this allows an exact match or truncated-prefix match only.
bool IsPayloadResident(const std::string &payload_name, const std::vector<ProcessInfo> &processes);

} // namespace prospero
