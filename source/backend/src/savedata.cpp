#include "job_queue.hpp"
#include "savedata.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "archive_paths.hpp"
#include "log.hpp"
#include "miniz.h"
#include "notify.hpp"
#include "sqlite3.h"
#include "titles.hpp"

namespace fs = std::filesystem;

namespace prospero {

namespace {

constexpr const char *kHomeDir = "/user/home";

long FileSize(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : 0;
}

bool IsRegularNonEmptyFile(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool IsSafePathSegment(const std::string &value) {
    if (value.empty() || value.size() > 255) return false;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return value != "." && value != "..";
}

bool IsSafeFilename(const std::string &value) {
    return !value.empty() && value.size() <= 255 && value != "." && value != ".." &&
           value.find('/') == std::string::npos && value.find('\\') == std::string::npos;
}

std::string SaveDirectoryName(std::string filename) {
    if (filename.ends_with(".bin")) filename.resize(filename.size() - 4);
    if (filename.rfind("sdimg_", 0) == 0) filename.erase(0, 6);
    return filename;
}

std::string ReadSaveDatabaseTitle(const SaveEntry &entry) {
    const char *kind = entry.is_ps4 ? "savedata" : "savedata_prospero";
    std::string db_path = std::string("/system_data/") + kind + "/" + entry.user_id_hex +
                          "/db/user/savedata.db";
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return "";
    }
    sqlite3_busy_timeout(db, 1000);

    const char *sql =
        "SELECT main_title FROM savedata WHERE title_id=?1 AND main_title IS NOT NULL AND main_title!='' "
        "ORDER BY CASE WHEN dir_name=?2 THEN 0 ELSE 1 END LIMIT 1";
    sqlite3_stmt *stmt = nullptr;
    std::string title;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        const std::string dir_name = SaveDirectoryName(entry.filename);
        sqlite3_bind_text(stmt, 1, entry.title_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, dir_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(stmt, 0);
            if (value) title = reinterpret_cast<const char *>(value);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return title;
}

// Every entry directly under <user>/savedata_prospero/<title_id>/ is a PS5
// save image — no fixed naming convention to filter on, unlike PS4's sdimg_*.
void ScanPs5Saves(const std::string &user_hex, const std::string &title_dir, const std::string &title_id,
                   std::vector<SaveEntry> *out) {
    DIR *d = opendir(title_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        std::string path = title_dir + "/" + ent->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        SaveEntry e;
        e.user_id_hex = user_hex;
        e.title_id = title_id;
        e.filename = ent->d_name;
        e.path = path;
        e.size_bytes = static_cast<long>(st.st_size);
        e.is_ps4 = false;
        out->push_back(std::move(e));
    }
    closedir(d);
}

// PS4/BC saves are the image (sdimg_<name>) plus its sealed key (<name>.bin).
// Both are listed, so a backup carries the pair.
void ScanPs4Saves(const std::string &user_hex, const std::string &title_dir, const std::string &title_id,
                   std::vector<SaveEntry> *out) {
    DIR *d = opendir(title_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name.rfind("sdimg_", 0) != 0) continue;
        if (name.ends_with(".bin")) continue; // never treat a key as an image

        std::string path = title_dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        SaveEntry e;
        e.user_id_hex = user_hex;
        e.title_id = title_id;
        e.filename = name;
        e.path = path;
        e.size_bytes = static_cast<long>(st.st_size);
        e.is_ps4 = true;
        out->push_back(std::move(e));

        // The sealed key travels with the image — without it the image is unreadable.
        // Console saves drop the sdimg_ prefix.
        for (const std::string &key_name : {name.substr(6) + ".bin", name + ".bin"}) {
            std::string key_path = title_dir + "/" + key_name;
            long key_size = FileSize(key_path);
            if (key_size <= 0) continue;
            SaveEntry key;
            key.user_id_hex = user_hex;
            key.title_id = title_id;
            key.filename = key_name;
            key.path = key_path;
            key.size_bytes = key_size;
            key.is_ps4 = true;
            key.is_sealed_key = true;
            out->push_back(std::move(key));
        }
    }
    closedir(d);
}

void ScanTitles(const std::string &user_hex, const std::string &container_dir, bool is_ps4,
                 std::vector<SaveEntry> *out) {
    DIR *d = opendir(container_dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        std::string title_dir = container_dir + "/" + ent->d_name;
        struct stat st;
        if (stat(title_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (is_ps4) {
            ScanPs4Saves(user_hex, title_dir, ent->d_name, out);
        } else {
            ScanPs5Saves(user_hex, title_dir, ent->d_name, out);
        }
    }
    closedir(d);
}

} // namespace

std::vector<SaveEntry> ListSaves() {
    std::vector<SaveEntry> out;

    DIR *home = opendir(kHomeDir);
    if (!home) return out;

    struct dirent *ent;
    while ((ent = readdir(home))) {
        if (ent->d_name[0] == '.') continue;
        std::string user_dir = std::string(kHomeDir) + "/" + ent->d_name;
        struct stat st;
        if (stat(user_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        ScanTitles(ent->d_name, user_dir + "/savedata_prospero", false, &out);
        ScanTitles(ent->d_name, user_dir + "/savedata", true, &out);
    }
    closedir(home);

    // main_title lives in the per-user save database, not app.db. One lookup per
    // user/title/platform, attached to every container file.
    std::map<std::string, std::string> title_cache;
    for (auto &entry : out) {
        const std::string key = entry.user_id_hex + (entry.is_ps4 ? ":ps4:" : ":ps5:") + entry.title_id;
        auto it = title_cache.find(key);
        if (it == title_cache.end()) {
            it = title_cache.emplace(key, ReadSaveDatabaseTitle(entry)).first;
        }
        entry.title_name = it->second;
        entry.icon_path = SaveIconPath(entry.user_id_hex, entry.title_id, entry.filename);
    }
    return out;
}

std::string SaveIconPath(const std::string &user_id_hex, const std::string &title_id,
                         const std::string &filename) {
    if (!IsSafePathSegment(user_id_hex) || !IsSafePathSegment(title_id) || !IsSafeFilename(filename)) {
        return "";
    }

    const std::string dir_name = SaveDirectoryName(filename);
    static const char *const kMetaRoots[] = {
        "savedata_prospero_meta", // PS5 native saves and imported ones alike
        "savedata_meta",          // older/PS4 metadata layouts
        nullptr,
    };
    for (int i = 0; kMetaRoots[i]; i++) {
        const std::string dir = "/user/home/" + user_id_hex + "/" + kMetaRoots[i] +
                                "/user/" + title_id;
        const std::string exact = dir + "/" + dir_name + "_icon0.png";
        if (IsRegularNonEmptyFile(exact)) return exact;

        // Some firmware/import combinations rename the save directory but keep the
        // title-level icon, which still beats an empty square.
        DIR *meta = opendir(dir.c_str());
        if (!meta) continue;
        std::string fallback;
        while (dirent *item = readdir(meta)) {
            std::string name = item->d_name;
            if (name.size() < 10 || !name.ends_with("_icon0.png")) continue;
            std::string candidate = dir + "/" + name;
            if (IsRegularNonEmptyFile(candidate)) {
                fallback = std::move(candidate);
                break;
            }
        }
        closedir(meta);
        if (!fallback.empty()) return fallback;
    }
    return "";
}

namespace {

// Save images are already encrypted, so deflate burns CPU for no gain. Storing them
// turns a backup into a plain copy.
constexpr mz_uint kSaveZipLevel = MZ_NO_COMPRESSION;

// One layout for exports and backups: the path under /user/home, which is what a
// restore prepends back. Anything else imports to the wrong place.
std::string SaveArchiveName(const std::string &path) {
    const std::string prefix = std::string(kHomeDir) + "/";
    return path.rfind(prefix, 0) == 0 ? path.substr(prefix.size()) : path;
}

} // namespace

bool ZipSavesToFile(const std::vector<std::string> &paths, const std::string &out_path, std::string *err) {
    if (paths.empty()) {
        if (err) *err = "no files selected";
        return false;
    }

    // Straight to disk, never a heap archive: a save image is far too big to hold
    // a second copy of in this payload's address space.
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_path.c_str(), 0)) {
        if (err) *err = "zip init failed";
        return false;
    }

    bool ok = true;
    for (const auto &path : paths) {
        if (!mz_zip_writer_add_file(&zip, SaveArchiveName(path).c_str(), path.c_str(), nullptr, 0,
                                    kSaveZipLevel)) {
            ok = false;
            break;
        }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);

    if (!ok) {
        std::error_code ec;
        fs::remove(out_path, ec);
        if (err) *err = "failed to add a save file to the zip";
        return false;
    }
    return true;
}

// ---- Native auto-backup -------------------------------------------------

namespace {

std::string DefaultBackupRoot(const Config &cfg) { return cfg.root_dir + "/save_backups"; }
std::string BackupConfigPath(const Config &cfg) { return cfg.root_dir + "/save_backup_config.txt"; }

// Configured destination if the user picked one (e.g. a USB path), otherwise the default.
std::string BackupRoot(const Config &cfg) {
    std::string configured = GetSaveBackupConfig(cfg).backup_root;
    return configured.empty() ? DefaultBackupRoot(cfg) : configured;
}

// Same PPSA/CUSA prefix convention titles.cpp uses — duplicated here rather than exported,
// since it's one line and the only other place that needs it.
std::string PlatformFromTitleId(const std::string &title_id) {
    if (title_id.rfind("PPSA", 0) == 0) return "PS5";
    if (title_id.rfind("CUSA", 0) == 0) return "PS4";
    return "APP";
}

std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Keep only the newest 5 backups for a title — an unattended watcher
// shouldn't grow disk usage without bound.
void PruneOldBackups(const std::string &title_dir, size_t keep) {
    std::error_code ec;
    std::vector<fs::path> zips;
    for (const auto &entry : fs::directory_iterator(title_dir, ec)) {
        if (entry.path().extension() == ".zip") zips.push_back(entry.path());
    }
    if (zips.size() <= keep) return;
    std::sort(zips.begin(), zips.end()); // timestamp-named, so lexical order is chronological
    for (size_t i = 0; i + keep < zips.size(); i++) fs::remove(zips[i], ec);
}

} // namespace

SaveBackupConfig GetSaveBackupConfig(const Config &cfg) {
    SaveBackupConfig out;
    std::ifstream in(BackupConfigPath(cfg));
    if (!in.is_open()) return out;
    std::string enabled_line;
    std::getline(in, enabled_line);
    in >> out.settle_seconds;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::getline(in, out.backup_root);
    out.enabled = enabled_line == "1";
    if (out.settle_seconds < 3) out.settle_seconds = 3;
    return out;
}

bool SetSaveBackupConfig(const Config &cfg, const SaveBackupConfig &config, std::string *err) {
    if (!config.backup_root.empty() && config.backup_root[0] != '/') {
        if (err) *err = "backup destination must be an absolute path";
        return false;
    }
    std::ofstream out(BackupConfigPath(cfg), std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "could not write save backup config";
        return false;
    }
    out << (config.enabled ? "1" : "0") << '\n' << std::max(3, config.settle_seconds) << '\n' << config.backup_root
        << '\n';
    return true;
}

bool IsSaveBackupPath(const Config &cfg, const std::string &path) {
    std::error_code ec;
    fs::path normalized = fs::path(path).lexically_normal();
    std::string root = fs::path(BackupRoot(cfg)).lexically_normal().string() + "/";
    if (normalized.string().rfind(root, 0) != 0) return false;
    return fs::is_regular_file(normalized, ec);
}

std::vector<SaveBackupEntry> ListSaveBackups(const Config &cfg) {
    std::vector<SaveBackupEntry> out;
    std::error_code ec;
    if (!fs::is_directory(BackupRoot(cfg), ec)) return out;

    auto titles = ListTitles(cfg);
    std::map<std::string, std::string> title_names;
    for (const auto &t : titles) title_names[t.title_id] = t.name;

    for (const auto &title_dir : fs::directory_iterator(BackupRoot(cfg), ec)) {
        if (!title_dir.is_directory()) continue;
        std::string title_id = title_dir.path().filename().string();
        for (const auto &zip_entry : fs::directory_iterator(title_dir.path(), ec)) {
            if (zip_entry.path().extension() != ".zip") continue;
            struct stat st {};
            if (stat(zip_entry.path().c_str(), &st) != 0) continue;

            SaveBackupEntry b;
            b.title_id = title_id;
            auto it = title_names.find(title_id);
            b.title_name = it != title_names.end() ? it->second : title_id;
            b.platform = PlatformFromTitleId(title_id);
            b.filename = zip_entry.path().filename().string();
            b.path = zip_entry.path().string();
            b.size_bytes = static_cast<uint64_t>(st.st_size);
            b.created_unix = static_cast<int64_t>(st.st_mtime);
            out.push_back(std::move(b));
        }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.created_unix > b.created_unix; });
    return out;
}

bool BackupTitleSaves(const Config &cfg, const std::string &title_id, std::string *err) {
    std::vector<SaveEntry> matching;
    for (auto &entry : ListSaves()) {
        if (entry.title_id == title_id) matching.push_back(std::move(entry));
    }
    if (matching.empty()) {
        if (err) *err = "no save data found for that title";
        return false;
    }

    std::error_code ec;
    std::string title_dir = BackupRoot(cfg) + "/" + title_id;
    fs::create_directories(title_dir, ec);

    std::string final_path = title_dir + "/" + Timestamp() + ".zip";
    std::string staged_path = final_path + ".tmp";

    std::vector<std::string> paths;
    paths.reserve(matching.size());
    for (const auto &entry : matching) paths.push_back(entry.path);

    std::string zip_err;
    if (!ZipSavesToFile(paths, staged_path, &zip_err)) {
        if (err) *err = zip_err;
        return false;
    }

    fs::rename(staged_path, final_path, ec);
    if (ec) {
        fs::remove(staged_path, ec);
        if (err) *err = "could not publish backup file: " + ec.message();
        return false;
    }

    PruneOldBackups(title_dir, 5);
    return true;
}

namespace {

constexpr const char *kRestoreTempSuffix = ".pmgr-restore";

// A PS4 image is unreadable without its sealed key, so a backup that lost one
// would restore a save the console rejects as corrupt.
bool PlanHasEveryKey(const std::vector<std::pair<mz_uint, fs::path>> &plan, std::string *err) {
    std::set<std::string> names;
    for (const auto &item : plan) names.insert(item.second.string());
    for (const auto &item : plan) {
        std::string file = item.second.filename().string();
        if (file.rfind("sdimg_", 0) != 0 || file.ends_with(".bin")) continue;
        std::string dir = item.second.parent_path().string() + "/";
        if (names.count(dir + file.substr(6) + ".bin") || names.count(dir + file + ".bin")) continue;
        if (err) *err = "backup is incomplete: " + file + " has no sealed key";
        return false;
    }
    return true;
}

} // namespace

bool RestoreSaveBackup(const Config &cfg, const std::string &backup_path, std::string *err,
                       std::string *code, bool snapshot_first) {
    auto fail = [&](const char *reason, const std::string &message) {
        if (code) *code = reason;
        if (err) *err = message;
        return false;
    };

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, backup_path.c_str(), 0)) {
        return fail("invalid_zip", "could not open backup — is it valid?");
    }

    // Validate every entry before writing anything, so a backup we would reject
    // never gets part-way applied.
    const fs::path home_root(kHomeDir);
    std::vector<std::pair<mz_uint, fs::path>> plan;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat_info;
        if (!mz_zip_reader_file_stat(&zip, i, &stat_info)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        // Every entry name is "<hex>/savedata.../<title>/<file>" — same
        // ZIP Slip guard the general file-manager unzip uses (archive_paths.hpp).
        fs::path relative;
        std::string path_err;
        if (!SafeArchiveRelativePath(stat_info.m_filename, &relative, &path_err)) {
            mz_zip_reader_end(&zip);
            return fail("unsafe_path", "backup contains an unsafe path");
        }
        fs::path normalized = (home_root / relative).lexically_normal();
        if (!IsWithinRoot(home_root, normalized)) {
            mz_zip_reader_end(&zip);
            return fail("unsafe_path", "backup contains an unsafe path");
        }
        plan.emplace_back(i, normalized);
    }
    if (plan.empty()) {
        mz_zip_reader_end(&zip);
        return fail("no_files", "backup contains no save files");
    }
    if (!PlanHasEveryKey(plan, err)) {
        mz_zip_reader_end(&zip);
        if (code) *code = "missing_key";
        return false;
    }

    // Taken from the entries, not the file's own location, so an uploaded save
    // is identified the same way a stored backup is.
    std::string title_id = plan.front().second.parent_path().filename().string();

    // The console holds a running title's save open; replacing it underneath is
    // how a save gets corrupted.
    if (!title_id.empty() && GetRunningTitleId() == title_id) {
        mz_zip_reader_end(&zip);
        return fail("title_running", "close the game before restoring its save");
    }

    // Snapshot what is on the console now, so a bad restore can be undone.
    bool has_existing = false;
    if (snapshot_first) {
        for (const auto &entry : ListSaves()) {
            if (entry.title_id == title_id) { has_existing = true; break; }
        }
    }
    if (has_existing) {
        std::string snapshot_err;
        if (!BackupTitleSaves(cfg, title_id, &snapshot_err)) {
            mz_zip_reader_end(&zip);
            return fail("snapshot_failed", "could not back up the current save first: " + snapshot_err);
        }
    }

    // Unpack beside each target first. A failure here leaves the existing save
    // untouched rather than half-replaced.
    std::error_code ec;
    bool ok = true;
    for (const auto &item : plan) {
        fs::create_directories(item.second.parent_path(), ec);
        std::string temp = item.second.string() + kRestoreTempSuffix;
        if (!mz_zip_reader_extract_to_file(&zip, item.first, temp.c_str(), 0)) {
            ok = false;
            if (code) *code = "unpack_failed";
            if (err) *err = "failed to unpack " + item.second.filename().string();
            break;
        }
        chmod(temp.c_str(), 0666);
    }
    mz_zip_reader_end(&zip);

    if (ok) {
        for (const auto &item : plan) {
            std::string temp = item.second.string() + kRestoreTempSuffix;
            if (std::rename(temp.c_str(), item.second.c_str()) != 0) {
                ok = false;
                if (code) *code = "replace_failed";
                if (err) *err = "failed to replace " + item.second.filename().string();
                break;
            }
        }
    }
    for (const auto &item : plan) std::remove((item.second.string() + kRestoreTempSuffix).c_str());
    return ok;
}

bool DeleteTitleSaves(const Config &cfg, const std::string &user_id_hex, const std::string &title_id,
                      std::string *err, std::string *code) {
    auto fail = [&](const char *reason, const std::string &message) {
        if (code) *code = reason;
        if (err) *err = message;
        return false;
    };

    // Deleting a save the console still has open corrupts what's left of it.
    if (GetRunningTitleId() == title_id) {
        return fail("title_running", "close the game before deleting its save");
    }

    // Only paths ListSaves() found itself, never anything the client sent.
    std::vector<std::string> paths;
    for (const auto &entry : ListSaves()) {
        if (entry.user_id_hex == user_id_hex && entry.title_id == title_id) paths.push_back(entry.path);
    }
    if (paths.empty()) {
        return fail("no_saves", "no save data found for that user and title");
    }

    // Deleting is the one action with no undo, so keep a copy first.
    std::string backup_err;
    if (!BackupTitleSaves(cfg, title_id, &backup_err)) {
        return fail("snapshot_failed", "could not back up the save before deleting it: " + backup_err);
    }

    for (const auto &path : paths) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            return fail("delete_failed", "could not delete " + fs::path(path).filename().string());
        }
    }
    LogEvent("Deleted save data for " + title_id + " (backed up first)");
    return true;
}

namespace {

std::mutex g_save_job_mu;
SaveJobStatus g_save_job;

// One slot for import, restore and back-up-all alike: they all rewrite the same
// files, so a second one starting mid-flight is never right.
bool BeginSaveJob(SaveJobKind kind, const std::string &label, std::string *err) {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    if (g_save_job.phase == SaveJobPhase::kRunning) {
        if (err) *err = "another save operation is already in progress";
        return false;
    }
    g_save_job = SaveJobStatus{};
    g_save_job.kind = kind;
    g_save_job.phase = SaveJobPhase::kRunning;
    g_save_job.label = label;
    return true;
}

void FinishSaveJob(bool ok, const std::string &error, const std::string &code) {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    g_save_job.phase = ok ? SaveJobPhase::kDone : SaveJobPhase::kError;
    g_save_job.error = ok ? "" : error;
    g_save_job.error_code = ok ? "" : code;
}

} // namespace

SaveJobStatus GetSaveJobStatus() {
    std::lock_guard<std::mutex> lock(g_save_job_mu);
    return g_save_job;
}

bool StartSaveImportJob(const Config &cfg, const std::string &zip_path, const std::string &label,
                        std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kImport, label, err)) return false;

    StartBackgroundThread([cfg, zip_path] {
        std::string import_err, import_code;
        bool ok = RestoreSaveBackup(cfg, zip_path, &import_err, &import_code);
        std::remove(zip_path.c_str());
        FinishSaveJob(ok, import_err, import_code);
        // Sony's notification service must not hold the status lock, or polling
        // stalls behind the toast.
        if (ok) Notify("Prospero Manager\nSave data imported");
    });
    return true;
}

bool StartSaveRestoreJob(const Config &cfg, const std::string &backup_path, const std::string &label,
                         bool snapshot_first, std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kRestore, label, err)) return false;

    StartBackgroundThread([cfg, backup_path, snapshot_first] {
        std::string restore_err, restore_code;
        bool ok = RestoreSaveBackup(cfg, backup_path, &restore_err, &restore_code, snapshot_first);
        FinishSaveJob(ok, restore_err, restore_code);
        if (ok) Notify("Prospero Manager\nSave data restored");
    });
    return true;
}

void StartSaveAutoBackupWatcher(const Config &cfg) {
    StartBackgroundThread([cfg] {
        std::string previous_title;
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> pending;

        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            try {
                std::string current = GetRunningTitleId();
                if (current != previous_title) {
                    if (!previous_title.empty()) pending.emplace_back(previous_title, std::chrono::steady_clock::now());
                    previous_title = current;
                }
                if (pending.empty()) continue;

                SaveBackupConfig conf = GetSaveBackupConfig(cfg);
                auto now = std::chrono::steady_clock::now();
                for (auto it = pending.begin(); it != pending.end();) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                    if (elapsed < conf.settle_seconds) {
                        ++it;
                        continue;
                    }
                    if (conf.enabled) {
                        std::string err;
                        if (BackupTitleSaves(cfg, it->first, &err)) {
                            LogEvent("Auto-backed up saves for " + it->first);
                        } else {
                            LogEvent("Auto-backup skipped for " + it->first + ": " + err);
                            // Worth a toast, not just a log line — a silent failure here means the
                            // user thinks their saves are safely backed up (e.g. to USB) when they aren't.
                            NotifyDebug("Prospero Manager: Auto-backup failed for " + it->first + "\n" + err);
                        }
                    }
                    it = pending.erase(it);
                }
            } catch (const std::exception &e) {
                LogEvent(std::string("Save auto-backup watcher failed safely: ") + e.what());
            } catch (...) {
                LogEvent("Save auto-backup watcher failed safely");
            }
        }
    });
}

// Kept so the Saves panel's own progress row keeps working — it reads the same
// slot the activity dock does.
SaveBackupAllStatus GetSaveBackupAllStatus() {
    SaveJobStatus job = GetSaveJobStatus();
    SaveBackupAllStatus out;
    if (job.kind != SaveJobKind::kBackupAll) return out;
    out.running = job.phase == SaveJobPhase::kRunning;
    out.done = job.done;
    out.total = job.total;
    out.failed = job.failed;
    out.current_title_id = out.running ? job.label : "";
    return out;
}

bool StartBackupAllJob(const Config &cfg, std::string *err) {
    if (!BeginSaveJob(SaveJobKind::kBackupAll, "", err)) return false;

    StartBackgroundThread([cfg] {
        std::set<std::string> title_ids;
        for (const auto &entry : ListSaves()) title_ids.insert(entry.title_id);
        {
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.total = static_cast<int>(title_ids.size());
        }

        for (const auto &title_id : title_ids) {
            {
                std::lock_guard<std::mutex> lock(g_save_job_mu);
                g_save_job.label = title_id;
            }
            std::string title_err;
            bool ok = BackupTitleSaves(cfg, title_id, &title_err);
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.done++;
            if (!ok) {
                g_save_job.failed++;
                LogEvent("Back up all: skipped " + title_id + ": " + title_err);
            }
        }

        int done = 0, total = 0, failed = 0;
        {
            std::lock_guard<std::mutex> lock(g_save_job_mu);
            g_save_job.label.clear();
            done = g_save_job.done;
            total = g_save_job.total;
            failed = g_save_job.failed;
        }
        FinishSaveJob(true, "", "");
        LogEvent("Back up all: finished (" + std::to_string(done - failed) + "/" + std::to_string(total) +
                 " succeeded)");
    });
    return true;
}

} // namespace prospero
