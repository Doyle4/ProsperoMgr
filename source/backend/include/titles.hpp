#pragma once
// Installed PS5 title library — reads app.db (read-only) for the Home page's
// games grid, and launches a title through the system's own app launcher.

#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

struct TitleInfo {
    std::string title_id;
    std::string name;
    std::string platform;     // "PS5" | "PS4" | "APP" — derived from the title-ID prefix
    std::string icon_path;    // absolute path to icon0.png, empty if none found
    long played_seconds = 0;  // 0 if never played / no activity record found
    std::string last_played;  // as app.db stores it (ISO-ish string), empty if unknown
    std::string version;      // installed content version (e.g. "1.034.000"), empty if unknown
};

// `cfg` is only used to place the on-disk title-name cache (see titles.cpp's
// FetchStoreTitleName).
std::vector<TitleInfo> ListTitles(const Config &cfg);

// Resolves a title's icon0.png path for serving as an image response.
std::string TitleIconPath(const std::string &title_id);

bool LaunchTitle(const std::string &title_id, std::string *err);

// Title ID of whatever big-app is in the foreground right now, or empty if none — cheap
// sceSystemService polling with no SQL round-trip, so the Home page can mark the running tile.
std::string GetRunningTitleId();

} // namespace prospero
