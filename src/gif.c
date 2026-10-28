#include <stdlib.h>
#include <string.h>
#include "gif.h"

/* Block codes */
#define GIF_EXTENSION   0x21
#define GIF_IMAGE       0x2C
#define GIF_TRAILER     0x3B
#define GIF_GCE_LABEL   0xF9
#define LZW_NONE        0xFFFF
#define LZW_MAX_CODES   4096
#define LZW_START_CODES 512               /* the tables start this big and grow as the file needs */

/* Why the last GIF failed, for the screen. */
static const char *gif_err = "";
static bool gif_fail(const char *msg) {
    gif_err = msg;
    return false;
}
const char *gif_error(void) {
    return gif_err;
}

static const uint8_t *g_data = NULL;
static uint32_t g_len = 0;
static uint32_t g_first = 0;               /* first block after the global table */
static uint32_t g_pos = 0;                 /* where the next block starts */
static uint16_t g_w = 0, g_h = 0;
static uint32_t g_frames = 0;
static const uint8_t *g_gct = NULL;      /* the file's global color table, inside the file data */
static uint16_t g_gct_n = 0;

/* Big buffers are allocated when a GIF loads and freed when it unloads, so
 * they take no space in the program's static memory. */
static uint8_t *canvas = NULL;             /* w * h palette indexes */
static const uint8_t *cur_pal = NULL;      /* the table the current frame uses, inside the file data */
static uint16_t cur_pal_n = 0;
static uint32_t cur_delay_cs = 10;

/* Graphic Control Extension: applies to the next image only */
static bool gce_set = false;
static uint8_t gce_disposal = 0;
static bool gce_transparent = false;
static uint8_t gce_tidx = 0;
static uint16_t gce_delay = 0;

/* The last frame's rectangle, so it can be cleared when disposal says so */
static bool prev_valid = false;
static uint16_t prev_l, prev_t, prev_w, prev_h;
static uint8_t prev_disposal = 0;

/* LZW dictionary */
static uint16_t *lzw_prefix = NULL;
static uint8_t *lzw_suffix = NULL;
static uint16_t rowmap[GIF_MAX_H];
static uint8_t *lzw_stack = NULL;          /* a string being written out, last byte first */
static uint32_t lzw_cap = 0;               /* entries the tables have room for */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Skips a run of sub-blocks (size byte, data, ... , 0 byte). Returns the
 * offset after the terminator, or g_len + 1 if the data runs out. */
static uint32_t skip_sub(uint32_t pos) {
    while (pos < g_len) {
        uint32_t n = g_data[pos++];
        if (n == 0) return pos;
        pos += n;
    }
    return g_len + 1;
}

static bool gif_parse(const uint8_t *data, uint32_t len);

/* Parses a GIF. If it fails, everything allocated for it is freed again. */
bool gif_load(const uint8_t *data, uint32_t len) {
    gif_err = "";
    bool ok = gif_parse(data, len);
    if (!ok) gif_unload();
    return ok;
}

static bool gif_parse(const uint8_t *data, uint32_t len) {
    uint32_t pos = 13, frames = 0;
    uint8_t packed;
    uint16_t w, h, n;

    gif_unload();
    g_data = data;                  /* skip_sub() needs these while the file is scanned */
    g_len = len;
    if (len < 13 || memcmp(data, "GIF", 3) != 0) return gif_fail("not a GIF file");
    if (memcmp(data + 3, "87a", 3) != 0 && memcmp(data + 3, "89a", 3) != 0) return gif_fail("not GIF87a or 89a");
    w = rd16(data + 6);
    h = rd16(data + 8);
    if (w == 0 || h == 0 || w > GIF_MAX_W || h > GIF_MAX_H) return gif_fail("image is over 160x160");

    packed = data[10];
    g_gct_n = 0;
    if (packed & 0x80) {
        n = (uint16_t)(2 << (packed & 7));
        if (pos + 3u * n > len) return gif_fail("truncated color table");
        g_gct = data + pos;
        g_gct_n = n;
        pos += 3u * n;
    }
    g_first = pos;

    canvas = malloc((size_t)w * h);
    lzw_prefix = malloc(LZW_START_CODES * sizeof(uint16_t));
    lzw_suffix = malloc(LZW_START_CODES);
    lzw_stack = malloc(LZW_START_CODES);
    lzw_cap = LZW_START_CODES;
    if (!canvas || !lzw_prefix || !lzw_suffix || !lzw_stack) {
        gif_unload();
        return gif_fail("no memory for decoder");
    }

    /* Count the frames now, so the viewer knows whether to animate. */
    while (pos < len) {
        uint8_t b = data[pos];
        if (b == GIF_TRAILER) break;
        if (b == GIF_EXTENSION) {
            pos = skip_sub(pos + 2);
        } else if (b == GIF_IMAGE) {
            uint8_t p;
            if (pos + 10 > len) return gif_fail("truncated image block");
            p = data[pos + 9];
            pos += 10;
            if (p & 0x80) pos += 3u * (2u << (p & 7));
            pos = skip_sub(pos + 1);
            frames++;
        } else {
            return gif_fail("unknown block in file");
        }
        if (pos > len) return gif_fail("truncated file");
    }
    if (frames == 0) return gif_fail("file has no frames");

    g_w = w;
    g_h = h;
    g_frames = frames;
    g_pos = g_first;
    memset(canvas, 0, (size_t)w * h);
    return true;
}

void gif_unload(void) {
    g_data = NULL;
    g_len = 0;
    g_frames = 0;
    g_gct = NULL;
    cur_pal = NULL;
    prev_valid = false;
    gce_set = false;
    free(canvas);
    free(lzw_prefix);
    free(lzw_suffix);
    free(lzw_stack);
    canvas = NULL;
    lzw_prefix = NULL;
    lzw_suffix = NULL;
    lzw_stack = NULL;
    lzw_cap = 0;
}

uint16_t gif_width(void) { return g_w; }
uint16_t gif_height(void) { return g_h; }
const uint8_t *gif_canvas(void) { return canvas; }
const uint8_t *gif_palette_rgb(void) { return cur_pal; }
uint16_t gif_palette_count(void) { return cur_pal_n; }
uint32_t gif_delay_cs(void) { return cur_delay_cs; }
uint32_t gif_frame_count(void) { return g_frames; }

/* Reads codes of a fixed bit width from LZW data split into sub-blocks. */
typedef struct {
    const uint8_t *p;
    uint32_t end;
    uint32_t left;      /* bytes left in the current sub-block */
    uint32_t cur;
    uint32_t nbits;
} bitreader_t;

static int rd_byte(bitreader_t *r) {
    if (r->left == 0) {
        uint32_t n;
        if (r->p >= g_data + r->end) return -1;
        n = *r->p++;
        if (n == 0) return -1;
        r->left = n;
    }
    if (r->p >= g_data + r->end) return -1;
    r->left--;
    return *r->p++;
}

static int rd_code(bitreader_t *r, uint32_t bits) {
    while (r->nbits < bits) {
        int b = rd_byte(r);
        if (b < 0) return -1;
        r->cur |= (uint32_t)b << r->nbits;
        r->nbits += 8;
    }
    {
        uint32_t code = r->cur & ((1u << bits) - 1);
        r->cur >>= bits;
        r->nbits -= bits;
        return (int)code;
    }
}

/* Makes the tables hold at least `need` entries (up to LZW_MAX_CODES). */
static bool lzw_grow(uint32_t need) {
    uint32_t nc = lzw_cap;
    uint16_t *np;
    uint8_t *ns, *nt;

    while (nc < need) nc *= 2;
    if (nc > LZW_MAX_CODES) nc = LZW_MAX_CODES;
    np = realloc(lzw_prefix, nc * sizeof(uint16_t));
    if (!np) return false;
    lzw_prefix = np;
    ns = realloc(lzw_suffix, nc);
    if (!ns) return false;
    lzw_suffix = ns;
    nt = realloc(lzw_stack, nc);
    if (!nt) return false;
    lzw_stack = nt;
    lzw_cap = nc;
    return true;
}

/* The first byte of the string for code c. Found by walking back to the root,
 * which is slower than a table but only runs while a GIF is decoded on open. */
static uint8_t first_byte(uint32_t c) {
    while (lzw_prefix[c] != LZW_NONE) c = lzw_prefix[c];
    return (uint8_t)c;
}

/* Decodes one image's LZW data into the canvas. Pixels outside the canvas
 * are dropped, and transparent pixels leave the canvas as it was. */
static bool lzw_decode(uint32_t data_start, uint32_t min, uint32_t l, uint32_t t, uint32_t fw, uint32_t fh,
                       bool interlaced, bool transparent, uint8_t tidx) {
    uint32_t clear = 1u << min, eoi = clear + 1, next = eoi + 1, codesize = min + 1;
    uint32_t prev = LZW_NONE, fx = 0, fy = 0, i;   /* fx, fy: where the next pixel goes in the frame */
    bitreader_t r;

    if (min < 1 || min > 8) return gif_fail("bad LZW code size");
    for (i = 0; i < clear; i++) {
        lzw_prefix[i] = LZW_NONE;
        lzw_suffix[i] = (uint8_t)i;
    }
    r.p = g_data + data_start;
    r.end = g_len;
    r.left = 0;
    r.cur = 0;
    r.nbits = 0;

    for (;;) {
        int code = rd_code(&r, codesize);
        uint32_t c;
        uint32_t len;

        if (code < 0 || fy >= fh) break;
        c = (uint32_t)code;
        if (c == clear) {
            codesize = min + 1;
            next = eoi + 1;
            prev = LZW_NONE;
            continue;
        }
        if (c == eoi) break;

        if (prev == LZW_NONE) {
            if (c >= clear) return gif_fail("bad LZW code");
        } else if (c < next) {
            if (next < LZW_MAX_CODES) {
                if (next >= lzw_cap && !lzw_grow(next + 1)) return gif_fail("no memory for decoder");
                lzw_prefix[next] = (uint16_t)prev;
                lzw_suffix[next] = first_byte(c);
                next++;
            }
        } else if (c == next) {
            if (next >= LZW_MAX_CODES) return gif_fail("LZW table full");
            if (next >= lzw_cap && !lzw_grow(next + 1)) return gif_fail("no memory for decoder");
            lzw_prefix[next] = (uint16_t)prev;
            lzw_suffix[next] = first_byte(prev);
            next++;
        } else {
            return gif_fail("bad LZW code");
        }

        /* Write the string for code c, last byte first, then reversed. */
        len = 0;
        for (i = c; i >= clear; ) {
            lzw_stack[len++] = lzw_suffix[i];
            i = lzw_prefix[i];
            if (len >= LZW_MAX_CODES) return gif_fail("LZW string too long");
        }
        lzw_stack[len++] = (uint8_t)i;
        while (len > 0 && fy < fh) {
            uint8_t v = lzw_stack[--len];
            uint32_t cx = l + fx, cy = t + (interlaced ? rowmap[fy] : fy);

            if (cx < g_w && cy < g_h && !(transparent && v == tidx)) canvas[cy * g_w + cx] = v;
            if (++fx == fw) {                  /* next row */
                fx = 0;
                fy++;
            }
        }
        if (prev != LZW_NONE && next == (1u << codesize) && codesize < 12) codesize++;
        prev = c;
    }
    return true;
}

/* Fills the rectangle of the canvas with color 0. */
static void clear_rect(uint16_t l, uint16_t t, uint16_t w, uint16_t h) {
    uint16_t y, x;
    for (y = t; y < t + h && y < g_h; y++) {
        for (x = l; x < l + w && x < g_w; x++) canvas[y * g_w + x] = 0;
    }
}

/* Decodes the image block at g_pos. Its pixels go onto the canvas. */
static bool decode_image(void) {
    uint32_t pos = g_pos;
    uint16_t l, t, fw, fh, n, y;
    uint8_t p, min;
    uint32_t data_start;
    bool interlaced;

    if (pos + 10 > g_len) return gif_fail("truncated frame");
    l = rd16(g_data + pos + 1);
    t = rd16(g_data + pos + 3);
    fw = rd16(g_data + pos + 5);
    fh = rd16(g_data + pos + 7);
    p = g_data[pos + 9];
    pos += 10;

    if (fw == 0 || fh == 0 || fh > GIF_MAX_H) return gif_fail("bad frame size");

    /* Undo the previous frame if it asked for it (disposal 2 = clear to background). */
    if (prev_valid && prev_disposal == 2) clear_rect(prev_l, prev_t, prev_w, prev_h);

    if (p & 0x80) {
        n = (uint16_t)(2 << (p & 7));
        if (pos + 3u * n > g_len) return gif_fail("truncated palette");
        cur_pal = g_data + pos;
        cur_pal_n = n;
        pos += 3u * n;
    } else {
        cur_pal = g_gct;
        cur_pal_n = g_gct_n;
    }
    interlaced = (p & 0x40) != 0;
    if (interlaced) {
        uint16_t r = 0;
        for (y = 0; y < fh; y += 8) rowmap[r++] = y;
        for (y = 4; y < fh; y += 8) rowmap[r++] = y;
        for (y = 2; y < fh; y += 4) rowmap[r++] = y;
        for (y = 1; y < fh; y += 2) rowmap[r++] = y;
    }

    if (pos >= g_len) return gif_fail("frame data missing");
    min = g_data[pos];
    data_start = pos + 1;
    if (!lzw_decode(data_start, min, l, t, fw, fh, interlaced, gce_set && gce_transparent, gce_tidx)) return gif_fail("frame data bad");

    g_pos = skip_sub(data_start);
    if (g_pos > g_len) return gif_fail("frame data truncated");

    cur_delay_cs = (gce_set && gce_delay >= 2) ? gce_delay : 10;   /* 0 or 1 means "as fast as possible": use 100 ms */
    prev_valid = true;
    prev_l = l;
    prev_t = t;
    prev_w = fw;
    prev_h = fh;
    prev_disposal = gce_disposal;
    gce_set = false;
    return true;
}

bool gif_next(void) {
    uint32_t wraps = 0;

    if (!g_data) return gif_fail("GIF not loaded");
    while (wraps < 2) {
        if (g_pos >= g_len || g_data[g_pos] == GIF_TRAILER) {
            /* Start again from the first frame. */
            g_pos = g_first;
            memset(canvas, 0, (size_t)g_w * g_h);
            prev_valid = false;
            wraps++;
            continue;
        }
        if (g_data[g_pos] == GIF_EXTENSION) {
            uint32_t pos = g_pos;
            if (pos + 2 < g_len && g_data[pos + 1] == GIF_GCE_LABEL && g_data[pos + 2] == 4 && pos + 7 <= g_len) {
                uint8_t packed = g_data[pos + 3];
                gce_disposal = (uint8_t)((packed >> 2) & 7);
                gce_transparent = (packed & 1) != 0;
                gce_delay = rd16(g_data + pos + 4);
                gce_tidx = g_data[pos + 6];
                gce_set = true;
            }
            g_pos = skip_sub(pos + 2);
            if (g_pos > g_len) return gif_fail("truncated extension");
            continue;
        }
        if (g_data[g_pos] == GIF_IMAGE) {
            return decode_image();
        }
        return gif_fail("unknown block in frame");
    }
    return gif_fail("no more frames");
}
