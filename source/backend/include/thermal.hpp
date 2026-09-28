#pragma once
// CPU/SoC temperature reads and fan-threshold control. Reads are always safe (plain sceKernel
// getters); writing a threshold needs /dev/icc_fan, accessible only when kstuff is loaded.

#include <string>

#include "config.hpp"

namespace prospero {

struct ThermalInfo {
    bool ok = false;
    int cpu_c = -1;
    int soc_c = -1;
    int m2_c = -1;      // -1 if no NVMe present/readable
    int hottest_c = -1; // max of whichever of the above are valid
    int fan_duty_raw = -1;
    int fan_duty_percent = -1;
    int threshold_c = 0; // 0 = nothing pinned (fan running on firmware automatic)
    int min_c = 50;
    int max_c = 85;
    int warm_c = 65; // UI hint
    int hot_c = 80;
};

ThermalInfo GetThermalInfo();

// Pins the fan-on threshold via /dev/icc_fan and re-applies it every 15s (the firmware resets
// overrides on app/game launches). Failure is usually "device not accessible" — kstuff isn't loaded.
bool SetFanThreshold(const Config &cfg, int temp_c, std::string *err);

// Stops re-applying the custom target. ShellCore restores its active profile
// on the next home/game transition.
bool ResetFanToAutomatic(const Config &cfg, std::string *err);

// Starts the watcher thread if a threshold was already pinned in a previous
// session (persisted to disk) — call once at boot so a pin survives a redeploy.
void RestorePinnedFanThreshold(const Config &cfg);

} // namespace prospero
