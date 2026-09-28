#pragma once

#include <stdint.h>

#define PROSPERO_PKG_SYSTEM_AUTHID UINT64_C(0x4801000000000013)
#define PROSPERO_PKG_SHELLCORE_AUTHID UINT64_C(0x3800000000000010)

typedef struct {
    int major;
    int minor;
} prospero_pkg_firmware_t;

static inline prospero_pkg_firmware_t prospero_pkg_decode_firmware(uint32_t encoded) {
    prospero_pkg_firmware_t firmware = {0, 0};
    uint8_t major = (uint8_t)(encoded >> 24);
    uint8_t minor = (uint8_t)(encoded >> 16);
    if ((major >> 4) > 9 || (major & 0x0f) > 9 || (minor >> 4) > 9 || (minor & 0x0f) > 9) {
        return firmware;
    }
    firmware.major = (major >> 4) * 10 + (major & 0x0f);
    firmware.minor = (minor >> 4) * 10 + (minor & 0x0f);
    return firmware;
}

// Below firmware 11 the installer only takes a local package under ShellCore
// authorization. Unknown firmware uses it too — it covers more consoles.
static inline int prospero_pkg_needs_shellcore_authid(int firmware_major) {
    return firmware_major < 11;
}
