/* GIF viewer. See gif_viewer.h for how it works. The decoder is in gif.c. */

#include <tice.h>
#include <graphx.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <fatdrvce.h>
#include "gif_viewer.h"
#include "gif.h"
#include "fmt.h"

#define SCREEN_W      320
#define SCREEN_H      240
#define COLOR_BLACK   0x00
#define FILE_BLOCK    512
#define GIF_FILE_MAX  32768                /* biggest GIF file read into memory */
#define RLE_BUDGET    12288                /* most bytes the converted frames may take */
#define GIF_RING_BYTES 12288               /* memory for the ring slots (fallback) */
#define GIF_RING_MAX  4                    /* most ring slots, even for small frames */

static bool showing = false;               /* a GIF is on screen */
static const uint16_t *pal_loaded = NULL;  /* palette the calculator has now (skip reloading it) */
static bool animated = false;              /* more than one frame */
static uint32_t next_due = 0;              /* clock() value when the next frame is due */
static const char *gif_msg = "";           /* why the last open failed */
static char gif_msg_buf[40];               /* text for failures that include a number */
static fat_file_t file;
static bool file_open = false;
static uint8_t blk[FILE_BLOCK];
static uint8_t *gif_buf = NULL;            /* the GIF file: needed until decoding is done */
static bool gif_loaded = false;            /* the decoder holds this GIF */
static int gif_w = 0, gif_h = 0;

/* Converted frames: runs of (count, palette index) per frame, plus each
 * distinct palette in RGB1555. A frame is expanded into `scratch` to draw. */
static bool rle_mode = false;
static uint8_t *rle = NULL;
static uint32_t rle_used = 0, rle_cap = 0;
static uint8_t *pals = NULL;
static uint32_t pals_used = 0, pals_cap = 0;
static uint32_t *fr_start = NULL;          /* where each frame's runs start in rle */
static uint16_t *fr_len = NULL;            /* bytes of runs in each frame */
static uint32_t *fr_pal = NULL;            /* where each frame's palette starts in pals */
static uint16_t *fr_delay = NULL;          /* each frame's delay, hundredths of a second */
static uint8_t *scratch = NULL;            /* one expanded frame: w, h, then indexes */
static uint32_t play_cur = 0;
static uint32_t play_n = 0;

/* Ring fallback: frames decoded a few ahead of the screen. */
static uint32_t gif_k = 0;                 /* slots in the ring */
static uint8_t *ring_px = NULL;            /* slot frames: w, h, then indexes */
static uint16_t *ring_pal = NULL;          /* slot palettes, RGB1555 */
static uint16_t *ring_delay = NULL;        /* slot delays */
static uint32_t decoded = 0;               /* frames decoded so far */
static uint32_t shown = 0;                 /* frame on screen */

static void set_msg(const char *msg) {
    gif_msg = msg;
}

const char *gif_viewer_error(void) {
    return gif_msg;
}

static bool has_gif_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return tolower((unsigned char)dot[1]) == 'g' &&
           tolower((unsigned char)dot[2]) == 'i' &&
           tolower((unsigned char)dot[3]) == 'f' &&
           dot[4] == 0;
}

bool gif_viewer_can_open(const char *name) {
    return has_gif_ext(name);
}

bool gif_viewer_is_open(void) {
    return showing;
}

/* Clock ticks for a delay in hundredths of a second. */
static uint32_t cs_ticks(uint32_t cs) {
    return (uint32_t)(((uint64_t)cs * 10 * CLOCKS_PER_SEC) / 1000);
}

static void close_file(void) {
    if (file_open) {
        fat_CloseFile(&file);
        file_open = false;
    }
}

/* Converts a palette to RGB1555 for the calculator. */
static void to_rgb1555(const uint8_t *rgb, uint32_t cnt, uint16_t *out) {
    uint32_t j;
    for (j = 0; j < 256; j++) out[j] = 0;
    for (j = 0; j < cnt && j < 256; j++) out[j] = gfx_RGBTo1555(rgb[3 * j], rgb[3 * j + 1], rgb[3 * j + 2]);
}

/* Draws a frame: `sprite` holds its width and height, then its palette
 * indexes. `pal` is its 256 colors. The frame is centered, and shrunk by whole
 * pixels if it is bigger than the screen. */
static void draw_sprite(const uint8_t *sprite, const uint16_t *pal) {
    int w = sprite[0], h = sprite[1];
    const uint8_t *px = sprite + 2;
    int step = 1, dw, dh, ox, oy, dx, dy;

    if (w > SCREEN_W) step = (w + SCREEN_W - 1) / SCREEN_W;
    if (h > SCREEN_H) {
        int s2 = (h + SCREEN_H - 1) / SCREEN_H;
        if (s2 > step) step = s2;
    }
    dw = w / step;
    dh = h / step;
    ox = (SCREEN_W - dw) / 2;
    oy = (SCREEN_H - dh) / 2;
    if (pal != pal_loaded) {               /* the palette is the same for most frames */
        gfx_SetPalette(pal, 512, 0);
        pal_loaded = pal;
    }
    if (step == 1) {                       /* the whole frame in one copy */
        gfx_Sprite_NoClip((const gfx_sprite_t *)sprite, (uint24_t)ox, (uint8_t)oy);
        gfx_SwapDraw();
        return;
    }
    for (dy = 0; dy < dh; dy++) {
        for (dx = 0; dx < dw; dx++) {
            gfx_SetColor(px[(dy * step) * w + dx * step]);
            gfx_SetPixel(ox + dx, oy + dy);
        }
    }
    gfx_SwapDraw();
}

/* Reads a whole GIF file into gif_buf. Returns its size, or 0 on failure. */
static uint32_t read_whole_file(fat_t *fat, const char *fpath) {
    uint32_t size, got = 0;

    if (fat_OpenFile(fat, fpath, 0, &file) != FAT_SUCCESS) {
        set_msg("GIF: can't open file");
        return 0;
    }
    file_open = true;
    size = fat_GetFileSize(&file);
    if (size == 0 || size > GIF_FILE_MAX) {
        set_msg(size == 0 ? "GIF: file is empty" : "GIF: file is over 32 KB");
        close_file();
        return 0;
    }
    gif_buf = malloc(size);
    if (!gif_buf) {
        set_msg("GIF: no memory for file");
        close_file();
        return 0;
    }
    while (got < size) {
        uint32_t take = size - got;
        if (take > FILE_BLOCK) take = FILE_BLOCK;
        if (fat_ReadFile(&file, 1, blk) != 1) break;
        memcpy(gif_buf + got, blk, take);
        got += take;
    }
    close_file();
    if (got != size) {
        set_msg("GIF: file read failed");
        free(gif_buf);
        gif_buf = NULL;
        return 0;
    }
    return size;
}

static void free_frames(void) {
    free(rle);
    free(pals);
    free(fr_start);
    free(fr_len);
    free(fr_pal);
    free(fr_delay);
    free(scratch);
    free(ring_px);
    free(ring_pal);
    free(ring_delay);
    rle = NULL;
    pals = NULL;
    fr_start = NULL;
    fr_len = NULL;
    fr_pal = NULL;
    fr_delay = NULL;
    scratch = NULL;
    ring_px = NULL;
    ring_pal = NULL;
    ring_delay = NULL;
    rle_used = 0;
    pals_used = 0;
    rle_cap = 0;
    pals_cap = 0;
}

/* Makes *buf at least `need` bytes big. */
static bool grow(uint8_t **buf, uint32_t *cap, uint32_t need) {
    uint8_t *nb;

    if (need <= *cap) return true;
    nb = realloc(*buf, need + 128);
    if (!nb) return false;
    *buf = nb;
    *cap = need + 128;
    return true;
}

/* Decodes all n frames and converts them. Returns 0 when they are all kept,
 * 1 when they don't fit in RLE_BUDGET (the caller uses the ring instead), and
 * 2 when the file is bad or out of memory (gif_msg is set). */
static int build_rle(uint32_t n, uint32_t wh) {
    uint32_t i, pos, run, start, last_pal = 0, cs;
    const uint8_t *cv;
    uint16_t tmp[256];
    bool have_pal = false;

    fr_start = malloc(n * sizeof(uint32_t));
    fr_len = malloc(n * sizeof(uint16_t));
    fr_pal = malloc(n * sizeof(uint32_t));
    fr_delay = malloc(n * sizeof(uint16_t));
    /* The store is one block of its full budget, so it never has to grow. */
    if (!fr_start || !fr_len || !fr_pal || !fr_delay) {
        free_frames();
        set_msg("GIF: no memory for frames");
        return 2;
    }
    rle = malloc(RLE_BUDGET);
    if (!rle) {                            /* no room for the store: play from the ring */
        free_frames();
        return 1;
    }
    rle_cap = RLE_BUDGET;
    for (i = 0; i < n; i++) {
        if (!gif_next()) {
            fmt(gif_msg_buf, "GIF frame %lu: %s", (unsigned long)(i + 1), gif_error());
            set_msg(gif_msg_buf);
            free_frames();
            return 2;
        }
        cv = gif_canvas();
        start = rle_used;
        pos = 0;
        while (pos < wh) {
            uint8_t v = cv[pos];
            run = 1;
            while (pos + run < wh && cv[pos + run] == v && run < 255) run++;
            if (rle_used + pals_used + 2 > RLE_BUDGET || !grow(&rle, &rle_cap, rle_used + 2)) {
                free_frames();
                return 1;
            }
            rle[rle_used++] = (uint8_t)run;
            rle[rle_used++] = v;
            pos += run;
        }
        fr_start[i] = start;
        fr_len[i] = (uint16_t)(rle_used - start);

        to_rgb1555(gif_palette_rgb(), gif_palette_count(), tmp);
        if (!have_pal || memcmp(tmp, pals + last_pal, 512) != 0) {   /* a new palette: keep it */
            if (rle_used + pals_used + 512 > RLE_BUDGET || !grow(&pals, &pals_cap, pals_used + 512)) {
                free_frames();
                return 1;
            }
            memcpy(pals + pals_used, tmp, 512);
            last_pal = pals_used;
            pals_used += 512;
            have_pal = true;
        }
        fr_pal[i] = last_pal;
        cs = gif_delay_cs();
        fr_delay[i] = (uint16_t)(cs > 65535 ? 65535 : cs);
    }
    return 0;
}

/* Expands converted frame i into scratch. */
static void expand_frame(uint32_t i) {
    uint32_t p = fr_start[i], end = fr_start[i] + fr_len[i], d = 2;

    scratch[0] = (uint8_t)gif_w;
    scratch[1] = (uint8_t)gif_h;
    while (p < end) {
        uint8_t run = rle[p], v = rle[p + 1];
        memset(scratch + d, v, run);
        d += run;
        p += 2;
    }
}

/* Decodes the next frame into the ring slot for it. */
static bool fill_one(void) {
    uint32_t wh = (uint32_t)gif_w * gif_h, slot = decoded % gif_k, cs;
    uint32_t base = slot * (wh + 2);

    if (!gif_next()) return false;
    ring_px[base] = (uint8_t)gif_w;
    ring_px[base + 1] = (uint8_t)gif_h;
    memcpy(ring_px + base + 2, gif_canvas(), wh);
    to_rgb1555(gif_palette_rgb(), gif_palette_count(), ring_pal + slot * 256);
    cs = gif_delay_cs();
    ring_delay[slot] = (uint16_t)(cs > 65535 ? 65535 : cs);
    decoded++;
    return true;
}

/* Draws ring slot `slot`. */
static void draw_ring_frame(uint32_t slot) {
    uint32_t stride = (uint32_t)gif_w * gif_h + 2;
    draw_sprite(ring_px + slot * stride, ring_pal + slot * 256);
}

/* The screen shows one of two buffers, alternating each frame. Both must be
 * cleared, or the old screen (the file list) shows on every other frame. */
static void clear_both_buffers(void) {
    gfx_FillScreen(COLOR_BLACK);
    gfx_SwapDraw();
    gfx_FillScreen(COLOR_BLACK);
}

/* Leaves the GIF and frees everything it used. */
static void gif_close(void) {
    close_file();
    if (gif_loaded) gif_unload();
    gif_loaded = false;
    free(gif_buf);
    gif_buf = NULL;
    free_frames();
    gif_k = 0;
    decoded = 0;
    shown = 0;
    rle_mode = false;
    play_cur = 0;
    play_n = 0;
    animated = false;
    showing = false;
    pal_loaded = NULL;
}

void gif_viewer_close(void) {
    gif_close();
}

/* Opens a GIF. Returns false with gif_viewer_error() set if it can't be shown. */
bool gif_viewer_open(fat_t *fat, const char *fpath) {
    uint32_t size, n, wh, i;
    int r;

    gif_close();
    set_msg("Could not open GIF");
    size = read_whole_file(fat, fpath);
    if (size == 0) return false;
    if (!gif_load(gif_buf, size)) {
        set_msg(gif_error());
        gif_close();
        return false;
    }
    gif_loaded = true;
    n = gif_frame_count();
    gif_w = gif_width();
    gif_h = gif_height();
    wh = (uint32_t)gif_w * gif_h;

    r = build_rle(n, wh);
    if (r == 2) {
        gif_close();
        return false;
    }
    if (r == 0) {
        /* Every frame fits: decoding is done, playback never runs the decoder. */
        scratch = malloc(wh + 2);          /* the expanded frame, only needed from here on */
        if (!scratch) {
            set_msg("GIF: no memory for frames");
            gif_close();
            return false;
        }
        rle_mode = true;
        play_n = n;
        play_cur = 0;
        gif_unload();
        gif_loaded = false;
        free(gif_buf);
        gif_buf = NULL;
        animated = n > 1;
        expand_frame(0);
        next_due = (uint32_t)clock() + cs_ticks(fr_delay[0]);
        clear_both_buffers();
        draw_sprite(scratch, (const uint16_t *)(pals + fr_pal[0]));
        showing = true;
        return true;
    }

    /* Too big to keep every frame: decode a few ahead into a ring instead. */
    gif_unload();
    gif_loaded = false;
    if (!gif_load(gif_buf, size)) {
        set_msg(gif_error());
        gif_close();
        return false;
    }
    gif_loaded = true;
    gif_k = GIF_RING_BYTES / (wh + 256 * 2 + 2);
    if (gif_k > GIF_RING_MAX) gif_k = GIF_RING_MAX;
    if (gif_k < 1) gif_k = 1;
    if (gif_k > n) gif_k = n;
    /* Try the biggest ring first, then fewer slots if memory is short. */
    for (;;) {
        ring_px = malloc(gif_k * (wh + 2));
        ring_pal = malloc(gif_k * 256 * sizeof(uint16_t));
        ring_delay = malloc(gif_k * sizeof(uint16_t));
        if (ring_px && ring_pal && ring_delay) break;
        free(ring_px);
        free(ring_pal);
        free(ring_delay);
        ring_px = NULL;
        ring_pal = NULL;
        ring_delay = NULL;
        if (gif_k == 1) {
            set_msg("GIF: no memory for frames");
            gif_close();
            return false;
        }
        gif_k = gif_k > 2 ? 2 : 1;
    }

    decoded = 0;
    shown = 0;
    for (i = 0; i < gif_k; i++) {
        if (!fill_one()) {
            fmt(gif_msg_buf, "GIF frame %lu: %s", (unsigned long)(decoded + 1), gif_error());
            set_msg(gif_msg_buf);
            gif_close();
            return false;
        }
    }
    animated = n > 1;
    next_due = (uint32_t)clock() + cs_ticks(ring_delay[0]);
    clear_both_buffers();
    draw_ring_frame(0);
    showing = true;
    return true;
}

/* Moves to the next frame once the current one's delay is up. */
void gif_viewer_tick(void) {
    uint32_t now, next;

    if (!showing || !animated) return;
    now = (uint32_t)clock();
    if ((int32_t)(now - next_due) < 0) return;
    if (rle_mode) {
        next = (play_cur + 1) % play_n;
        expand_frame(next);
        draw_sprite(scratch, (const uint16_t *)(pals + fr_pal[next]));
        play_cur = next;
        next_due = now + cs_ticks(fr_delay[next]);
        return;
    }
    next = shown + 1;
    while (decoded <= next) {              /* normally already decoded */
        if (!fill_one()) {
            animated = false;              /* can't decode any more: keep the last frame */
            return;
        }
    }
    shown = next;
    draw_ring_frame(shown % gif_k);
    next_due = now + cs_ticks(ring_delay[shown % gif_k]);
    if (decoded < shown + gif_k) {
        if (!fill_one()) animated = false;
    }
}

/* Enter, Del or Clear goes back. Any other key is ignored. */
bool gif_viewer_key(uint8_t key) {
    if (key == sk_Enter || key == sk_Del || key == sk_Clear) {
        gif_close();
        return true;
    }
    return false;
}
