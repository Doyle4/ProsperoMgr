#pragma once
// Offline account activation and sign-in state, via registry + dat file writes —
// no private signing key needed. Every write goes through BackupAccountSlot() first.

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace prospero {

struct AccountSlot {
    int slot = 0; // 1..16, a local PS5 user account index — not a PSN account_id
    std::string name;    // empty if this slot has no local account
    std::string id_hex;  // current account_id, "0x0" if never activated
    std::string type;    // e.g. "np" once activated, empty otherwise
    int flags = 0;
    bool is_foreground = false; // this slot is the currently signed-in local user
    bool has_backup = false;
};

std::vector<AccountSlot> ListAccountSlots(const Config &cfg);

// FNV-style hash fallback for when no account_id is set yet — a stable
// suggestion, not a requirement; the caller can pass any hex id.
uint64_t GenerateAccountId(const std::string &name);

// Snapshots the slot's registry state so RestoreAccountSlot can undo it.
// One backup per slot — a second apply overwrites the previous one.
bool BackupAccountSlot(const Config &cfg, int slot, std::string *err);

// Writes account_id (hex, e.g. "0x1a2b3c") and account_type "np" via
// registry, nothing more. Doesn't touch sign-in state or NP files. Backs up first.
bool ApplyOfflineActivation(const Config &cfg, int slot, const std::string &account_id_hex, std::string *err);

// Fuller sign-in flow: activates if needed, then writes auth.dat/config.dat plus the
// sign-in registry fields. Foreground user only — NP files go to its real home path.
bool ApplySignIn(const Config &cfg, const std::string &account_id_hex, std::string *err);

bool RestoreAccountSlot(const Config &cfg, int slot, std::string *err);

} // namespace prospero
