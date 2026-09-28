#include "ini.hpp"

#include <cstdlib>
#include <fstream>

namespace prospero {

namespace {

std::string Trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

Ini Ini::Load(const std::string &path) {
    Ini ini;

    std::ifstream f(path);
    if (!f.is_open()) return ini;

    std::string line;
    std::string section;

    while (std::getline(f, line)) {
        // A bare '#' or ';' starts a comment; values here never need one.
        size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line = line.substr(0, comment);

        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        IniEntry e;
        e.section = section;
        e.key = Trim(line.substr(0, eq));
        e.value = Trim(line.substr(eq + 1));
        if (!e.key.empty()) ini.entries_.push_back(std::move(e));
    }

    return ini;
}

std::string Ini::GetString(const std::string &section, const std::string &key,
                            const std::string &default_value) const {
    for (const auto &e : entries_) {
        if (e.section == section && e.key == key) return e.value;
    }
    return default_value;
}

int Ini::GetInt(const std::string &section, const std::string &key, int default_value) const {
    std::string v = GetString(section, key, "");
    if (v.empty()) return default_value;
    return std::atoi(v.c_str());
}

bool Ini::GetBool(const std::string &section, const std::string &key, bool default_value) const {
    std::string v = GetString(section, key, "");
    if (v.empty()) return default_value;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::vector<IniEntry> Ini::Entries(const std::string &section) const {
    std::vector<IniEntry> out;
    for (const auto &e : entries_) {
        if (e.section == section) out.push_back(e);
    }
    return out;
}

} // namespace prospero
