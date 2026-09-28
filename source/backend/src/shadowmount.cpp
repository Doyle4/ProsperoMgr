#include "shadowmount.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace prospero {
namespace {

constexpr const char *kConfigPath = "/data/shadowmount/config.ini";

std::string Trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// True for an uncommented "scanpath=<value>" line (case-insensitive key).
bool IsActiveScanpathLine(const std::string &trimmed, std::string *value) {
    if (trimmed.empty() || trimmed.front() == '#') return false;
    size_t eq = trimmed.find('=');
    if (eq == std::string::npos) return false;
    std::string key = Trim(trimmed.substr(0, eq));
    for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key != "scanpath") return false;
    if (value) *value = Trim(trimmed.substr(eq + 1));
    return true;
}

bool IsSafeAbsolutePath(const std::string &path) {
    if (path.empty() || path.front() != '/' || path.size() > 255) return false;
    if (path.find("..") != std::string::npos) return false;
    return true;
}

std::vector<std::string> ReadAllLines() {
    std::vector<std::string> lines;
    std::ifstream in(kConfigPath);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

} // namespace

std::vector<std::string> ReadShadowMountScanPaths() {
    std::vector<std::string> out;
    for (const auto &line : ReadAllLines()) {
        std::string value;
        if (IsActiveScanpathLine(Trim(line), &value) && !value.empty()) out.push_back(value);
    }
    return out;
}

bool WriteShadowMountScanPaths(const std::vector<std::string> &scan_paths, std::string *err) {
    for (const auto &path : scan_paths) {
        if (!IsSafeAbsolutePath(path)) {
            if (err) *err = "invalid scan path: " + path;
            return false;
        }
    }

    std::vector<std::string> lines = ReadAllLines();
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](const std::string &line) { return IsActiveScanpathLine(Trim(line), nullptr); }),
               lines.end());
    for (const auto &path : scan_paths) lines.push_back("scanpath=" + path);

    std::error_code ec;
    fs::create_directories(fs::path(kConfigPath).parent_path(), ec);

    std::string tmp = std::string(kConfigPath) + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
        if (err) *err = "could not write ShadowMountPlus config";
        return false;
    }
    for (const auto &line : lines) out << line << '\n';
    out.flush();
    out.close();
    if (!out) {
        if (err) *err = "could not flush ShadowMountPlus config";
        return false;
    }
    if (std::rename(tmp.c_str(), kConfigPath) != 0) {
        if (err) *err = "could not commit ShadowMountPlus config";
        return false;
    }
    return true;
}

} // namespace prospero
