#pragma once
// Save-data backup — lists raw save-image files under /user/home and zips them for
// download. Never mounts/decrypts (needs a privileged flow we don't have); copies opaque bytes only.

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

struct SaveEntry {
    std::string user_id_hex;
    std::string title_id;
    std::string title_name; // save database fallback when app.db has no installed-title row
    std::string filename;
    std::string path; // absolute path, safe to hand back to a download route
    std::string icon_path; // save-specific metadata icon, when the PS5 kept one
    long size_bytes = 0;
    bool is_ps4 = false; // true for the savedata/sdimg_* (PS4/BC) convention
    // The PS4 sealed key travelling with its image. Backups and exports need it,
    // but it isn't a save the user picks, so listings leave it out.
    bool is_sealed_key = false;
};

std::vector<SaveEntry> ListSaves();

// Resolves the icon saved beside a user's encrypted save container — separate from the
// installed game's app icon, and survives more move/uninstall cases, so the Saves tab prefers it.
std::string SaveIconPath(const std::string &user_id_hex, const std::string &title_id,
                         const std::string &filename);

// Every path must come from ListSaves(), never from client input. Entries are named
// the way a restore expects, so an exported zip imports back onto the same paths.
bool ZipSavesToFile(const std::vector<std::string> &paths, const std::string &out_path, std::string *err);

// ---- Native auto-backup: wait for a title to close, settle, then back it up locally —
// same raw-bytes copy as the manual download, kept under <root>/save_backups/<title_id>/.

struct SaveBackupConfig {
    bool enabled = false;
    int settle_seconds = 15;
    std::string backup_root; // empty = default <root>/save_backups; otherwise replaces it entirely
};

SaveBackupConfig GetSaveBackupConfig(const Config &cfg);
bool SetSaveBackupConfig(const Config &cfg, const SaveBackupConfig &config, std::string *err);

struct SaveBackupEntry {
    std::string title_id;
    std::string title_name;
    std::string platform;
    std::string filename; // the zip's own filename, e.g. "20260819-141502.zip"
    std::string path;      // absolute path, for download/restore
    uint64_t size_bytes = 0;
    int64_t created_unix = 0;
};

std::vector<SaveBackupEntry> ListSaveBackups(const Config &cfg);

// True if `path` is a real file directly under <root>/save_backups/ — every
// route that takes a backup path from the client must check this first.
bool IsSaveBackupPath(const Config &cfg, const std::string &path);

// Zips every save file on disk for `title_id` (all local users), publishes it atomically
// under save_backups/, and prunes older backups beyond the last 5.
bool BackupTitleSaves(const Config &cfg, const std::string &title_id, std::string *err);

// Refuses while the title is running, snapshots the current save first, and applies all
// files or none. `snapshot_first` false drops that snapshot, and with it the undo.
bool RestoreSaveBackup(const Config &cfg, const std::string &backup_path, std::string *err,
                       std::string *code = nullptr, bool snapshot_first = true);

// Deletes every save file for one user and title, after backing them up. Refuses while
// the title is running, and never removes an image without its sealed key.
bool DeleteTitleSaves(const Config &cfg, const std::string &user_id_hex, const std::string &title_id,
                      std::string *err, std::string *code = nullptr);

// ---- Long save operations share one background slot: they write the same files, so
// two at once is never right, and the activity panel shows whichever is running.

enum class SaveJobKind { kNone, kImport, kRestore, kBackupAll };
enum class SaveJobPhase { kIdle, kRunning, kDone, kError };

struct SaveJobStatus {
    SaveJobKind kind = SaveJobKind::kNone;
    SaveJobPhase phase = SaveJobPhase::kIdle;
    std::string label;      // uploaded filename, title name, or the title being backed up
    int done = 0;           // back-up-all only
    int total = 0;          // back-up-all only
    int failed = 0;         // back-up-all only
    std::string error;
    std::string error_code; // so the UI can translate `error` instead of showing it raw
};

SaveJobStatus GetSaveJobStatus();

// Takes ownership of `zip_path` — the file is removed once the import finishes.
bool StartSaveImportJob(const Config &cfg, const std::string &zip_path, const std::string &label,
                        std::string *err);

bool StartSaveRestoreJob(const Config &cfg, const std::string &backup_path, const std::string &label,
                         bool snapshot_first, std::string *err);

// Background thread: detects a title closing (was running, now isn't or switched), waits
// the settle window, then calls BackupTitleSaves() if enabled. Safe to call once at startup either way.
void StartSaveAutoBackupWatcher(const Config &cfg);

struct SaveBackupAllStatus {
    bool running = false;
    int done = 0;
    int total = 0;
    int failed = 0;
    std::string current_title_id;
};

SaveBackupAllStatus GetSaveBackupAllStatus();

// Backs up every title with save data on disk, one at a time, in the background — the
// one-touch "back up everything now" action. Reports busy via the returned bool if already running.
bool StartBackupAllJob(const Config &cfg, std::string *err);

} // namespace prospero
