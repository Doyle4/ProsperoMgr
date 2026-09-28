#include "titles.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

#include "https_client.hpp"
#include "json_writer.hpp"
#include "ps5_services.hpp"

#include "sqlite3.h"

namespace prospero {

namespace {
bool IsSafeTitleId(const std::string &title_id) {
    if (title_id.size() < 5 || title_id.size() > 32) return false;
    for (unsigned char c : title_id) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

bool IsNonEmptyFile(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
} // namespace

std::string TitleIconPath(const std::string &title_id) {
    if (!IsSafeTitleId(title_id)) return "";

    static std::mutex cache_mu;
    static std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        auto found = cache.find(title_id);
        if (found != cache.end() && IsNonEmptyFile(found->second)) return found->second;
    }

    std::string result;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2("/system_data/priv/mms/app.db", &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        const char *sql =
            "SELECT icon0Info FROM tbl_contentinfo WHERE titleId=?1 AND icon0Info IS NOT NULL "
            "AND icon0Info!='' LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *value = sqlite3_column_text(stmt, 0);
                if (value) {
                    result = reinterpret_cast<const char *>(value);
                    size_t end = result.rfind(".png");
                    if (end != std::string::npos) result.resize(end + 4); else result.clear();
                    if (result.rfind("/mnt/ext0/", 0) == 0 || result.rfind("/mnt/ext1/", 0) == 0 ||
                        !IsNonEmptyFile(result)) result.clear();
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(db);
    } else if (db) {
        sqlite3_close(db);
    }

    static const char *const kCandidates[] = {
        "/user/appmeta/%s/icon0.png",
        "/user/appmeta/%s/sce_sys/icon0.png",
        "/user/appmeta/external/%s/icon0.png",
        "/user/appmeta/external/%s/sce_sys/icon0.png",
        "/system_data/priv/appmeta/%s/icon0.png",
        "/system_data/priv/appmeta/%s/sce_sys/icon0.png",
        "/system_data/priv/appmeta/external/%s/icon0.png",
        "/system_data/priv/appmeta/external/%s/sce_sys/icon0.png",
        "/user/app/%s/sce_sys/icon0.png",
        "/user/patch/%s/sce_sys/icon0.png",
        nullptr,
    };
    char path[512];
    if (result.empty()) {
        for (int i = 0; kCandidates[i]; i++) {
            std::snprintf(path, sizeof(path), kCandidates[i], title_id.c_str());
            if (IsNonEmptyFile(path)) { result = path; break; }
        }
    }

    static const char *const kRoots[] = {
        "/user/appmeta", "/user/appmeta/external", "/system_data/priv/appmeta",
        "/system_data/priv/appmeta/external", nullptr,
    };
    for (int i = 0; result.empty() && kRoots[i]; i++) {
        DIR *root = opendir(kRoots[i]);
        if (!root) continue;
        std::string found;
        while (dirent *entry = readdir(root)) {
            if (entry->d_name[0] == '.' || !std::strstr(entry->d_name, title_id.c_str())) continue;
            const std::string base = std::string(kRoots[i]) + "/" + entry->d_name;
            if (IsNonEmptyFile(base + "/icon0.png")) found = base + "/icon0.png";
            else if (IsNonEmptyFile(base + "/sce_sys/icon0.png")) found = base + "/sce_sys/icon0.png";
            if (!found.empty()) break;
        }
        closedir(root);
        if (!found.empty()) result = found;
    }
    if (!result.empty()) {
        std::lock_guard<std::mutex> lock(cache_mu);
        cache[title_id] = result;
    }
    return result;
}

// Sony's own title-id prefix scheme, resolved once here so every
// consumer of ListTitles() gets it.
std::string DerivePlatform(const std::string &title_id) {
    if (title_id.rfind("PPSA", 0) == 0) return "PS5";
    if (title_id.rfind("CUSA", 0) == 0) return "PS4";
    return "APP";
}

namespace {

constexpr const char *kAppDbPath = "/system_data/priv/mms/app.db";

// app.db's tbl_appbrowse.titleName is legitimately blank for a lot of real titles (fpkg/PS4
// especially) — falls back to param.sfo ("\0PSF" magic), the authoritative source every title ships.
uint16_t ReadLe16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool SfoReadString(const std::string &path, const char *key, std::string *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto *data = reinterpret_cast<const uint8_t *>(buf.data());
    size_t len = buf.size();

    if (len < 20 || std::memcmp(data, "\0PSF", 4) != 0) return false;

    uint32_t key_table = ReadLe32(data + 8);
    uint32_t data_table = ReadLe32(data + 12);
    uint32_t count = ReadLe32(data + 16);
    if (key_table >= len || data_table >= len || count > 1024 || 20 + static_cast<uint64_t>(count) * 16 > len) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = data + 20 + i * 16;
        uint16_t key_off = ReadLe16(entry + 0);
        uint32_t data_len = ReadLe32(entry + 4);
        uint32_t data_off = ReadLe32(entry + 12);
        if (key_table + key_off >= len || data_table + data_off >= len) continue;

        const char *k = reinterpret_cast<const char *>(data + key_table + key_off);
        size_t kmax = len - (key_table + key_off);
        if (!memchr(k, '\0', kmax)) continue;
        if (std::strcmp(k, key) != 0) continue;

        size_t avail = len - (data_table + data_off);
        size_t n = data_len;
        if (n > avail) n = avail;
        while (n > 0 && data[data_table + data_off + n - 1] == '\0') n--;
        if (n == 0) return false;

        out->assign(reinterpret_cast<const char *>(data + data_table + data_off), n);
        return true;
    }
    return false;
}

bool IsUnresolvedName(const std::string &title_id, const std::string &name) {
    size_t begin = 0;
    while (begin < name.size() && std::isspace(static_cast<unsigned char>(name[begin]))) begin++;
    size_t end = name.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1]))) end--;
    if (begin == end) return true;

    std::string trimmed = name.substr(begin, end - begin);
    if (trimmed.size() == title_id.size()) {
        bool same = true;
        for (size_t i = 0; i < trimmed.size(); i++) {
            if (std::toupper(static_cast<unsigned char>(trimmed[i])) !=
                std::toupper(static_cast<unsigned char>(title_id[i]))) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return trimmed == "Unknown" || trimmed == "Unknown title";
}

bool ReadSmallTextFile(const std::string &path, std::string *out) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

// Native PS5 applications use param.json, not param.sfo, for localized metadata. `titleName`
// lives below localizedParameters; JsonExtractString() finds that first string in this small, trusted doc.
bool JsonReadValue(const std::string &path, const char *json_key, std::string *out) {
    std::string json;
    if (!ReadSmallTextFile(path, &json)) return false;
    *out = JsonExtractString(json, json_key);
    return !out->empty();
}

bool ReadMetadataDir(const std::string &dir, const char *json_key, const char *sfo_key, std::string *out) {
    if (JsonReadValue(dir + "/param.json", json_key, out) ||
        JsonReadValue(dir + "/sce_sys/param.json", json_key, out) ||
        SfoReadString(dir + "/param.sfo", sfo_key, out) ||
        SfoReadString(dir + "/sce_sys/param.sfo", sfo_key, out)) {
        return !out->empty();
    }
    return false;
}

// External storage and some native installs use a content ID as the appmeta dir name (e.g.
// UP....-PPSA28180_00-...), invisible to a direct /appmeta/PPSA28180 lookup — search children instead.
bool ScanAppMetaForTitle(const std::string &title_id, const char *json_key, const char *sfo_key,
                         std::string *out) {
    static const char *const kBases[] = {"/user/appmeta", "/user/appmeta/external", nullptr};
    for (int i = 0; kBases[i]; i++) {
        DIR *dir = opendir(kBases[i]);
        if (!dir) continue;

        bool found = false;
        while (dirent *entry = readdir(dir)) {
            if (entry->d_name[0] == '.' || !std::strstr(entry->d_name, title_id.c_str())) continue;
            std::string metadata_dir = std::string(kBases[i]) + "/" + entry->d_name;
            if (ReadMetadataDir(metadata_dir, json_key, sfo_key, out)) {
                found = true;
                break;
            }

            std::string sandbox = std::string("/mnt/sandbox/") + entry->d_name + "_000";
            if (ReadMetadataDir(sandbox + "/patch0", json_key, sfo_key, out) ||
                ReadMetadataDir(sandbox + "/app0", json_key, sfo_key, out)) {
                found = true;
                break;
            }
        }
        closedir(dir);
        if (found) return true;
    }
    return false;
}

bool ReadInstalledMetadata(const std::string &title_id, const char *json_key, const char *sfo_key,
                           std::string *out) {
    static const char *const kJsonPaths[] = {
        "/system_data/priv/appmeta/%s/param.json",
        "/system_data/priv/appmeta/external/%s/param.json",
        "/user/appmeta/%s/param.json",
        "/user/appmeta/%s/sce_sys/param.json",
        "/user/appmeta/external/%s/param.json",
        "/user/appmeta/external/%s/sce_sys/param.json",
        "/user/patch/%s/sce_sys/param.json",
        "/user/app/%s/sce_sys/param.json",
        nullptr,
    };
    static const char *const kPaths[] = {
        "/system_data/priv/appmeta/%s/param.sfo",
        "/system_data/priv/appmeta/external/%s/param.sfo",
        "/user/patch/%s/sce_sys/param.sfo",
        "/user/appmeta/%s/param.sfo",
        "/user/appmeta/%s/sce_sys/param.sfo",
        "/user/appmeta/external/%s/param.sfo",
        "/user/appmeta/external/%s/sce_sys/param.sfo",
        "/user/app/%s/sce_sys/param.sfo",
        nullptr,
    };
    char path[512];

    for (int i = 0; kJsonPaths[i]; i++) {
        std::snprintf(path, sizeof(path), kJsonPaths[i], title_id.c_str());
        if (JsonReadValue(path, json_key, out) && !out->empty()) return true;
    }
    for (int i = 0; kPaths[i]; i++) {
        std::snprintf(path, sizeof(path), kPaths[i], title_id.c_str());
        if (SfoReadString(path, sfo_key, out) && !out->empty()) return true;
    }
    return ScanAppMetaForTitle(title_id, json_key, sfo_key, out);
}

bool ReadInstalledTitleName(const std::string &title_id, std::string *out) {
    return ReadInstalledMetadata(title_id, "titleName", "TITLE", out) && !IsUnresolvedName(title_id, *out);
}

bool ReadInstalledContentId(const std::string &title_id, std::string *out) {
    return ReadInstalledMetadata(title_id, "contentId", "CONTENT_ID", out);
}

// Cache dir for names resolved online (see FetchStoreTitleName), so a title
// only ever needs one network round trip, not one per Home-tab load forever after.
std::string TitleCachePath(const Config &cfg, const std::string &title_id) {
    return cfg.root_dir + "/.title_cache/" + title_id + ".txt";
}

bool ReadTitleCache(const Config &cfg, const std::string &title_id, std::string *out) {
    std::ifstream f(TitleCachePath(cfg, title_id));
    if (!f.is_open()) return false;
    std::getline(f, *out);
    return !out->empty();
}

void WriteTitleCache(const Config &cfg, const std::string &title_id, const std::string &name) {
    mkdir((cfg.root_dir + "/.title_cache").c_str(), 0755);
    std::ofstream f(TitleCachePath(cfg, title_id), std::ios::trunc);
    if (f.is_open()) f << name;
}

// Last resort before falling back to just the title ID: an unauthenticated GET of the game's
// public Store page, reading the name out of its embedded JSON-LD block.
bool FetchStoreTitleName(const std::string &content_id, std::string *out) {
    if (content_id.empty()) return false;

    HttpsResponse resp;
    std::string err;
    if (!HttpsClient::Get("https://store.playstation.com/en-us/product/" + content_id, &resp, &err)) return false;

    const std::string open_tag = "<script id=\"mfe-jsonld-tags\" type=\"application/ld+json\">";
    size_t start = resp.body.find(open_tag);
    if (start == std::string::npos) return false;
    start += open_tag.size();

    size_t end = resp.body.find("</script>", start);
    if (end == std::string::npos) return false;

    std::string name = JsonExtractString(resp.body.substr(start, end - start), "name");
    if (name.empty()) return false;
    *out = name;
    return true;
}

bool TableExists(sqlite3 *db, const char *table) {
    const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?1";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool TableHasColumn(sqlite3 *db, const char *table, const char *column) {
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && column == reinterpret_cast<const char *>(name)) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool ReadAppInfoTitleName(sqlite3 *db, const std::string &title_id, std::string *out) {
    if (!TableExists(db, "tbl_appinfo")) return false;

    static const char *const kKeys[] = {
        "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04", "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08",
        "TITLE_09", "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13", "TITLE_14", "TITLE_15", "TITLE_16",
        "TITLE_17", "TITLE_18", "TITLE_19", "TITLE_20", "TITLE_21", "TITLE_22", "TITLE_00", "TITLE", nullptr,
    };
    const char *sql =
        "SELECT val FROM tbl_appinfo WHERE titleId=?1 AND key=?2 AND val IS NOT NULL AND val!='' LIMIT 1";
    for (int i = 0; kKeys[i]; i++) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, title_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, kKeys[i], -1, SQLITE_TRANSIENT);
        bool found = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(stmt, 0);
            if (value) {
                *out = reinterpret_cast<const char *>(value);
                found = !IsUnresolvedName(title_id, *out);
            }
        }
        sqlite3_finalize(stmt);
        if (found) return true;
    }
    return false;
}

void RunTitleQuery(sqlite3 *db, const char *sql, std::vector<TitleInfo> *out) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *tid = sqlite3_column_text(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (!tid) continue;

        TitleInfo info;
        info.title_id = reinterpret_cast<const char *>(tid);
        // COALESCE gives an empty string, not null, so fpkg titles need a
        // length check rather than a pointer check.
        const char *name_str = name ? reinterpret_cast<const char *>(name) : "";
        info.name = *name_str ? name_str : info.title_id;
        info.platform = DerivePlatform(info.title_id);
        info.icon_path = TitleIconPath(info.title_id);
        out->push_back(std::move(info));
    }
    sqlite3_finalize(stmt);
}

struct Activity {
    long played_seconds = 0;
    std::string last_played;
};

// Playtime/last-played live in a per-profile table (tbl_iconinfo_<user hex>) — merge across
// every profile found so a shared console still shows something sensible per title.
void MergeActivity(sqlite3 *db, std::unordered_map<std::string, Activity> *activity) {
    sqlite3_stmt *tables_stmt = nullptr;
    const char *tables_sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'";
    if (sqlite3_prepare_v2(db, tables_sql, -1, &tables_stmt, nullptr) != SQLITE_OK) return;

    std::vector<std::string> tables;
    while (sqlite3_step(tables_stmt) == SQLITE_ROW) {
        const unsigned char *n = sqlite3_column_text(tables_stmt, 0);
        if (n) tables.push_back(reinterpret_cast<const char *>(n));
    }
    sqlite3_finalize(tables_stmt);

    for (const auto &table : tables) {
        if (!TableHasColumn(db, table.c_str(), "playedTimeOnConsole")) continue;

        std::string sql = "SELECT titleId, playedTimeOnConsole, lastAccessTime FROM " + table;
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) continue;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *tid = sqlite3_column_text(stmt, 0);
            if (!tid) continue;
            long played = sqlite3_column_int(stmt, 1);
            const unsigned char *last = sqlite3_column_text(stmt, 2);

            Activity &a = (*activity)[reinterpret_cast<const char *>(tid)];
            if (played > a.played_seconds) a.played_seconds = played;
            if (last && reinterpret_cast<const char *>(last) > a.last_played) {
                a.last_played = reinterpret_cast<const char *>(last);
            }
        }
        sqlite3_finalize(stmt);
    }
}

} // namespace

std::vector<TitleInfo> ListTitles(const Config &cfg) {
    // Home and Save Data can ask for this at the same time; serializing keeps
    // the app.db snapshot and the name cache coherent.
    static std::mutex list_mu;
    std::lock_guard<std::mutex> list_lock(list_mu);

    std::vector<TitleInfo> out;

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(kAppDbPath, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }

    // Real game title IDs only — the same table also holds system services,
    // media apps and store listings.
    constexpr const char *kPrefixFilter =
        " AND (titleId LIKE 'CUSA%' OR titleId LIKE 'PPSA%' OR titleId LIKE 'ULUS%' OR titleId LIKE 'ULES%' OR "
        "titleId LIKE 'ULJS%' OR titleId LIKE 'ULKS%' OR titleId LIKE 'SLUS%' OR titleId LIKE 'SCUS%' OR "
        "titleId LIKE 'SLES%' OR titleId LIKE 'SCES%' OR titleId LIKE 'SLPS%' OR titleId LIKE 'SLPM%' OR "
        "titleId LIKE 'SCED%' OR titleId LIKE 'SLED%' OR titleId LIKE 'SCPS%')";

    // A DB rebuild can drop a PS4 fpkg's browse row, so union both tables and
    // dedup by titleId, keeping whichever name isn't empty.
    bool has_appbrowse = TableExists(db, "tbl_appbrowse");
    bool has_appbrowse_name = has_appbrowse && TableHasColumn(db, "tbl_appbrowse", "titleName");
    bool has_cinfo = TableExists(db, "tbl_contentinfo");
    bool has_cinfo_name = has_cinfo && TableHasColumn(db, "tbl_contentinfo", "titleName");

    std::string sql;
    if (has_appbrowse) {
        std::string ab = std::string("SELECT titleId, ") + (has_appbrowse_name ? "COALESCE(titleName,'')" : "''") +
                          " AS n FROM tbl_appbrowse WHERE titleId IS NOT NULL AND titleId != ''" + kPrefixFilter;
        std::string ci;
        if (has_cinfo) {
            ci = std::string(" UNION ALL SELECT titleId, ") + (has_cinfo_name ? "COALESCE(titleName,'')" : "''") +
                 " AS n FROM tbl_contentinfo WHERE titleId IS NOT NULL AND titleId != ''" + kPrefixFilter;
        }
        sql = "SELECT titleId, MAX(n) AS n FROM (" + ab + ci + ") GROUP BY titleId ORDER BY n COLLATE NOCASE";
    } else if (has_cinfo) {
        sql = std::string("SELECT DISTINCT titleId, ") + (has_cinfo_name ? "COALESCE(titleName,'')" : "''") +
              " AS n FROM tbl_contentinfo WHERE titleId IS NOT NULL AND titleId != ''" + kPrefixFilter +
              " ORDER BY n COLLATE NOCASE";
    }

    if (!sql.empty()) RunTitleQuery(db, sql.c_str(), &out);

    // Resolve localized names while app.db is still open; tbl_appinfo covers
    // rows that carry only a title ID.
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;
        std::string appinfo_name;
        if (ReadAppInfoTitleName(db, t.title_id, &appinfo_name)) t.name = appinfo_name;
    }

    std::unordered_map<std::string, Activity> activity;
    MergeActivity(db, &activity);
    for (auto &t : out) {
        auto it = activity.find(t.title_id);
        if (it == activity.end()) continue;
        t.played_seconds = it->second.played_seconds;
        t.last_played = it->second.last_played;
    }

    sqlite3_close(db);

    // PS5 param.json calls it "contentVersion", PS4 SFO calls it "APP_VER".
    // Patch metadata is read before the base app's, so this reflects patches.
    for (auto &t : out) {
        ReadInstalledMetadata(t.title_id, "contentVersion", "APP_VER", &t.version);
    }

    // app.db had no usable localized name: read native PS5 param.json first,
    // then the PSF/SFO metadata used by PS4 titles and some homebrew.
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;
        std::string local_name;
        if (ReadInstalledTitleName(t.title_id, &local_name)) t.name = local_name;
    }

    // Last resort for content with a blank TITLE: look the name up on the PS
    // Store, cached to disk and capped so one page load can't stack requests.
    int online_lookups_left = 5;
    for (auto &t : out) {
        if (!IsUnresolvedName(t.title_id, t.name)) continue;

        std::string cached;
        if (ReadTitleCache(cfg, t.title_id, &cached)) {
            t.name = cached;
            continue;
        }
        if (online_lookups_left <= 0) continue;

        std::string content_id;
        if (!ReadInstalledContentId(t.title_id, &content_id)) continue;

        std::string store_name;
        online_lookups_left--;
        if (FetchStoreTitleName(content_id, &store_name)) {
            t.name = store_name;
            WriteTitleCache(cfg, t.title_id, store_name);
        }
    }

    return out;
}

bool LaunchTitle(const std::string &title_id, std::string *err) {
    sceUserServiceInitialize(nullptr);

    uint32_t user_id = 0;
    if (sceUserServiceGetForegroundUser(&user_id) != 0 || user_id == 0) {
        if (err) *err = "no foreground user — sign in to a profile first";
        return false;
    }

    AppLaunchCtx ctx{};
    ctx.structsize = sizeof(ctx);
    ctx.user_id = user_id;
    ctx.argv = nullptr;

    int rc = sceSystemServiceLaunchApp(title_id.c_str(), nullptr, &ctx);
    // A success can return the app id rather than zero; SCE errors have the
    // high bit set and arrive negative.
    if (rc < 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "%08x", static_cast<unsigned>(rc));
        if (err) *err = std::string("sceSystemServiceLaunchApp failed (0x") + code + ")";
        return false;
    }
    return true;
}

std::string GetRunningTitleId() {
    int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) return "";

    char title_id[16] = {0};
    if (sceSystemServiceGetAppTitleId(app_id, title_id) != 0) return "";
    return title_id;
}

} // namespace prospero
