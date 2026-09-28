#pragma once
// Talks to a GitHub/Gitea repo's releases API directly — no custom index, no Prospero-hosted
// mirror. Used by the Payloads tab's direct-URL install job and the catalog's version badge.

#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

class PayloadRegistry; // payload_registry.hpp — only used by pointer here

enum class RepositoryProvider { kGitHub, kGitea };

// A parsed, normalized repository URL.
struct RepoRef {
    RepositoryProvider provider = RepositoryProvider::kGitHub;
    std::string host;
    std::string owner;
    std::string repo;
};

// Parses "https://github.com/owner/repo" plus trailing slash/".git"/"/releases" variants.
// Requires https:// — only validates URL shape, never touches the network.
bool ParseRepositoryUrl(const std::string &url, RepoRef *out, std::string *err);

// The newest non-draft, non-prerelease release tag for a repo URL.
struct LatestReleaseResult {
    std::string tag;    // e.g. "v0.18" — empty if none found
    // "ok" | "no_releases" | "unreachable" | "invalid_url"
    std::string state;
    std::string error;  // user-facing reason, empty when state == "ok"
};
LatestReleaseResult FetchLatestReleaseTag(const std::string &repo_url);

// One public release with the payload asset a download would install.
struct ReleaseEntry {
    std::string tag;
    std::string published_at; // "YYYY-MM-DD", empty if not reported
    std::string asset_name;
    std::string asset_url;
    uint64_t asset_size = 0;
    bool zip_only = false;
    bool prerelease = false;
};

struct ReleaseListResult {
    std::vector<ReleaseEntry> releases; // newest first, as the host returns them
    // "ok" | "no_releases" | "unreachable" | "invalid_url"
    std::string state;
    std::string error;
};
ReleaseListResult FetchReleases(const std::string &repo_url);

// ---- Download state, exposed to the UI progress/status endpoint -----------
// The transfer runs in a detached worker while the UI polls this state.

enum class DownloadJobPhase { kIdle, kDownloading, kInstalling, kDone, kError };

struct DownloadJobStatus {
    DownloadJobPhase phase = DownloadJobPhase::kIdle;
    long long loaded = 0;
    long long total = -1; // -1 = unknown (no Content-Length)
    double speed_bps = 0;
    std::string filename;
    bool launched = false;
    std::string error;
};

DownloadJobStatus GetDownloadJobStatus();

// ---- Optional GitHub token, raises api.github.com's unauthenticated
// 60/hour rate limit to 5000/hour. Never returned back to callers once set.

void InitializeGithubToken(const Config &cfg);
bool HasGithubToken();
bool SetGithubToken(const std::string &token, std::string *err);
void ClearGithubToken();

// Empty filename/checksum/version means derive from the URL with no checksum — the direct-URL
// case. `repo_url` links the download back to its catalog entry for later version checks.
bool StartPayloadDownloadJob(const Config &cfg, const std::string &url, const std::string &filename,
                              const std::string &checksum, const std::string &version, bool launch_after,
                              PayloadRegistry *registry, std::string *err, const std::string &repo_url = "");

} // namespace prospero
