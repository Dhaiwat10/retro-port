/* Converts Direct3D 8 texture data to RGBA8 for upload. */
#include "d3d8.h"

static void rgb565(uint16_t v, uint8_t *o) {
    o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((v & 31) * 255 / 31);
}

static void decode_dxt_block(const uint8_t *b, uint32_t fmt, uint8_t out[16][4]) {
    const uint8_t *cb = fmt == FMT_DXT1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || fmt != FMT_DXT1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
    if (fmt == FMT_DXT2 || fmt == FMT_DXT3) {
        for (int i = 0; i < 16; i++) out[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else if (fmt == FMT_DXT4 || fmt == FMT_DXT5) {
        uint8_t a0 = b[0], a1 = b[1], ap[8];
        ap[0] = a0; ap[1] = a1;
        if (a0 > a1) for (int i = 1; i < 7; i++) ap[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        else { for (int i = 1; i < 5; i++) ap[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); ap[6] = 0; ap[7] = 255; }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) out[i][3] = ap[(bits >> (3 * i)) & 7];
    }
}

/* src: level data with `pitch` bytes per row (per block row for DXT); dst: w*h*4 RGBA8.
   palette: 256 RGBA entries for P8 (may be NULL). Returns 0 for unsupported formats. */
int texture_to_rgba8(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst,
                     const uint8_t *palette) {
    if (format_is_dxt(fmt)) {
        uint32_t bs = fmt == FMT_DXT1 ? 8 : 16;
        uint8_t blk[16][4];
        for (uint32_t by = 0; by < (h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                decode_dxt_block(src + by * pitch + bx * bs, fmt, blk);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x, py = by * 4 + y;
                        if (px < w && py < h) memcpy(dst + 4 * (py * w + px), blk[y * 4 + x], 4);
                    }
            }
        return 1;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch;
        uint8_t *o = dst + 4 * y * w;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            switch (fmt) {
            case FMT_A8R8G8B8: o[0] = s[4 * x + 2]; o[1] = s[4 * x + 1]; o[2] = s[4 * x]; o[3] = s[4 * x + 3]; break;
            case FMT_X8R8G8B8: o[0] = s[4 * x + 2]; o[1] = s[4 * x + 1]; o[2] = s[4 * x]; o[3] = 255; break;
            case FMT_R8G8B8: o[0] = s[3 * x + 2]; o[1] = s[3 * x + 1]; o[2] = s[3 * x]; o[3] = 255; break;
            case FMT_R5G6B5: rgb565((uint16_t)(s[2 * x] | s[2 * x + 1] << 8), o); o[3] = 255; break;
            case FMT_X1R5G5B5: case FMT_A1R5G5B5: {
                uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = fmt == FMT_A1R5G5B5 ? ((v >> 15) ? 255 : 0) : 255;
                break;
            }
            case FMT_A4R4G4B4: {
                uint16_t v = (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                o[0] = (uint8_t)(((v >> 8) & 15) * 17); o[1] = (uint8_t)(((v >> 4) & 15) * 17);
                o[2] = (uint8_t)((v & 15) * 17); o[3] = (uint8_t)((v >> 12) * 17);
                break;
            }
            case FMT_A8: o[0] = o[1] = o[2] = 255; o[3] = s[x]; break;
            case FMT_L8: o[0] = o[1] = o[2] = s[x]; o[3] = 255; break;
            case FMT_A8L8: o[0] = o[1] = o[2] = s[2 * x]; o[3] = s[2 * x + 1]; break;
            case FMT_P8:
                if (palette) memcpy(o, palette + 4 * s[x], 4);
                else { o[0] = o[1] = o[2] = s[x]; o[3] = 255; }
                break;
            default: return 0;
            }
        }
    }
    return 1;
}
