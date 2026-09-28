#include "job_queue.hpp"
#include "thermal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include "fan_profile.hpp"
#include "log.hpp"
#include "ps5_services.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace prospero {

namespace {

std::string PinPath(const Config &cfg) { return cfg.root_dir + "/fan_pinned.txt"; }

void SavePinned(const Config &cfg, int temp_c) {
    std::ofstream f(PinPath(cfg), std::ios::trunc);
    if (f.is_open()) f << temp_c;
}

int LoadPinned(const Config &cfg) {
    std::ifstream f(PinPath(cfg));
    int v = 0;
    if (f.is_open()) f >> v;
    return v;
}

std::atomic<int> g_pinned_c{0}; // 0 = nothing pinned
std::atomic<bool> g_watcher_started{false};
std::mutex g_controller_mu;

constexpr const char *kIccFanDevice = "/dev/icc_fan";
constexpr unsigned long kIccFanSetAutoservo = 0xC01C8F07ul;
constexpr unsigned long kIccFanGetAutoservo = 0xC01C8F08ul;
constexpr int kFanMinC = 50;
constexpr int kFanMaxC = 85;
constexpr int kFanReapplySec = 15;
constexpr int kFanMaxConsecutiveFailures = 3;

void SetDeviceError(std::string *err, const char *operation, int eno) {
    if (!err) return;
    *err = std::string("/dev/icc_fan ") + operation + " failed (errno=" +
           std::to_string(eno) + " " + std::strerror(eno) + ")";
}

bool ReadAutoservo(int fd, FanAutoservoProfile *profile, std::string *err) {
    profile->fill(0);
    if (ioctl(fd, kIccFanGetAutoservo, profile->data()) == 0) return true;
    SetDeviceError(err, "read", errno);
    return false;
}

bool ApplyThresholdLocked(int temp_c, std::string *err) {
    int fd = open(kIccFanDevice, O_RDWR);
    if (fd < 0) {
        SetDeviceError(err, "open", errno);
        return false;
    }

    FanAutoservoProfile current{};
    if (!ReadAutoservo(fd, &current, err)) {
        close(fd);
        return false;
    }
    if (FanTargetMatches(current, temp_c)) {
        close(fd);
        return true;
    }

    FanAutoservoProfile request{};
    BuildFanTargetProfile(current, temp_c, &request);
    if (ioctl(fd, kIccFanSetAutoservo, request.data()) != 0) {
        int eno = errno;
        close(fd);
        SetDeviceError(err, "write", eno);
        return false;
    }

    FanAutoservoProfile verified{};
    if (!ReadAutoservo(fd, &verified, err)) {
        close(fd);
        return false;
    }
    close(fd);

    if (!FanTargetMatches(verified, temp_c)) {
        if (err) *err = "/dev/icc_fan rejected the requested target";
        return false;
    }
    return true;
}

void FanWatcherLoop(Config cfg) {
    int consecutive_failures = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(kFanReapplySec));

        int t = g_pinned_c.load();
        if (t < kFanMinC || t > kFanMaxC) {
            consecutive_failures = 0;
            continue;
        }

        std::string err;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(g_controller_mu);
            if (g_pinned_c.load() != t) continue;
            ok = ApplyThresholdLocked(t, &err);
            if (!ok && ++consecutive_failures >= kFanMaxConsecutiveFailures) {
                g_pinned_c.store(0);
                SavePinned(cfg, 0);
            }
        }
        if (ok) {
            consecutive_failures = 0;
        } else if (consecutive_failures >= kFanMaxConsecutiveFailures) {
            LogEvent("Fan threshold disabled after repeated controller errors: " + err);
        }
    }
}

void EnsureWatcher(const Config &cfg) {
    bool expected = false;
    if (!g_watcher_started.compare_exchange_strong(expected, true)) return;
    StartBackgroundThread([cfg] { FanWatcherLoop(cfg); });
}

} // namespace

ThermalInfo GetThermalInfo() {
    ThermalInfo t;

    int cpu = -1;
    bool cpu_ok = sceKernelGetCpuTemperature(&cpu) == 0 && cpu >= 0 && cpu <= 130;
    int soc = -1;
    bool soc_ok = sceKernelGetSocSensorTemperature(0, &soc) == 0 && soc >= 0 && soc <= 130;

    // Channel 2 is the M.2 NVMe sensor on most revisions — an empty slot
    // reads ~0, so require a plausible >=20C before trusting it.
    int m2 = -1;
    bool m2_ok = false;
    {
        int v = -1;
        if (sceKernelGetSocSensorTemperature(2, &v) == 0 && v >= 20 && v <= 130) {
            m2 = v;
            m2_ok = true;
        }
    }

    t.ok = cpu_ok || soc_ok;
    if (cpu_ok) t.cpu_c = cpu;
    if (soc_ok) t.soc_c = soc;
    if (m2_ok) t.m2_c = m2;

    uint16_t duty = 0;
    uint64_t chassis = 0;
    if (sceKernelGetCurrentFanDuty(&duty, &chassis) == 0) {
        t.fan_duty_raw = static_cast<int>(duty);
        t.fan_duty_percent = std::min(100, (static_cast<int>(duty) * 100 + 512) / 1024);
    }

    int hottest = -1;
    if (cpu_ok && cpu > hottest) hottest = cpu;
    if (soc_ok && soc > hottest) hottest = soc;
    if (m2_ok && m2 > hottest) hottest = m2;
    t.hottest_c = hottest;

    t.threshold_c = g_pinned_c.load();
    return t;
}

bool SetFanThreshold(const Config &cfg, int temp_c, std::string *err) {
    if (temp_c < kFanMinC || temp_c > kFanMaxC) {
        if (err) *err = "temp must be " + std::to_string(kFanMinC) + ".." + std::to_string(kFanMaxC) + " Celsius";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_controller_mu);
        if (!ApplyThresholdLocked(temp_c, err)) return false;
        g_pinned_c.store(temp_c);
        SavePinned(cfg, temp_c);
    }
    EnsureWatcher(cfg);
    return true;
}

bool ResetFanToAutomatic(const Config &cfg, std::string *err) {
    std::lock_guard<std::mutex> lock(g_controller_mu);
    g_pinned_c.store(0);
    SavePinned(cfg, 0);
    (void)err;
    return true;
}

void RestorePinnedFanThreshold(const Config &cfg) {
    int saved = LoadPinned(cfg);
    if (saved < kFanMinC || saved > kFanMaxC) return;
    {
        std::lock_guard<std::mutex> lock(g_controller_mu);
        g_pinned_c.store(saved);
        std::string ignored;
        ApplyThresholdLocked(saved, &ignored);
    }
    EnsureWatcher(cfg);
}

} // namespace prospero
