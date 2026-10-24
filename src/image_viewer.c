/* Image viewer. Reads an uncompressed BMP (8-bit or 24-bit) one row at a
 * time, so the whole picture never sits in memory. The picture is drawn once
 * when it opens. */

#include <tice.h>
#include <graphx.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <fatdrvce.h>
#include "image_viewer.h"

#define SCREEN_W     320
#define SCREEN_H     240
#define FILE_BLOCK   512
#define IMG_MAX_W    640
#define ROW_MAX      (IMG_MAX_W * 3 + 4)   /* one 24-bit row, padded to 4 bytes */
#define COLOR_BLACK  0x00

static bool showing = false;
static fat_file_t file;
static bool file_open = false;
static uint8_t blk[FILE_BLOCK];
static uint32_t blk_pos = FILE_BLOCK;      /* next unread byte in blk */
static uint8_t row[ROW_MAX];
static uint8_t bmp_map[256];               /* BMP palette index -> 3-3-2 color */

static bool has_bmp_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return tolower((unsigned char)dot[1]) == 'b' &&
           tolower((unsigned char)dot[2]) == 'm' &&
           tolower((unsigned char)dot[3]) == 'p' &&
           dot[4] == 0;
}

static uint32_t u32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t u16_at(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Packs an 8-bit RGB color into the 3-3-2 palette used for drawing. */
static uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6));
}

/* Reads n bytes from the file, or skips them if dst is NULL. */
static bool read_bytes(uint8_t *dst, uint32_t n) {
    while (n > 0) {
        uint32_t take;
        if (blk_pos >= FILE_BLOCK) {
            if (fat_ReadFile(&file, 1, blk) != 1) return false;
            blk_pos = 0;
        }
        take = FILE_BLOCK - blk_pos;
        if (take > n) take = n;
        if (dst) {
            memcpy(dst, blk + blk_pos, take);
            dst += take;
        }
        blk_pos += take;
        n -= take;
    }
    return true;
}

static void close_file(void) {
    if (file_open) {
        fat_CloseFile(&file);
        file_open = false;
    }
}

/* Sets the palette to 256 colors covering the 3-3-2 cube. */
static void set_image_palette(void) {
    static uint16_t pal[256];
    int i;

    for (i = 0; i < 256; i++) {
        uint8_t r = (uint8_t)(((i >> 5) & 7) * 255 / 7);
        uint8_t g = (uint8_t)(((i >> 2) & 7) * 255 / 7);
        uint8_t b = (uint8_t)((i & 3) * 255 / 3);
        pal[i] = gfx_RGBTo1555(r, g, b);
    }
    gfx_SetPalette(pal, sizeof(pal), 0);
}

bool image_can_open(const char *name) {
    return has_bmp_ext(name);
}

bool image_open(fat_t *fat, const char *fpath, const char *name) {
    uint8_t hdr[54];
    uint8_t entry[4];
    uint32_t off, ncol = 0, pos, stride, k;
    int32_t w_s, h_s;
    uint16_t bpp;
    bool top_down;
    int w, h, step, dw, dh, ox, oy;
    int dx, y;

    (void)name;
    image_close();
    if (fat_OpenFile(fat, fpath, 0, &file) != FAT_SUCCESS) return false;
    file_open = true;
    blk_pos = FILE_BLOCK;

    /* File header (14 bytes) + BITMAPINFOHEADER (40 bytes). */
    if (!read_bytes(hdr, 54) || hdr[0] != 'B' || hdr[1] != 'M') goto fail;
    if (u32_at(hdr + 14) != 40) goto fail;                 /* header size */
    w_s = (int32_t)u32_at(hdr + 18);
    h_s = (int32_t)u32_at(hdr + 22);
    bpp = u16_at(hdr + 28);
    if (u16_at(hdr + 26) != 1) goto fail;                  /* planes */
    if (u32_at(hdr + 30) != 0) goto fail;                  /* compressed */
    if (bpp != 8 && bpp != 24) goto fail;
    if (w_s <= 0 || w_s > IMG_MAX_W || h_s == 0) goto fail;
    w = (int)w_s;
    top_down = h_s < 0;
    h = top_down ? -(int)h_s : (int)h_s;
    off = u32_at(hdr + 10);
    pos = 54;

    if (bpp == 8) {
        ncol = u32_at(hdr + 46);
        if (ncol == 0 || ncol > 256) ncol = 256;
        for (k = 0; k < ncol; k++) {
            if (!read_bytes(entry, 4)) goto fail;          /* B, G, R, 0 */
            bmp_map[k] = rgb332(entry[2], entry[1], entry[0]);
        }
        pos += ncol * 4;
    }
    if (off < pos) goto fail;
    if (!read_bytes(NULL, off - pos)) goto fail;

    stride = bpp == 8 ? (uint32_t)((w + 3) & ~3) : (uint32_t)((w * 3 + 3) & ~3);
    if (stride > ROW_MAX) goto fail;

    /* Shrink by whole pixels if the picture is bigger than the screen. */
    step = 1;
    if (w > SCREEN_W) step = (w + SCREEN_W - 1) / SCREEN_W;
    if (h > SCREEN_H) {
        int s2 = (h + SCREEN_H - 1) / SCREEN_H;
        if (s2 > step) step = s2;
    }
    dw = w / step;
    dh = h / step;
    ox = (SCREEN_W - dw) / 2;
    oy = (SCREEN_H - dh) / 2;

    set_image_palette();
    gfx_FillScreen(COLOR_BLACK);

    for (k = 0; k < (uint32_t)h; k++) {
        /* File rows run bottom-up unless the height was negative. */
        int r = top_down ? (int)k : h - 1 - (int)k;
        if (r % step != 0 || r / step >= dh) {
            if (!read_bytes(NULL, stride)) break;
            continue;
        }
        if (!read_bytes(row, stride)) break;
        y = oy + r / step;
        for (dx = 0; dx < dw; dx++) {
            int sx = dx * step;
            uint8_t c;
            if (bpp == 8) {
                c = bmp_map[row[sx]];
            } else {
                c = rgb332(row[sx * 3 + 2], row[sx * 3 + 1], row[sx * 3]);
            }
            gfx_SetColor(c);
            gfx_SetPixel(ox + dx, y);
        }
    }

    close_file();
    gfx_SwapDraw();
    showing = true;
    return true;

fail:
    close_file();
    return false;
}

bool image_is_open(void) {
    return showing;
}

/* Enter, Del or Clear goes back. Any other key is ignored. */
bool image_key(uint8_t key) {
    if (key == sk_Enter || key == sk_Del || key == sk_Clear) {
        showing = false;
        return true;
    }
    return false;
}

void image_close(void) {
    close_file();
    showing = false;
}
