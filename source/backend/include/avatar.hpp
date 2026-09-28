#pragma once
// Profile avatar pipeline: decode an uploaded image, build the four DDS sizes the PS5
// shell expects, and (PS5-only) write them into the foreground user's profile cache.

#include <cstdint>
#include <string>

#include "config.hpp"

namespace prospero {

// The four square sizes the PS5 profile cache stores per user.
inline constexpr int kAvatarSizes[] = {64, 128, 260, 440};
inline constexpr int kAvatarSizeCount = 4;

struct AvatarUser {
    bool ok = false;
    uint32_t user_id = 0;
    std::string user_id_hex;
    std::string user_name;
    std::string profile_dir;
    std::string error;
};

// Decodes raw image bytes, center-crops to square, writes DDS + PNG preview pairs for
// every size into the work directory. Pure image work — runs identically on host and PS5.
bool BuildAvatarWork(const Config &cfg, const std::string &image_bytes, std::string *err);

// Path to a size's PNG preview, for the browser to show before applying.
std::string AvatarPreviewPath(const Config &cfg, int size);

// Foreground PS5 user — PS5-only, needed before an apply.
AvatarUser GetAvatarUser();

// Renames the foreground user's PSN display name via sceUserServiceSetUserName.
// Sony caps online IDs at 16 chars; callers should validate before this.
bool SetForegroundUserName(const std::string &name, std::string *err);

// Display name for an arbitrary (not necessarily foreground) local user ID — works for
// every account under /user/home, not just the foreground one. PS5-only; host builds return "".
std::string GetUserDisplayName(uint32_t user_id);

// Path to that user's cached profile picture, if PSN has ever downloaded one. Empty/unreadable
// if none — caller should fall back to a generic avatar. Safe to call on host too.
std::string ProfileIconPath(const std::string &user_id_hex);

// Copies the built work files into that user's profile cache dir. PS5-only.
bool ApplyAvatarToUser(const Config &cfg, const AvatarUser &user, std::string *err);

} // namespace prospero
