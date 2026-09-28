#pragma once
// A developer-grouped directory of PS5 homebrew payloads for the Payload Repositories
// tab — built-in (shipped, fixed) or custom (user-added). Just links out; see repository.hpp for installs.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.hpp"

namespace prospero {

struct PayloadCatalogEntry {
    std::string developer;
    std::string name;
    std::string description;
    std::string url;
    bool custom = false; // false = shipped with Prospero, true = user-added
};

// Loads custom_repositories.json under cfg.root_dir. Call once at startup,
// before any other function in this file — mirrors InitializeFileJobs(cfg).
void InitializePayloadCatalog(const Config &cfg);

// Built-in entries followed by any custom entries, in that order —
// grouping/sorting by developer happens on the frontend.
std::vector<PayloadCatalogEntry> GetPayloadCatalog();

// Parses/normalizes `url`, rejects duplicates, derives the developer group from the built-in
// catalog's owner aliases, and best-effort fetches upstream metadata — never fails just because that's unreachable.
bool AddCustomRepository(const std::string &url, PayloadCatalogEntry *out_entry, std::string *err);
// Only ever removes a *custom* entry — a built-in URL is rejected.
bool RemoveCustomRepository(const std::string &url, std::string *err);

struct CatalogVersionStatus {
    std::string version; // latest non-draft/non-prerelease release tag, empty if unknown
    // "ok" | "no_releases" | "unreachable" | "invalid_url" | "" (not checked yet)
    std::string state;
    std::string error;
    int64_t checked_at_unix = 0; // 0 = never checked
};

struct CatalogRefreshStatus {
    bool running = false;
    bool have_cache = false;
    int completed = 0;
    int total = 0;
};

// Scans every catalog entry's latest release in the background and returns immediately;
// poll GetCatalogRefreshStatus()/GetCachedCatalogVersions(). User-triggered only — burns GitHub's 60 req/hour.
bool StartCatalogRefreshJob(bool force, std::string *err);

// Checks one entry and returns once it has an answer. This is what the
// per-entry refresh button calls, so it costs exactly one upstream request.
CatalogVersionStatus RefreshCatalogEntry(const std::string &url);
CatalogRefreshStatus GetCatalogRefreshStatus();
// Keyed by PayloadCatalogEntry::url.
std::unordered_map<std::string, CatalogVersionStatus> GetCachedCatalogVersions();

} // namespace prospero
