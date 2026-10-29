/* Text editor. See text_editor.h for how it works. */

#include <tice.h>
#include <graphx.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <fatdrvce.h>
#include "keys.h"
#include "text_editor.h"
#include "fmt.h"

#define EDIT_MAX    16384              /* biggest file the editor will hold */
#define SCREEN_W    320
#define SCREEN_H    240
#define TITLE_H     20                 /* title bar: file name, like the viewer */
#define STATUS_H    14                 /* bottom bar: status and keys, like main.c */
#define X0          4                  /* line numbers start here */
#define X1          (SCREEN_W - 4)     /* text wraps before here */
#define Y0          (TITLE_H + 2)
#define ROW_H       10
#define ROWS        ((SCREEN_H - STATUS_H - Y0) / ROW_H)
#define JUMP_DIGITS_MAX 6              /* most digits typed for a line number */

/* Palette entries, the same as the text viewer's */
#define C_BG        0x40
#define C_TOP       0x41
#define C_TXT       0x44
#define C_TOPTXT    0x46
#define C_BOT       0x43
#define C_BOTTXT    0x45

static uint8_t *buf = NULL;                /* the text */
static uint32_t len = 0;                   /* bytes of text */
static uint32_t cursor = 0;                /* insert position, 0..len */
static int top = 0;                        /* first screen row shown */
static uint32_t top_byte = 0;              /* file offset of the first row shown (set while drawing) */
static bool dirty = false;                 /* changed since the last save */
static bool leave_armed = false;           /* Clear was pressed once with unsaved changes */
static bool open_now = false;
static fat_t *efat = NULL;
static char epath[160];
#define MSG_MS      2500               /* a status message stays this long, like main.c's */
static char msg[48] = "";                  /* message on the status line */
static uint32_t msg_at = 0;                /* clock() value when the message was set */
static bool msg_timed = false;             /* msg is showing and will expire */
static bool jump_open = false;             /* Trace was pressed: typing a line or percent */
static bool jump_by_line = true;           /* Zoom in the jump prompt switches line / percent */
static bool jump_top = false;              /* a jump just moved the cursor: put its row at the top */
static uint32_t jump_val = 0;              /* the number typed so far */
static int jump_digits = 0;                /* how many digits are typed */
static const char *err = "";
static void (*overlay_fn)(void) = NULL;   /* set by main.c: battery, modifier icons, colors */
static uint8_t sblk[FAT_BLOCK_SIZE];
static uint32_t nl_total = 0;              /* newlines in the text, kept up to date on every edit */
static uint32_t top_line = 1;              /* line number of the first row shown (set while drawing) */
static bool anchor_ok = false;             /* the anchor below is a known row start: layout begins there */
static uint32_t anchor_off = 0;            /* byte offset of a row start at or above the top row */
static uint32_t anchor_line = 1;           /* the line number at that offset */
static int anchor_row = 0;                 /* that row's index, counted from the top of the file */
static bool anchor_fresh = true;           /* the anchor starts a line (so it gets a number), not a wrapped row */
static uint8_t cw_tab[128];                /* pixel width of each character, 0 = not looked up yet */
static char rowtxt[ROWS][48];              /* the text of each visible row, filled in by layout() */
static char rownum[ROWS][8];               /* the line number shown on each row ("" if none) */
static uint8_t rowlen[ROWS];
/* What the last frame drew on each row, so the next frame only redraws rows that changed. */
static char prev_txt[ROWS][48];
static char prev_num[ROWS][8];
static int prev_cur_x[ROWS];               /* x of the cursor bar on that row, -1 if none */
static bool prev_ok[ROWS];                 /* the prev_ entries describe what is on screen */
static int prev_top = 0;                   /* top row of the last frame */
static bool frame_ready = false;           /* the screen holds a frame the editor drew */
static bool force_full = false;            /* the next frame must redraw everything */

/* Pixel width of a character, looked up once and then kept in cw_tab. */
static int char_w(char c) {
    uint8_t k = (uint8_t)c & 0x7F;

    if (!cw_tab[k]) cw_tab[k] = (uint8_t)gfx_GetCharWidth((char)k);
    return cw_tab[k];
}

static void set_msg(const char *s) {
    strncpy(msg, s, sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = 0;
    msg_at = (uint32_t)clock();
    msg_timed = true;
}

static void clear_msg(void) {
    msg[0] = 0;
    msg_timed = false;
}

/* Clears the status message once its time is up. Returns true if it just cleared,
 * so main.c knows to redraw. */
bool editor_tick(void) {
    if (!msg_timed) return false;
    if ((uint32_t)clock() - msg_at < (uint32_t)MSG_MS * CLOCKS_PER_SEC / 1000UL) return false;
    clear_msg();
    leave_armed = false;                  /* the warning ran out: Clear asks again */
    return true;
}

const char *editor_error(void) {
    return err;
}

void editor_set_overlay(void (*fn)(void)) {
    overlay_fn = fn;
}

bool editor_is_open(void) {
    return open_now;
}

bool editor_can_open(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return tolower((unsigned char)dot[1]) == 't' &&
           tolower((unsigned char)dot[2]) == 'x' &&
           tolower((unsigned char)dot[3]) == 't' &&
           dot[4] == 0;
}

/* Width of the line-number column: the widest number in the file, then a space. */
static int gutter_width(void) {
    uint32_t n = nl_total + 1;
    int digits = 1;

    for (; n >= 10; n /= 10) digits++;
    return (digits + 1) * char_w('0');
}

/* Walks the text the way it is drawn: each line starts with its number, and
 * long lines wrap at the screen edge (the wrapped rows get no number). Sets
 * *cur_row and *cur_x to where the cursor is. When draw is true, paints the
 * rows that are on screen and records top_byte. */
static void layout(bool draw, int tx, int *cur_row, int *cur_x) {
    uint32_t i, line_no = 1;
    int row = 0, x = tx, r;
    bool fresh_line = true, top_seen = false;

    /* Start from the anchor when it is still valid, so the walk covers only the text
     * from there down, not the whole file. */
    if (anchor_ok && (cursor < anchor_off || anchor_row > top)) anchor_ok = false;
    if (anchor_ok) {
        i = anchor_off;
        row = anchor_row;
        line_no = anchor_line;
        fresh_line = anchor_fresh;
    } else {
        i = 0;
    }
    if (draw) {
        for (r = 0; r < ROWS; r++) {
            rowlen[r] = 0;
            rownum[r][0] = 0;
        }
    }
    for (; ; i++) {
        uint8_t c;
        int w;

        if (i == cursor) {
            *cur_row = row;
            *cur_x = x;
        }
        if (draw && !top_seen && row == top) {
            top_byte = i;
            top_line = line_no;
            top_seen = true;
        }
        /* Stop once the cursor is placed and nothing below it is on screen. This keeps
         * redraws cheap: only the text above the visible rows is walked, not the whole file. */
        if (i >= cursor && (!draw || row >= top + ROWS)) break;
        if (fresh_line) {                 /* the line number, once per line */
            fresh_line = false;
            if (draw && row <= top) {     /* a row start above the screen: the next anchor */
                anchor_ok = true;
                anchor_off = i;
                anchor_row = row;
                anchor_line = line_no;
                anchor_fresh = true;
            }
            if (draw && row >= top && row < top + ROWS) {
                fmt(rownum[row - top], "%d", (int)line_no);
            }
        }
        if (i >= len) break;
        c = buf[i];
        if (c == '\n') {
            row++;
            x = tx;
            line_no++;
            fresh_line = true;
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 32 || c > 126) c = '?';
        w = char_w((char)c);
        if (x + w > X1) {                 /* too wide for this row: wrap */
            row++;
            x = tx;
            if (draw && row <= top) {     /* a wrapped row starts here: the next anchor */
                anchor_ok = true;
                anchor_off = i;
                anchor_row = row;
                anchor_line = line_no;
                anchor_fresh = false;
            }
        }
        if (draw && row >= top && row < top + ROWS && rowlen[row - top] < sizeof(rowtxt[0]) - 1) {
            rowtxt[row - top][rowlen[row - top]++] = (char)c;
        }
        x += w;
    }
    if (draw) {
        for (r = 0; r < ROWS; r++) rowtxt[r][rowlen[r]] = 0;
    }
}

/* Scrolls so the cursor's row is on screen. After a jump, the cursor's row goes to the top. */
static void follow_cursor(void) {
    int r, x;

    layout(false, X0 + gutter_width(), &r, &x);
    if (jump_top) {
        top = r;
        jump_top = false;
    } else if (r < top) {
        top = r;
    } else if (r >= top + ROWS) {
        top = r - ROWS + 1;
    }
}

/* The file's name, without its folder. */
static const char *file_name(void) {
    const char *slash = strrchr(epath, '/');
    return slash ? slash + 1 : epath;
}

static uint32_t total_lines(void) {
    return nl_total + 1;
}

/* Draws a key's label centered under its key (col 0 is Y=, col 4 is Graph). */
static void bottom_label(int col, const char *s) {
    int w = (int)gfx_GetStringWidth(s);
    gfx_PrintStringXY(s, col * (SCREEN_W / 5) + (SCREEN_W / 5) / 2 - w / 2, SCREEN_H - STATUS_H + 3);
}

void editor_draw(void) {
    int tx = X0 + gutter_width();
    int cr = 0, cx = 0, k, r;
    char info[48];
    uint32_t pct;
    bool full;

    if (!open_now) return;
    follow_cursor();
    layout(true, tx, &cr, &cx);
    k = top - prev_top;
    full = force_full || !frame_ready || k < 0 || k >= ROWS;   /* scrolling up redraws all */
    force_full = false;
    gfx_SetTextBGColor(C_BG);
    gfx_SetTextTransparentColor(C_BG);   /* text drawn over the background, as in the viewer */
    gfx_SetTextFGColor(C_TXT);
    if (full) {
        gfx_FillScreen(C_BG);
        for (r = 0; r < ROWS; r++) prev_ok[r] = false;
    } else {
        gfx_BlitScreen();                /* start from the frame on screen */
        if (k > 0) {                     /* scrolled down: move the rows that stay on screen up */
            int s;

            gfx_CopyRectangle(gfx_buffer, gfx_buffer, 0, (uint8_t)(Y0 + k * ROW_H),
                              0, Y0, SCREEN_W, (uint8_t)((ROWS - k) * ROW_H));
            for (s = 0; s < ROWS; s++) {
                if (s + k < ROWS) {
                    strcpy(prev_txt[s], prev_txt[s + k]);
                    strcpy(prev_num[s], prev_num[s + k]);
                    prev_cur_x[s] = prev_cur_x[s + k];
                    prev_ok[s] = prev_ok[s + k];
                } else {
                    prev_ok[s] = false;
                }
            }
        }
    }
    /* Rows: redraw only the ones whose text, number or cursor bar changed. */
    for (r = 0; r < ROWS; r++) {
        int here = (cr - top == r) ? cx : -1;

        if (prev_ok[r] && here == prev_cur_x[r] &&
            strcmp(prev_txt[r], rowtxt[r]) == 0 && strcmp(prev_num[r], rownum[r]) == 0) {
            continue;
        }
        if (!full) {                     /* erase the old row before drawing it again */
            gfx_SetColor(C_BG);
            gfx_FillRectangle(0, Y0 + r * ROW_H, SCREEN_W, ROW_H);
        }
        if (rownum[r][0]) gfx_PrintStringXY(rownum[r], X0, Y0 + r * ROW_H);
        if (rowlen[r]) gfx_PrintStringXY(rowtxt[r], tx, Y0 + r * ROW_H);
        if (here >= 0) {                 /* cursor: a short bar */
            gfx_SetColor(C_TXT);
            gfx_VertLine(here, Y0 + r * ROW_H, 8);
        }
        strcpy(prev_txt[r], rowtxt[r]);
        strcpy(prev_num[r], rownum[r]);
        prev_cur_x[r] = here;
        prev_ok[r] = true;
    }

    /* Title bar: the file name. Redrawn every frame, it is small. */
    gfx_SetColor(C_TOP);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    gfx_SetTextBGColor(C_TOP);
    gfx_SetTextFGColor(C_TOPTXT);
    gfx_PrintStringXY(file_name(), 6, 6);

    /* Bottom bar, like main.c: status on the left, keys on the right. */
    gfx_SetColor(C_BOT);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    gfx_SetTextBGColor(C_BOT);
    gfx_SetTextFGColor(C_BOTTXT);
    if (msg[0]) {
        /* A status message replaces the keys until it times out or a key clears it. */
        gfx_PrintStringXY(msg, 4, SCREEN_H - STATUS_H + 3);
    } else if (jump_open) {
        /* Same prompt as the text viewer's jump. */
        if (jump_by_line) {
            if (jump_digits) fmt(info, "Jump to line: %lu_  Zoom: %%", (unsigned long)jump_val);
            else fmt(info, "Jump to line: _  Zoom: %%");
        } else {
            if (jump_digits) fmt(info, "Jump to %%: %lu_  Zoom: line", (unsigned long)jump_val);
            else fmt(info, "Jump to %%: _  Zoom: line");
        }
        gfx_PrintStringXY(info, 4, SCREEN_H - STATUS_H + 3);
    } else {
        pct = len ? (top_byte * 100) / len : 100;
        fmt(info, "%sLine %lu of %lu  %d%%", dirty ? "Edited  " : "",
            (unsigned long)top_line, (unsigned long)total_lines(), (int)pct);
        gfx_PrintStringXY(info, 4, SCREEN_H - STATUS_H + 3);
        bottom_label(3, "Jump");
        bottom_label(4, "Save");
    }

    if (overlay_fn) overlay_fn();
    gfx_SwapDraw();
    prev_top = top;
    frame_ready = true;
}

void editor_force_full(void) {
    force_full = true;
}

static bool insert_char(char c) {
    if (len >= EDIT_MAX) {
        set_msg("Full: 16 KB max");
        return false;
    }
    if (cursor <= anchor_off) anchor_ok = false;  /* the change is at or above the anchor */
    memmove(buf + cursor + 1, buf + cursor, len - cursor);
    buf[cursor] = (uint8_t)c;
    cursor++;
    len++;
    if (c == '\n') nl_total++;
    dirty = true;
    return true;
}

static void backspace(void) {
    if (cursor == 0) return;
    if (buf[cursor - 1] == '\n') nl_total--;
    if (cursor <= anchor_off) anchor_ok = false;  /* the change is above the anchor */
    memmove(buf + cursor - 1, buf + cursor, len - cursor);
    cursor--;
    len--;
    dirty = true;
}

static uint32_t line_start(uint32_t p) {
    while (p > 0 && buf[p - 1] != '\n') p--;
    return p;
}

static uint32_t line_end(uint32_t p) {
    while (p < len && buf[p] != '\n') p++;
    return p;
}

/* Up and Down move by real lines (a long line that wraps counts as one). */
static void move_up(void) {
    uint32_t ls = line_start(cursor), col = cursor - ls, pe, ps;

    if (ls == 0) return;
    pe = ls - 1;                           /* the newline ending the line above */
    ps = line_start(pe);
    cursor = ps + col;
    if (cursor > pe) cursor = pe;
}

static void move_down(void) {
    uint32_t ls = line_start(cursor), col = cursor - ls, le = line_end(cursor), ns, ne;

    if (le >= len) return;
    ns = le + 1;
    ne = line_end(ns);
    cursor = ns + col;
    if (cursor > ne) cursor = ne;
}

/* Alpha + Up/Down move a page (as many lines as the screen shows). */
static void move_page(int dir) {
    int k;

    for (k = 0; k < ROWS; k++) {
        if (dir < 0) move_up();
        else move_down();
    }
}

/* Writes the whole text over the file at path. Returns true if it all went out. */
static bool write_text(const char *path) {
    fat_file_t f;
    uint32_t off = 0, n;
    bool ok;

    if (fat_OpenFile(efat, path, 0, &f) != FAT_SUCCESS) return false;
    ok = fat_SetFileSize(&f, len) == FAT_SUCCESS;
    while (ok && off < len) {
        n = len - off;
        if (n > FAT_BLOCK_SIZE) n = FAT_BLOCK_SIZE;
        memset(sblk, 0, FAT_BLOCK_SIZE);
        memcpy(sblk, buf + off, n);
        if (fat_WriteFile(&f, 1, sblk) != 1) ok = false;
        off += n;
    }
    fat_CloseFile(&f);
    return ok;
}

static bool save_now(void) {
    if (!write_text(epath)) {
        set_msg("Save failed");
        return false;
    }
    dirty = false;
    set_msg("Saved");
    return true;
}

bool editor_is_dirty(void) {
    return open_now && dirty;
}

/* Saves a copy of the text as AUTOSAV1.TXT (or the next free number up to 9),
 * in the same folder as the file. The original is left as it is. name_out gets
 * the name used. */
bool editor_autosave(char *name_out, size_t size) {
    char dir[160];
    char name[16];
    char path[176];
    const char *slash;
    fat_file_t f;
    int k;

    if (!open_now || !dirty) return false;
    slash = strrchr(epath, '/');
    if (!slash || (size_t)(slash - epath + 1) >= sizeof(dir)) return false;
    memcpy(dir, epath, slash - epath + 1);
    dir[slash - epath + 1] = 0;
    for (k = 1; k <= 9; k++) {
        fmt(name, "AUTOSAV%d.TXT", k);
        strcpy(path, dir);
        strcat(path, name);
        if (fat_OpenFile(efat, path, 0, &f) == FAT_SUCCESS) {   /* already there: next number */
            fat_CloseFile(&f);
            continue;
        }
        if (fat_Create(efat, dir, name, 0) != FAT_SUCCESS) return false;
        if (!write_text(path)) return false;
        strncpy(name_out, name, size - 1);
        name_out[size - 1] = 0;
        return true;
    }
    return false;
}

/* Moves the cursor to the start of a line number (clamped to the file). */
static void jump_to_line(uint32_t n) {
    uint32_t lines = 1, off = 0, i;

    if (n < 1) n = 1;
    for (i = 0; i < len && lines < n; i++) {
        if (buf[i] == '\n') {
            lines++;
            off = i + 1;
        }
    }
    cursor = off;
    jump_top = true;
}

/* Moves the cursor to a percentage of the file, to the start of that line. */
static void jump_to_percent(uint32_t p) {
    if (p > 100) p = 100;
    cursor = line_start((len * p) / 100);
    jump_top = true;
}

/* Keys while the jump prompt is open. Digits type a number, Del removes one,
 * Zoom switches line / percent, Enter jumps, Clear or Trace backs out. */
static void jump_key(uint8_t key) {
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
        uint32_t next = jump_val * 10 + (uint32_t)d;
        int max_digits = jump_by_line ? JUMP_DIGITS_MAX : 3;
        if (jump_digits < max_digits && (jump_by_line || next <= 100)) {
            jump_val = next;
            jump_digits++;
        }
    } else if (key == sk_Del) {
        jump_val /= 10;
        if (jump_digits > 0) jump_digits--;
    } else if (key == sk_Enter) {
        if (jump_val > 0) {
            if (jump_by_line) jump_to_line(jump_val);
            else jump_to_percent(jump_val);
        }
        jump_open = false;
    } else if (key == sk_Zoom) {
        jump_by_line = !jump_by_line;
        jump_val = 0;
        jump_digits = 0;
    } else if (key == sk_Clear || key == sk_Trace) {
        jump_open = false;
    }
}

bool editor_open(fat_t *fat, const char *fpath, bool is_new) {
    fat_file_t f;
    uint32_t size, got = 0;

    editor_close();
    err = "";
    if (strlen(fpath) >= sizeof(epath)) {
        err = "Edit: path too long";
        return false;
    }
    buf = malloc(EDIT_MAX);
    if (!buf) {
        err = "Edit: no memory";
        return false;
    }
    if (!is_new) {
        if (fat_OpenFile(fat, fpath, 0, &f) != FAT_SUCCESS) {
            err = "Edit: can't open file";
            goto fail;
        }
        size = fat_GetFileSize(&f);
        if (size > EDIT_MAX) {
            fat_CloseFile(&f);
            err = "Edit: over 16 KB";
            goto fail;
        }
        while (got < size) {
            uint32_t n = size - got;
            if (n > FAT_BLOCK_SIZE) n = FAT_BLOCK_SIZE;
            if (fat_ReadFile(&f, 1, sblk) != 1) break;
            memcpy(buf + got, sblk, n);
            got += n;
        }
        fat_CloseFile(&f);
        if (got != size) {
            err = "Edit: read failed";
            goto fail;
        }
        len = size;
    }
    nl_total = 0;
    anchor_ok = false;
    frame_ready = false;
    {
        uint32_t i;
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n') nl_total++;
        }
    }
    efat = fat;
    strncpy(epath, fpath, sizeof(epath) - 1);
    epath[sizeof(epath) - 1] = 0;
    cursor = 0;
    top = 0;
    top_byte = 0;
    dirty = false;
    leave_armed = false;
    jump_open = false;
    jump_top = false;
    jump_by_line = true;
    clear_msg();
    open_now = true;
    return true;

fail:
    free(buf);
    buf = NULL;
    len = 0;
    return false;
}

void editor_close(void) {
    free(buf);
    buf = NULL;
    len = 0;
    nl_total = 0;
    top_line = 1;
    anchor_ok = false;
    frame_ready = false;
    cursor = 0;
    top = 0;
    top_byte = 0;
    dirty = false;
    leave_armed = false;
    jump_open = false;
    jump_top = false;
    open_now = false;
    efat = NULL;
}

/* Handles one key press. typed is the character the key gave, or 0. */
enum edit_result editor_key(uint8_t key, char typed, bool page, bool second) {
    bool was_armed = leave_armed;

    if (!open_now) return EDIT_NONE;
    leave_armed = false;
    /* Any key but Clear and Graph clears the status message. */
    if (typed || (key != sk_Clear && key != sk_Graph)) clear_msg();
    if (jump_open) {                      /* the jump prompt takes the keys until it closes */
        if (!typed) jump_key(key);
        return EDIT_NONE;
    }
    if (typed) {
        insert_char(typed);
        return EDIT_NONE;
    }
    switch (key) {
        case sk_Left:
            if (second) cursor = line_start(cursor);   /* 2nd + Left: start of the line */
            else if (cursor > 0) cursor--;
            break;
        case sk_Right:
            if (second) cursor = line_end(cursor);     /* 2nd + Right: end of the line */
            else if (cursor < len) cursor++;
            break;
        case sk_Up:    if (page) move_page(-1); else move_up(); break;
        case sk_Down:  if (page) move_page(1); else move_down(); break;
        case sk_Trace:
            jump_open = true;
            jump_val = 0;
            jump_digits = 0;
            break;
        case sk_Zoom:
            jump_by_line = !jump_by_line;
            set_msg(jump_by_line ? "Jump by line" : "Jump by percent");
            break;
        case sk_Enter: insert_char('\n'); break;
        case sk_Del:   backspace(); break;
        case sk_Graph:
            if (save_now()) return EDIT_SAVED;
            break;
        case sk_Clear:
            if (dirty && !was_armed) {
                leave_armed = true;
                set_msg("Press Clear again to exit without saving.");
                break;
            }
            editor_close();
            return EDIT_CLOSED;
        default:
            break;
    }
    return EDIT_NONE;
}
