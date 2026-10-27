/* Text viewer. Keeps a window of up to VIEW_MAX bytes of a .txt file in
 * memory. When the user scrolls near either end of the window, the window
 * is moved so the text on screen stays in it, so long files are not cut off.
 * Go-to takes a percentage of the file. */

#include <tice.h>
#include <graphx.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <fatdrvce.h>
#include "fmt.h"
#include "text_viewer.h"

/* Layout and palette. These match main.c so the screen looks the same. */
#define SCREEN_W       320
#define SCREEN_H       240
#define TITLE_H        20
#define STATUS_H       14
#define C_BLACK        0x00
#define C_WHITE        0xFF
#define C_BLUE         0x32
/* Theme palette slots, the same numbers as main.c, so the viewer follows the
 * colors chosen in the settings menu. */
#define VT_BG      0x40   /* screen background */
#define VT_TOP     0x41   /* title bar */
#define VT_BOT     0x43   /* bottom bar */
#define VT_TXT     0x44   /* text */
#define VT_BOTTXT  0x45   /* bottom bar text */
#define VT_TOPTXT  0x46   /* title bar text */

static void (*overlay_fn)(void) = NULL;   /* set by main.c: battery and modifier icons */

void viewer_set_overlay(void (*fn)(void)) {
    overlay_fn = fn;
}

#define VIEW_MAX       4096          /* bytes kept in memory at once */
#define VIEW_ROWS_MAX  1024          /* wrapped rows kept in memory */
#define VIEW_COLS      43            /* most characters in one row */
#define VIEW_PX        312           /* widest a row may be in pixels (x=4 to x=316) */
#define VIEW_TOP       20            /* first text row, below the title bar */
#define VIEW_LINE_H    10
#define VIEW_VISIBLE   ((SCREEN_H - STATUS_H - VIEW_TOP) / VIEW_LINE_H)
#define FILE_BLOCK     512           /* bytes per block for fat_ReadFile */
#define VIEW_BACK      1024          /* bytes kept above the top row after a reload */
#define VIEW_PATH_MAX  144

static fat_t *vfat = NULL;           /* FAT volume the viewer reads from */
static bool viewing = false;
static char view_name[16];
static char view_path[VIEW_PATH_MAX];
static uint32_t view_size = 0;       /* full size of the file on the drive */
static uint32_t view_base = 0;       /* file offset of view_buf[0] */
static uint8_t view_block[FILE_BLOCK];
static char view_buf[VIEW_MAX + 1];
static size_t view_len = 0;          /* bytes in view_buf that rows cover */
static uint16_t view_row[VIEW_ROWS_MAX];   /* start index in view_buf of each row */
static int view_rows = 0;
static int view_top = 0;             /* first visible row */
static bool view_goto = false;       /* Graph was pressed: typing a line number */
static uint32_t goto_val = 0;        /* line number typed so far */
static int goto_digits = 0;          /* how many digits are typed */
#define GOTO_DIGITS_MAX 6
#define IDX_MAX     512          /* index entries kept */
#define IDX_STRIDE0 1024         /* bytes between index entries at first; doubles when full */
typedef struct { uint32_t off; uint32_t nl; } idx_t;   /* byte offset and newlines before it */
static idx_t idx[IDX_MAX];
static uint32_t idx_count = 0;
static uint32_t idx_stride = IDX_STRIDE0;
static uint32_t total_lines = 0;
static bool jump_by_line = true;     /* Zoom in the jump prompt switches line / percent */
static bool cache_valid = false;     /* cache_off / cache_nl hold a known position */
static uint32_t cache_off = 0;       /* a byte offset in the file ... */
static uint32_t cache_nl = 0;        /* ... and how many newlines come before it */
static fat_file_t vf;                /* the open file, kept while the viewer is open */
static bool vf_open = false;

/* Same drawing helper as main.c: sets fg, bg and transparent color together. */
static void text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    gfx_SetTextFGColor(fg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextTransparentColor(bg);
    gfx_PrintStringXY(s, x, y);
}

/* True if the name ends in one of the extensions below (any case). HTML is
 * shown as plain text, so its tags are visible. */
static bool ext_is(const char *name, const char *ext) {
    const char *dot = strrchr(name, '.');
    size_t n = strlen(ext);
    size_t i;

    if (!dot || strlen(dot) != n + 1) return false;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)dot[1 + i]) != ext[i]) return false;
    }
    return true;
}

static bool has_text_ext(const char *name) {
    return ext_is(name, "txt") || ext_is(name, "html") || ext_is(name, "htm");
}

/* Finds where the row that starts at `start` ends. A row holds at most
 * VIEW_COLS characters and VIEW_PX pixels. If a word doesn't fit, the row breaks at the space before
 * it, so words are not split. Only a word longer than a whole row is cut.
 * Returns the end of the row's text (exclusive) and sets *next to where the
 * following row starts (past any space or newline that was skipped). */
/* Pixel width of one byte as it is drawn (see viewer_draw). The font is not
 * monospaced, so rows are limited by width as well as by character count. */
static int char_px(char c) {
    if (c == '\r') return 0;
    if (c == '\t') c = ' ';
    else if (c < 32 || c > 126) c = '.';
    return gfx_GetCharWidth(c);
}

static size_t row_end(size_t start, size_t *next) {
    size_t pos = start, col = 0;
    int px = 0;

    while (pos < view_len && view_buf[pos] != '\n') {
        if (col == VIEW_COLS || px + char_px(view_buf[pos]) > VIEW_PX) {
            size_t sp = pos;   /* first byte that doesn't fit */
            if (view_buf[pos] == ' ') {          /* row ends exactly at a space */
                *next = pos + 1;
                return pos;
            }
            while (sp > start && view_buf[sp - 1] != ' ') sp--;
            if (sp > start) {                    /* break before the last word */
                *next = sp;
                return sp - 1;                   /* the space at sp-1 is dropped */
            }
            *next = pos;                         /* one long word: hard cut */
            return pos;
        }
        px += char_px(view_buf[pos]);
        pos++;
        col++;
    }
    /* Ended at a newline (consumed) or at the end of the window. */
    *next = (pos < view_len) ? pos + 1 : pos;
    return pos;
}

/* Splits view_buf into rows with row_end(). If the row table fills up, the
 * window is cut to the bytes the rows cover, so nothing is shown without a row. */
static void layout_view(void) {
    size_t pos = 0, next;

    view_rows = 0;
    while (view_rows < VIEW_ROWS_MAX) {
        view_row[view_rows++] = (uint16_t)pos;
        row_end(pos, &next);
        if (next >= view_len) return;   /* whole window laid out */
        pos = next;
    }
    view_len = pos;                     /* row table full: keep only covered bytes */
}

static void view_clamp(void) {
    int max_top = view_rows - VIEW_VISIBLE;
    if (max_top < 0) max_top = 0;
    if (view_top > max_top) view_top = max_top;
    if (view_top < 0) view_top = 0;
}

/* Opens the file once for the whole time the viewer is open. Looking up a
 * file by name searches the directory, so doing that on every seek is slow. */
static bool vopen(void) {
    if (vf_open) return true;
    if (fat_OpenFile(vfat, view_path, 0, &vf) != FAT_SUCCESS) return false;
    vf_open = true;
    view_size = fat_GetFileSize(&vf);
    return true;
}

/* Forgets the open file. The file is read-only, and its handle is a fixed
 * 64-byte struct with nothing allocated, so nothing needs closing. Skipping
 * fat_CloseFile also means no driver call is made if the drive was pulled. */
static void vclose(void) {
    vf_open = false;
}

/* Moves the open file to a block. Blocks are FILE_BLOCK bytes. */
static bool vseek_block(uint32_t block) {
    return fat_SetFileBlockOffset(&vf, block) == FAT_SUCCESS;
}

/* Reads the window of the file that starts at base (block aligned). */
static bool load_window(uint32_t base) {
    uint32_t got = 0;

    if (!vopen()) return false;
    if (base > view_size) base = view_size;
    if (!vseek_block(base / FILE_BLOCK)) return false;

    while (got + FILE_BLOCK <= VIEW_MAX && base + got < view_size) {
        uint32_t in_block = view_size - base - got;
        if (in_block > FILE_BLOCK) in_block = FILE_BLOCK;
        if (fat_ReadFile(&vf, 1, view_block) != 1) break;
        memcpy(view_buf + got, view_block, in_block);
        got += in_block;
    }

    view_base = base;
    view_len = got;
    layout_view();
    return true;
}

/* Moves the window so that the byte at file offset anchor is on screen,
 * with VIEW_BACK bytes of text above it. */
static bool view_seek(uint32_t anchor) {
    uint32_t base = anchor > VIEW_BACK ? anchor - VIEW_BACK : 0;
    int r, best = 0;

    base &= ~(uint32_t)(FILE_BLOCK - 1);
    if (!load_window(base)) return false;

    for (r = 0; r < view_rows; r++) {
        if (view_base + view_row[r] <= anchor) best = r;
        else break;
    }
    view_top = best;
    view_clamp();
    return true;
}

/* Returns the file offset of the top row. */
static uint32_t top_offset(void) {
    return view_base + view_row[view_top];
}

/* Reloads the window when scrolling is close to either end of it. */
static void view_maybe_reload(void) {
    if (view_top + 2 * VIEW_VISIBLE >= view_rows && view_base + view_len < view_size) {
        view_seek(top_offset());
    } else if (view_top < VIEW_VISIBLE && view_base > 0) {
        view_seek(top_offset());
    }
}

/* Shows a note on the bottom bar right away, before work that takes a moment. */
static void show_busy(const char *msg) {
    gfx_SetColor(VT_BOT);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    text_at(6, SCREEN_H - STATUS_H + 3, msg, VT_BOTTXT, VT_BOT);
    gfx_SwapDraw();
}

/* Shows a percentage on the bottom bar while the lines are counted. */
static void show_count_progress(uint32_t done, uint32_t total) {
    char msg[32];
    uint32_t pct = total ? (uint32_t)(((uint64_t)done * 100) / total) : 100;

    fmt(msg, "Counting lines... %lu%%", (unsigned long)pct);
    show_busy(msg);
}

/* Reads the file once to count its lines and records an index entry about
 * every idx_stride bytes: the byte offset and how many newlines come before it.
 * When the index is full, every other entry is dropped and the stride doubles,
 * so the index covers files of any size. */
static void build_index(void) {
    uint32_t pos = 0, nl = 0, n, i, j, last = 0, blocks = 0, next_rec = 0;

    idx_count = 0;
    idx_stride = IDX_STRIDE0;
    cache_valid = false;
    total_lines = 1;
    idx[idx_count].off = 0;
    idx[idx_count].nl = 0;
    idx_count++;
    if (view_size == 0 || !vopen() || !vseek_block(0)) return;
    next_rec = idx_stride;
    while (pos < view_size) {
        n = view_size - pos;
        if (n > FILE_BLOCK) n = FILE_BLOCK;
        if (fat_ReadFile(&vf, 1, view_block) != 1) break;
        if (pos >= next_rec) {
            if (idx_count == IDX_MAX) {           /* full: keep every other entry */
                for (j = 0; j < IDX_MAX / 2; j++) idx[j] = idx[2 * j];
                idx_count = IDX_MAX / 2;
                idx_stride *= 2;
            }
            idx[idx_count].off = pos;
            idx[idx_count].nl = nl;
            idx_count++;
            next_rec = pos + idx_stride;
        }
        for (i = 0; i < n; i++) {
            if (view_block[i] == '\n') nl++;
        }
        last = view_block[n - 1];
        pos += n;
        if ((++blocks & 63) == 0) show_count_progress(pos, view_size);
    }
    total_lines = nl + 1;
    if (last == '\n' && nl > 0) total_lines = nl;   /* a trailing newline doesn't start a new line */
}

/* Index entry with the largest key that is at most key (or below it when
 * strict). The key is a byte offset, or a newline count when by_nl is set. */
static uint32_t idx_find(uint32_t key, bool by_nl, bool strict) {
    uint32_t lo = 0, hi = idx_count - 1, mid, v;

    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        v = by_nl ? idx[mid].nl : idx[mid].off;
        if (strict ? v < key : v <= key) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* The line number (1-based) that holds byte offset off. Counts newlines from
 * the nearest known position, which is at most idx_stride bytes away. */
static uint32_t line_at(uint32_t off) {
    uint32_t k = idx_find(off, false, false);
    uint32_t start = idx[k].off, nl = idx[k].nl, pos, i, abs;
    bool ok = true;

    if (cache_valid && cache_off <= off && cache_off >= start) {
        start = cache_off;
        nl = cache_nl;
    }
    if (off > start) {
        ok = false;
        if (vopen() && vseek_block(start / FILE_BLOCK)) {
            ok = true;
            pos = (start / FILE_BLOCK) * FILE_BLOCK;
            while (pos < off) {
                if (fat_ReadFile(&vf, 1, view_block) != 1) { ok = false; break; }
                for (i = 0; i < FILE_BLOCK; i++) {
                    abs = pos + i;
                    if (abs >= off) break;
                    if (abs >= start && view_block[i] == '\n') nl++;
                }
                pos += FILE_BLOCK;
            }
        }
    }
    if (ok) {
        cache_off = off;
        cache_nl = nl;
        cache_valid = true;
    }
    return nl + 1;
}

/* Finds where line number line starts. If the file is shorter, gives the start
 * of its last line. Scans from the last index entry before the line, so it
 * reads about idx_stride bytes at most. */
static bool find_line(uint32_t line, uint32_t *offset) {
    uint32_t want, k, nl, start, pos, i, n, abs, last_start;
    bool found = false;

    if (line <= 1) {
        *offset = 0;
        return true;
    }
    want = line - 1;                        /* newlines to pass before the target line */
    k = idx_find(want, true, true);         /* last entry with fewer newlines than want */
    nl = idx[k].nl;
    start = idx[k].off;
    last_start = start;
    if (!vopen() || !vseek_block(start / FILE_BLOCK)) return false;
    pos = (start / FILE_BLOCK) * FILE_BLOCK;
    while (!found && pos < view_size) {
        n = view_size - pos;
        if (n > FILE_BLOCK) n = FILE_BLOCK;
        if (fat_ReadFile(&vf, 1, view_block) != 1) break;
        for (i = 0; i < n && !found; i++) {
            abs = pos + i;
            if (abs < start) continue;
            if (view_block[i] == '\n') {
                nl++;
                last_start = abs + 1;
                if (nl == want) found = true;
            }
        }
        pos += FILE_BLOCK;
    }
    *offset = last_start;
    return true;
}

/* Jumps to a line number. */
static void jump_to_line(uint32_t line) {
    uint32_t off = 0;

    show_busy("Finding line...");
    if (find_line(line, &off)) view_seek(off);
}

/* Jumps to a percentage of the file. */
static void jump_to_percent(uint32_t p) {
    uint32_t target;

    if (view_size == 0) return;
    target = (view_size / 100) * p + ((view_size % 100) * p) / 100;
    if (target >= view_size) target = view_size - 1;
    view_seek(target);
}

static void view_goto_key(uint8_t key) {
    int d = -1;

    switch (key) {
        case sk_0: d = 0; break;
        case sk_1: d = 1; break;
        case sk_2: d = 2; break;
        case sk_3: d = 3; break;
        case sk_4: d = 4; break;
        case sk_5: d = 5; break;
        case sk_6: d = 6; break;
        case sk_7: d = 7; break;
        case sk_8: d = 8; break;
        case sk_9: d = 9; break;
        default:   break;
    }

    if (d >= 0) {
        uint32_t next = goto_val * 10 + (uint32_t)d;
        int max_digits = jump_by_line ? GOTO_DIGITS_MAX : 3;
        if (goto_digits < max_digits && (jump_by_line || next <= 100)) {
            goto_val = next;
            goto_digits++;
        }
    } else if (key == sk_Del) {
        goto_val /= 10;
        if (goto_digits > 0) goto_digits--;
    } else if (key == sk_Enter) {
        if (goto_val > 0) {
            if (jump_by_line) jump_to_line(goto_val);
            else jump_to_percent(goto_val);
        }
        view_goto = false;
    } else if (key == sk_Zoom) {
        jump_by_line = !jump_by_line;   /* switch between line and percent */
        goto_val = 0;
        goto_digits = 0;
    } else if (key == sk_Clear || key == sk_Graph) {
        view_goto = false;              /* Clear or Graph backs out of the jump */
    }
}

bool viewer_can_open(const char *name) {
    return has_text_ext(name);
}

bool viewer_open(fat_t *fat, const char *fpath, const char *name) {
    vfat = fat;
    strncpy(view_path, fpath, sizeof(view_path) - 1);
    view_path[sizeof(view_path) - 1] = 0;

    vclose();
    if (!vopen() || !view_seek(0)) {
        vclose();
        return false;
    }
    show_busy("Counting lines...");
    build_index();

    strncpy(view_name, name, sizeof(view_name) - 1);
    view_name[sizeof(view_name) - 1] = 0;
    view_goto = false;
    viewing = true;
    return true;
}

bool viewer_is_open(void) {
    return viewing;
}

/* Up/Down scroll a row, Left/Right a page, Enter, Del or Clear goes back. */
bool viewer_key(uint8_t key) {
    if (view_goto) {
        view_goto_key(key);
        return true;
    }
    if (key == sk_Graph) {
        view_goto = true;
        goto_val = 0;
        goto_digits = 0;
        return true;
    }
    switch (key) {
        case sk_Up:    view_top--; break;
        case sk_Down:  view_top++; break;
        case sk_Left:  view_top -= VIEW_VISIBLE; break;
        case sk_Right: view_top += VIEW_VISIBLE; break;
        case sk_Enter:
        case sk_Del:
        case sk_Clear:
            viewing = false;
            return true;
        default:
            return false;
    }
    view_clamp();
    view_maybe_reload();
    return true;
}

void viewer_draw(void) {
    char line[VIEW_COLS + 1];
    char info[40];
    int i, n;
    size_t k, end;
    uint32_t pct = 100;

    gfx_FillScreen(VT_BG);

    /* Title bar carries the file name as a header. */
    gfx_SetColor(VT_TOP);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    fmt(info, "View: %s", view_name);
    text_at(6, 6, info, VT_TOPTXT, VT_TOP);

    for (i = 0; i < VIEW_VISIBLE; i++) {
        int row = view_top + i;
        if (row >= view_rows) break;
        size_t after;
        k = view_row[row];
        end = row_end(k, &after);
        n = 0;
        while (k < end) {
            char c = view_buf[k++];
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            else if (c < 32 || c > 126) c = '.';
            line[n++] = c;
        }
        line[n] = 0;
        text_at(4, VIEW_TOP + i * VIEW_LINE_H, line, VT_TXT, VT_BG);
    }
    if (view_len == 0) {
        text_at(4, VIEW_TOP, "(empty file)", VT_TXT, VT_BG);
    }

    gfx_SetColor(VT_BOT);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    if (view_goto) {
        if (jump_by_line) {
            if (goto_digits) fmt(info, "Jump to line: %lu_  Zoom: %%", (unsigned long)goto_val);
            else fmt(info, "Jump to line: _  Zoom: %%");
        } else {
            if (goto_digits) fmt(info, "Jump to %%: %lu_  Zoom: line", (unsigned long)goto_val);
            else fmt(info, "Jump to %%: _  Zoom: line");
        }
    } else {
        if (view_size > 0) pct = (top_offset() * 100) / view_size;
        if (total_lines > 0) {
            fmt(info, "Line %lu of %lu  %d%%", (unsigned long)line_at(top_offset()),
                    (unsigned long)total_lines, (int)pct);
        } else {
            fmt(info, "%d%%", (int)pct);
        }
    }
    text_at(6, SCREEN_H - STATUS_H + 3, info, VT_BOTTXT, VT_BOT);
    {
        /* "Jump" sits over the Graph key, the fifth of five columns. */
        int jw = (int)gfx_GetStringWidth("Jump");
        text_at(SCREEN_W - SCREEN_W / 10 - jw / 2, SCREEN_H - STATUS_H + 3, "Jump", VT_BOTTXT, VT_BOT);
    }

    if (overlay_fn) overlay_fn();   /* battery and modifier icons, on top of the title bar */
    gfx_SwapDraw();
}

void viewer_close(void) {
    viewing = false;
    view_goto = false;
    cache_valid = false;
    vclose();
}
