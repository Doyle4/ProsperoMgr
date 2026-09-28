#pragma once
// Embedded home-screen tile PKG, zlib-compressed at build time — see tools/gen_tile_pkg.py.
// bundled_tile_pkg.cpp is generated, don't hand-edit; tile_pkg.cpp inflates it at install time.

namespace prospero {

extern const unsigned char kTilePkg[];     // zlib-compressed bytes
extern const unsigned int kTilePkgSize;    // compressed size
extern const unsigned int kTilePkgRawSize; // decompressed size

} // namespace prospero
