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
#include "text_viewer.h"

/* Layout and palette. These match main.c so the screen looks the same. */
#define SCREEN_W       320
#define SCREEN_H       240
#define TITLE_H        20
#define STATUS_H       14
#define C_BLACK        0x00
#define C_WHITE        0xFF
#define C_BLUE         0x32

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
static bool view_goto = false;       /* Graph was pressed: typing a percentage */
static int goto_val = 0;             /* percentage typed so far */
static int goto_digits = 0;          /* how many digits are typed */

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

/* Reads the window of the file that starts at base (block aligned). */
static bool load_window(uint32_t base) {
    fat_file_t f;
    uint32_t got = 0;

    if (fat_OpenFile(vfat, view_path, 0, &f) != FAT_SUCCESS) return false;
    view_size = fat_GetFileSize(&f);
    if (base > view_size) base = view_size;
    if (fat_SetFileBlockOffset(&f, base / FILE_BLOCK) != FAT_SUCCESS) {
        fat_CloseFile(&f);
        return false;
    }

    while (got + FILE_BLOCK <= VIEW_MAX && base + got < view_size) {
        uint32_t in_block = view_size - base - got;
        if (in_block > FILE_BLOCK) in_block = FILE_BLOCK;
        if (fat_ReadFile(&f, 1, view_block) != 1) break;
        memcpy(view_buf + got, view_block, in_block);
        got += in_block;
    }
    fat_CloseFile(&f);

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
        if (goto_digits < 3 && goto_val * 10 + d <= 100) {
            goto_val = goto_val * 10 + d;
            goto_digits++;
        }
    } else if (key == sk_Del) {
        goto_val /= 10;
        if (goto_digits > 0) goto_digits--;
    } else if (key == sk_Enter) {
        if (goto_val > 0 && view_size > 0) {
            uint32_t target = (uint32_t)((view_size / 100) * (uint32_t)goto_val
                                         + ((view_size % 100) * (uint32_t)goto_val) / 100);
            if (target >= view_size) target = view_size - 1;
            view_seek(target);
        }
        view_goto = false;
    } else if (key == sk_Graph) {
        view_goto = false;
    }
}

bool viewer_can_open(const char *name) {
    return has_text_ext(name);
}

bool viewer_open(fat_t *fat, const char *fpath, const char *name) {
    vfat = fat;
    strncpy(view_path, fpath, sizeof(view_path) - 1);
    view_path[sizeof(view_path) - 1] = 0;

    if (!view_seek(0)) return false;

    strncpy(view_name, name, sizeof(view_name) - 1);
    view_name[sizeof(view_name) - 1] = 0;
    view_goto = false;
    viewing = true;
    return true;
}

bool viewer_is_open(void) {
    return viewing;
}

/* Up/Down scroll a row, Left/Right a page, Enter or Del goes back. */
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

    gfx_FillScreen(C_WHITE);

    /* Title bar carries the file name as a header. */
    gfx_SetColor(C_BLUE);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    sprintf(info, "View: %s", view_name);
    text_at(6, 6, info, C_WHITE, C_BLUE);

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
        text_at(4, VIEW_TOP + i * VIEW_LINE_H, line, C_BLACK, C_WHITE);
    }
    if (view_len == 0) {
        text_at(4, VIEW_TOP, "(empty file)", C_BLACK, C_WHITE);
    }

    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    if (view_goto) {
        sprintf(info, "Go to %%: %d_", goto_val);
    } else {
        if (view_size > 0) pct = (top_offset() * 100) / view_size;
        sprintf(info, "%d%%", (int)pct);
    }
    text_at(6, SCREEN_H - STATUS_H + 3, info, C_WHITE, C_BLACK);
    text_at(SCREEN_W - 88, SCREEN_H - STATUS_H + 3,
            view_goto ? "Enter: jump" : "Enter: back", C_WHITE, C_BLACK);

    gfx_SwapDraw();
}

void viewer_close(void) {
    viewing = false;
    view_goto = false;
}
