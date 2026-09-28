#pragma once
// Writes the embedded ftpsrv/klogsrv/etc. payloads to bundled_dir. See
// tools/gen_payloads.py — bundled_payloads.cpp is generated, don't hand-edit it.

#include <string>

namespace prospero {

void InstallBundledPayloads(const std::string &bundled_dir);

} // namespace prospero
