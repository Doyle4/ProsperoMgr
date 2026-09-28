#pragma once
// Manages ShadowMountPlus's own scanpath= lines in its config.ini.
// Prospero doesn't run or depend on ShadowMountPlus itself.

#include <string>
#include <vector>

namespace prospero {

// Empty means ShadowMountPlus scans its own compile-time default locations.
std::vector<std::string> ReadShadowMountScanPaths();

// Rewrites just the active "scanpath=" lines, leaving every other line
// untouched. An empty list restores ShadowMountPlus's built-in defaults.
bool WriteShadowMountScanPaths(const std::vector<std::string> &scan_paths, std::string *err);

} // namespace prospero
