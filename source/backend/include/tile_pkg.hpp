#pragma once
// Auto-installs the embedded home-screen tile PKG (deeplinks to
// 127.0.0.1:7070) the first time ProsperoMgr.elf runs. See pkg-source/.

#include "config.hpp"

namespace prospero {

void InstallTilePkgIfNeeded(const Config &cfg);

} // namespace prospero
