#include "avatar.hpp"

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "json_writer.hpp"
#include "ps5_services.hpp"
#include "stb_image.h"
#include "stb_image_write.h"

namespace prospero {

namespace {

void EnsureDir(const std::string &path) { mkdir(path.c_str(), 0755); }

std::string WorkDir(const Config &cfg) { return cfg.tmp_dir + "/avatar_work"; }

// ---- Bilinear resize ------------------------------------------------------

std::vector<uint8_t> ResizeRgba(const uint8_t *src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; y++) {
        double ys = static_cast<double>(y) * sh / dh;
        int y0 = static_cast<int>(ys);
        int y1 = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
        double yf = ys - y0;
        for (int x = 0; x < dw; x++) {
            double xs = static_cast<double>(x) * sw / dw;
            int x0 = static_cast<int>(xs);
            int x1 = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
            double xf = xs - x0;
            const uint8_t *p00 = &src[(y0 * sw + x0) * 4];
            const uint8_t *p10 = &src[(y0 * sw + x1) * 4];
            const uint8_t *p01 = &src[(y1 * sw + x0) * 4];
            const uint8_t *p11 = &src[(y1 * sw + x1) * 4];
            uint8_t *o = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            for (int c = 0; c < 4; c++) {
                double v = p00[c] * (1 - xf) * (1 - yf) + p10[c] * xf * (1 - yf) + p01[c] * (1 - xf) * yf +
                           p11[c] * xf * yf;
                o[c] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
            }
        }
    }
    return dst;
}

std::vector<uint8_t> CenterCropSquare(const uint8_t *src, int sw, int sh, int *side_out) {
    int side = sw < sh ? sw : sh;
    int x0 = (sw - side) / 2;
    int y0 = (sh - side) / 2;
    std::vector<uint8_t> dst(static_cast<size_t>(side) * side * 4);
    for (int y = 0; y < side; y++) {
        std::memcpy(&dst[static_cast<size_t>(y) * side * 4], &src[((y0 + y) * sw + x0) * 4],
                    static_cast<size_t>(side) * 4);
    }
    *side_out = side;
    return dst;
}

// ---- DXT5 block compression (standard fixed-palette approach) -------------

uint16_t Rgb888To565(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Rgb565To888(uint16_t c, int out[3]) {
    out[0] = ((c >> 11) & 0x1f) << 3;
    out[1] = ((c >> 5) & 0x3f) << 2;
    out[2] = (c & 0x1f) << 3;
}

int ColorDistanceSq(const int a[3], const int b[3]) {
    int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// One 4x4 RGBA block (64 bytes in) -> 16 bytes of DXT5.
void CompressBlockDxt5(const uint8_t pixels[64], uint8_t out[16]) {
    int a_min = 255, a_max = 0;
    for (int i = 0; i < 16; i++) {
        int a = pixels[i * 4 + 3];
        if (a < a_min) a_min = a;
        if (a > a_max) a_max = a;
    }
    uint8_t alpha0 = static_cast<uint8_t>(a_max), alpha1 = static_cast<uint8_t>(a_min);

    uint8_t apal[8] = {alpha0, alpha1};
    if (alpha0 > alpha1) {
        for (int i = 0; i < 6; i++) apal[2 + i] = static_cast<uint8_t>(((6 - i) * alpha0 + (1 + i) * alpha1) / 7);
    } else {
        for (int i = 0; i < 4; i++) apal[2 + i] = static_cast<uint8_t>(((4 - i) * alpha0 + (1 + i) * alpha1) / 5);
        apal[6] = 0;
        apal[7] = 255;
    }

    uint64_t aindex = 0;
    for (int i = 0; i < 16; i++) {
        int a = pixels[i * 4 + 3];
        int best = 0, dist = 256;
        for (int j = 0; j < 8; j++) {
            int d = std::abs(a - apal[j]);
            if (d < dist) {
                dist = d;
                best = j;
            }
        }
        aindex |= static_cast<uint64_t>(best) << (i * 3);
    }
    out[0] = alpha0;
    out[1] = alpha1;
    for (int i = 0; i < 6; i++) out[2 + i] = static_cast<uint8_t>((aindex >> (i * 8)) & 0xff);

    int min_c[3] = {255, 255, 255}, max_c[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            int v = pixels[i * 4 + c];
            if (v < min_c[c]) min_c[c] = v;
            if (v > max_c[c]) max_c[c] = v;
        }
    }
    uint16_t color0 = Rgb888To565(max_c[0], max_c[1], max_c[2]);
    uint16_t color1 = Rgb888To565(min_c[0], min_c[1], min_c[2]);
    if (color0 == color1 && color0 < 0xffff) color0++;

    int c0[3], c1[3];
    Rgb565To888(color0, c0);
    Rgb565To888(color1, c1);
    int pal[4][3] = {
        {c0[0], c0[1], c0[2]},
        {c1[0], c1[1], c1[2]},
        {},
        {},
    };
    for (int c = 0; c < 3; c++) {
        pal[2][c] = (2 * c0[c] + c1[c]) / 3;
        pal[3][c] = (c0[c] + 2 * c1[c]) / 3;
    }

    uint32_t cindex = 0;
    for (int i = 0; i < 16; i++) {
        int p[3] = {pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]};
        int best = 0, dist = ColorDistanceSq(p, pal[0]);
        for (int j = 1; j < 4; j++) {
            int d = ColorDistanceSq(p, pal[j]);
            if (d < dist) {
                dist = d;
                best = j;
            }
        }
        cindex |= static_cast<uint32_t>(best) << (i * 2);
    }
    out[8] = color0 & 0xff;
    out[9] = (color0 >> 8) & 0xff;
    out[10] = color1 & 0xff;
    out[11] = (color1 >> 8) & 0xff;
    out[12] = cindex & 0xff;
    out[13] = (cindex >> 8) & 0xff;
    out[14] = (cindex >> 16) & 0xff;
    out[15] = (cindex >> 24) & 0xff;
}

bool WriteDdsDxt5(const uint8_t *rgba, int w, int h, const std::string &path) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t comp_size = static_cast<size_t>(bw) * bh * 16;

    std::vector<uint8_t> buf(128 + comp_size, 0);
    std::memcpy(buf.data(), "DDS ", 4);
    buf[4] = 124;
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
    std::memcpy(&buf[8], &flags, 4);
    uint32_t hh = static_cast<uint32_t>(h), ww = static_cast<uint32_t>(w), lsz = static_cast<uint32_t>(comp_size);
    std::memcpy(&buf[12], &hh, 4);
    std::memcpy(&buf[16], &ww, 4);
    std::memcpy(&buf[20], &lsz, 4);
    buf[76] = 32;
    buf[80] = 4; // DDPF_FOURCC
    std::memcpy(&buf[84], "DXT5", 4);
    buf[108] = 0;
    buf[109] = 0x10; // DDSCAPS_TEXTURE

    uint8_t block[64];
    uint8_t *p = buf.data() + 128;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                int py = by * 4 + y;
                if (py >= h) py = h - 1;
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x;
                    if (px >= w) px = w - 1;
                    std::memcpy(&block[(y * 4 + x) * 4], &rgba[(py * w + px) * 4], 4);
                }
            }
            CompressBlockDxt5(block, p);
            p += 16;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return out.good();
}

bool CopyFile(const std::string &src, const std::string &dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << in.rdbuf();
    return out.good();
}

} // namespace

bool BuildAvatarWork(const Config &cfg, const std::string &image_bytes, std::string *err) {
    std::string work = WorkDir(cfg);
    EnsureDir(work);

    int w = 0, h = 0, channels = 0;
    uint8_t *rgba = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(image_bytes.data()),
                                           static_cast<int>(image_bytes.size()), &w, &h, &channels, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (err) *err = std::string("image decode failed: ") + stbi_failure_reason();
        if (rgba) stbi_image_free(rgba);
        return false;
    }

    int side = 0;
    std::vector<uint8_t> square = CenterCropSquare(rgba, w, h, &side);
    stbi_image_free(rgba);

    for (int sz : kAvatarSizes) {
        std::vector<uint8_t> resized = (sz == side) ? square : ResizeRgba(square.data(), side, side, sz, sz);

        std::string dds_path = work + "/avatar" + std::to_string(sz) + ".dds";
        std::string png_path = work + "/avatar" + std::to_string(sz) + ".png";

        if (!WriteDdsDxt5(resized.data(), sz, sz, dds_path)) {
            if (err) *err = "DDS write failed for " + std::to_string(sz) + "x" + std::to_string(sz);
            return false;
        }
        stbi_write_png(png_path.c_str(), sz, sz, 4, resized.data(), sz * 4);
    }
    return true;
}

std::string AvatarPreviewPath(const Config &cfg, int size) {
    bool valid = false;
    for (int sz : kAvatarSizes) valid = valid || sz == size;
    if (!valid) return "";
    return WorkDir(cfg) + "/avatar" + std::to_string(size) + ".png";
}

AvatarUser GetAvatarUser() {
    AvatarUser u;
    sceUserServiceInitialize(nullptr);

    uint32_t uid = 0;
    if (sceUserServiceGetForegroundUser(&uid) != 0 || uid == 0) {
        u.error = "no foreground user — sign in to a profile first";
        return u;
    }

    char name[32] = {0};
    sceUserServiceGetUserName(static_cast<int32_t>(uid), name, sizeof(name));

    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", uid);

    u.ok = true;
    u.user_id = uid;
    u.user_id_hex = hex;
    u.user_name = name[0] ? name : "?";
    u.profile_dir = std::string("/system_data/priv/cache/profile/") + hex;
    return u;
}

bool SetForegroundUserName(const std::string &name, std::string *err) {
    sceUserServiceInitialize(nullptr);

    uint32_t uid = 0;
    if (sceUserServiceGetForegroundUser(&uid) != 0 || uid == 0) {
        if (err) *err = "no foreground user — sign in to a profile first";
        return false;
    }

    int rc = sceUserServiceSetUserName(static_cast<int32_t>(uid), name.c_str());
    if (rc != 0) {
        if (err) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "sceUserServiceSetUserName failed (0x%08x)", static_cast<unsigned>(rc));
            *err = buf;
        }
        return false;
    }
    return true;
}

std::string GetUserDisplayName(uint32_t user_id) {
    if (user_id == 0) return "";
    sceUserServiceInitialize(nullptr);
    char name[32] = {0};
    sceUserServiceGetUserName(static_cast<int32_t>(user_id), name, sizeof(name));
    return name[0] ? name : "";
}

bool ApplyAvatarToUser(const Config &cfg, const AvatarUser &user, std::string *err) {
    if (!user.ok) {
        if (err) *err = user.error.empty() ? "no foreground user" : user.error;
        return false;
    }

    EnsureDir("/system_data/priv/cache");
    EnsureDir("/system_data/priv/cache/profile");
    EnsureDir(user.profile_dir);

    std::string work = WorkDir(cfg);
    bool all_ok = true;

    // The shell reads both an "avatar" and a "picture" prefix — write both.
    static const char *kKinds[] = {"avatar", "picture"};
    for (const char *kind : kKinds) {
        for (int sz : kAvatarSizes) {
            std::string src = work + "/avatar" + std::to_string(sz) + ".dds";
            std::string dst = user.profile_dir + "/" + kind + std::to_string(sz) + ".dds";
            all_ok = CopyFile(src, dst) && all_ok;
        }
        std::string src_png = work + "/avatar440.png";
        std::string dst_png = user.profile_dir + "/" + kind + ".png";
        all_ok = CopyFile(src_png, dst_png) && all_ok;
    }

    // Minimal local online.json the shell reads instead of hitting PSN. isOfficiallyVerified
    // must be the string "true" — the shell's parser rejects a real boolean here.
    std::string online_json =
        JsonWriter().Str("firstName", user.user_name).Str("lastName", "").Str("isOfficiallyVerified", "true").Done();
    std::ofstream online(user.profile_dir + "/online.json", std::ios::trunc);
    if (online.is_open()) {
        online << online_json;
    } else {
        all_ok = false;
    }

    if (!all_ok && err) *err = "some files failed to copy — see " + work;
    return all_ok;
}

std::string ProfileIconPath(const std::string &user_id_hex) {
    if (user_id_hex.empty()) return "";
    // "picture" is PSN's own downloaded avatar, "avatar" is ours after an apply.
    // Prefer picture: it reflects the real photo.
    for (const char *kind : {"picture", "avatar"}) {
        std::string path = "/system_data/priv/cache/profile/" + user_id_hex + "/" + kind + ".png";
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return path;
    }
    return "";
}

} // namespace prospero
