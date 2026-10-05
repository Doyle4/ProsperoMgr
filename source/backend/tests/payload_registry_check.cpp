// Host regression check (run from the repository root):
// c++ -std=c++20 -Wall -Wextra -Werror -Isource/backend/include \
//   source/backend/tests/payload_registry_check.cpp source/backend/src/payload_registry.cpp \
//   -o /tmp/prospero-registry-check && /tmp/prospero-registry-check
#include "payload_registry.hpp"
#include "elfldr_client.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
static std::string launched_path;

// Check the file selected for launch without contacting elfldr or a PS5.
namespace prospero {
ElfldrClient::ElfldrClient(std::string, uint16_t) {}
bool ElfldrClient::LaunchFile(const std::string &path, std::string *) const {
    launched_path = path;
    return true;
}
} // namespace prospero

int main() {
    char pattern[] = "/tmp/prospero-registry-XXXXXX";
    const char *dir = mkdtemp(pattern);
    assert(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{dir};
    prospero::Config cfg;
    cfg.payloads_dir = (cleanup.path / "payloads").string();
    cfg.bundled_dir = (cleanup.path / "bundled").string();
    fs::create_directories(cfg.payloads_dir);
    fs::create_directories(cfg.bundled_dir);
    auto write = [](const fs::path &path) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "test payload";
    };
    auto nested = fs::path(cfg.payloads_dir) / "GhostControl (Custom USB)/GhostControl (Custom USB).elf";
    write(nested);
    write(fs::path(cfg.payloads_dir) / "flat.bin");
    write(fs::path(cfg.payloads_dir) / "deep/more/upper.SELF");
    write(fs::path(cfg.payloads_dir) / "GhostControl (Custom USB)/meta.json");
    write(fs::path(cfg.bundled_dir) / "bundled.elf");
    write(fs::path(cfg.bundled_dir) / "prospero-pkgd.elf");
    write(cleanup.path / "outside/hidden.elf");
    fs::create_directory_symlink(cleanup.path / "outside", fs::path(cfg.payloads_dir) / "link");
    fs::create_directory_symlink(cfg.payloads_dir, fs::path(cfg.payloads_dir) / "loop");

    prospero::PayloadRegistry registry(cfg);
    auto items = registry.List();
    assert(items.size() == 4);
    std::string path, err;
    assert(registry.Resolve(nested.filename().string(), &path) && path == nested.string());
    assert(registry.Launch(nested.filename().string(), &err) && launched_path == nested.string());
    assert(!registry.Resolve("meta.json", &path));
    assert(!registry.Resolve("hidden.elf", &path));
    assert(!registry.Resolve("prospero-pkgd.elf", &path));
    assert(registry.SetEnabled(nested.filename().string(), false, &err));
    assert(!registry.Launch(nested.filename().string(), &err));
    assert(registry.SetEnabled(nested.filename().string(), true, &err));
    assert(registry.SetFavorite(nested.filename().string(), true, &err));
    assert(registry.Rename(nested.filename().string(), "renamed.elf", &err));
    auto renamed = nested.parent_path() / "renamed.elf";
    assert(!fs::exists(nested) && fs::exists(renamed));
    assert(registry.Resolve("renamed.elf", &path) && path == renamed.string());
    assert(!registry.Rename("renamed.elf", "flat.bin", &err));
    assert(!registry.Rename("renamed.elf", "../escape.elf", &err));
    assert(!registry.Delete("../escape.elf", &err));
    assert(!registry.Delete("bundled.elf", &err));
    assert(!registry.Rename("bundled.elf", "other.elf", &err));
    assert(fs::exists(fs::path(cfg.bundled_dir) / "bundled.elf"));
    items = registry.List();
    for (const auto &item : items) {
        if (item.name == "renamed.elf") assert(item.favorite && item.launch_count == 1);
    }
    assert(registry.Delete("renamed.elf", &err));
    assert(!fs::exists(renamed) && fs::exists(nested.parent_path() / "meta.json"));

    // Existing flat imports keep precedence over nested and bundled copies.
    write(fs::path(cfg.bundled_dir) / "duplicate.elf");
    auto a = fs::path(cfg.payloads_dir) / "a/duplicate.elf";
    auto z = fs::path(cfg.payloads_dir) / "z/duplicate.elf";
    write(z);
    write(a);
    assert(registry.Import("duplicate.elf", "flat override", &err));
    assert(registry.Resolve("duplicate.elf", &path));
    assert(path == (fs::path(cfg.payloads_dir) / "duplicate.elf").string());
    assert(registry.Delete("duplicate.elf", &err));
    assert(registry.Resolve("duplicate.elf", &path) && path == a.string());
    assert(registry.Delete("duplicate.elf", &err));
    assert(registry.Resolve("duplicate.elf", &path) && path == z.string());
    std::puts("payload registry checks passed");
}
