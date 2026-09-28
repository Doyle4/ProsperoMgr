#pragma once
// Minimal INI reader. Keeps repeated keys within a section, in file order —
// autoloader.ini's [sequence] relies on that for its load/delay ordering.

#include <string>
#include <vector>

namespace prospero {

struct IniEntry {
    std::string section; // "" for keys before any [section] header
    std::string key;
    std::string value;
};

class Ini {
  public:
    // A missing file just yields an empty Ini, not an error.
    static Ini Load(const std::string &path);

    std::string GetString(const std::string &section, const std::string &key,
                           const std::string &default_value = "") const;
    int GetInt(const std::string &section, const std::string &key, int default_value = 0) const;
    bool GetBool(const std::string &section, const std::string &key, bool default_value = false) const;

    // All entries in `section`, in file order (repeated keys included).
    std::vector<IniEntry> Entries(const std::string &section) const;

  private:
    std::vector<IniEntry> entries_;
};

} // namespace prospero
