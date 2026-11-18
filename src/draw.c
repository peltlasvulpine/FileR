#include "filer.h"

/* ---------- Drawing ---------- */

/* Size of the selected USB file, if the list is on the drive and a file is highlighted. */
bool selected_drive_file_size(uint32_t *size) {
    fat_file_t f;
    char fpath[PATH_MAX + 16];
    item_t *it;

    if (location != LOC_DRIVE || item_count == 0) return false;
    it = &items[selected];
    if (it->kind != KIND_FILE) return false;
    if (strcmp(drive_path, "/") == 0) {
        fmt(fpath, "/%s", it->name);
    } else {
        fmt(fpath, "%s/%s", drive_path, it->name);
    }
    if (fat_OpenFile(&fat, fpath, 0, &f) != FAT_SUCCESS) return false;
    *size = fat_GetFileSize(&f);
    fat_CloseFile(&f);
    return true;
}

/* Size of the selected item in bytes: a calculator variable (its VAT data
 * size), or a USB file. Returns false when nothing is selected or sized. */
/* Last calculator variable sized, so redraws don't reopen it. Cleared with the icon cache. */
static bool size_cached = false;
static char size_name[8];
static uint8_t size_type;
static uint32_t size_value;

bool selected_size(uint32_t *size) {
    item_t *it;
    uint8_t h;

    if (item_count == 0) return false;
    it = &items[selected];
    if (location == LOC_CALC && it->kind == KIND_VAR) {
        if (size_cached && size_type == it->type &&
            memcmp(size_name, it->name, sizeof(size_name)) == 0) {
            *size = size_value;
            return true;
        }
        h = ti_OpenVar(it->name, "r", it->type);
        if (!h) return false;
        size_value = (uint32_t)ti_GetSize(h);
        ti_Close(h);
        size_type = it->type;
        memcpy(size_name, it->name, sizeof(size_name));
        size_cached = true;
        *size = size_value;
        return true;
    }
    return selected_drive_file_size(size);
}

/* Writes a byte count in the largest unit that fits: bytes, KB or MB, with
 * one decimal place (for example "1.5 KB"). Integer math only. */
void format_size(uint32_t n, char *out) {
    if (n < 1024UL) {
        fmt(out, "%lu bytes", (unsigned long)n);
    } else if (n < 1024UL * 1024UL) {
        fmt(out, "%lu.%lu KB",
                (unsigned long)(n / 1024UL),
                (unsigned long)(((n % 1024UL) * 10UL) / 1024UL));
    } else {
        fmt(out, "%lu.%lu MB",
                (unsigned long)(n / (1024UL * 1024UL)),
                (unsigned long)(((n % (1024UL * 1024UL)) * 10UL) / (1024UL * 1024UL)));
    }
}

/* Sets fg, bg and transparent color together so text never inherits a
 * stale color. The transparent color equals bg, so text is drawn over the
 * background that was filled first. */
void text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    gfx_SetTextFGColor(fg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextTransparentColor(bg);
    gfx_PrintStringXY(s, x, y);
}

/* Shortens a path for the path line. Up to 4 levels show in full. Deeper paths
 * show the first two levels, "...", then the last two: /a/b/.../d/e */
void shorten_path(char *out, size_t outsz, const char *full) {
    char copy[PATH_MAX + 16];
    char *part[64];
    int n = 0;
    char *p;

    strncpy(copy, full, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    p = copy;
    while (*p) {
        while (*p == '/') *p++ = 0;        /* turn separators into string ends */
        if (!*p) break;
        if (n < 64) part[n++] = p;
        while (*p && *p != '/') p++;
    }
    if (n == 0) {
        strncpy(out, "/", outsz - 1);
        out[outsz - 1] = 0;
    } else if (n <= 4) {
        int i;
        out[0] = 0;
        for (i = 0; i < n; i++) {
            strncat(out, "/", outsz - strlen(out) - 1);
            strncat(out, part[i], outsz - strlen(out) - 1);
        }
    } else {
        out[0] = 0;
        strncat(out, "/", outsz - strlen(out) - 1);
        strncat(out, part[0], outsz - strlen(out) - 1);
        strncat(out, "/", outsz - strlen(out) - 1);
        strncat(out, part[1], outsz - strlen(out) - 1);
        strncat(out, "/.../", outsz - strlen(out) - 1);
        strncat(out, part[n - 2], outsz - strlen(out) - 1);
        strncat(out, "/", outsz - strlen(out) - 1);
        strncat(out, part[n - 1], outsz - strlen(out) - 1);
    }
}

/* Like text_at, but cut short with "..." so it fits in maxw pixels. Long names
 * and paths would otherwise wrap onto the next line. */
void text_clip(int x, int y, const char *s, uint8_t fg, uint8_t bg, int maxw) {
    char buf[128];
    size_t n;
    const int dots = (int)gfx_GetStringWidth("...");

    if ((int)gfx_GetStringWidth(s) <= maxw) {
        text_at(x, y, s, fg, bg);
        return;
    }
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    n = strlen(buf);
    while (n > 0 && (int)gfx_GetStringWidth(buf) + dots > maxw) {
        buf[--n] = 0;
    }
    strcat(buf, "...");
    text_at(x, y, buf, fg, bg);
}

/* Key labels across the bottom bar, each centered under its top key (y=, Window,
 * Zoom, Trace, Graph) with the screen split into five equal columns. */
/* ---------- Popup menus ---------- */

/* Window opens the Files menu on the USB list. Zoom opens the Actions menu. */
const char *files_items[3]   = { "New folder", "Delete", "New file" };
const char *actions_items[4] = { "Rename", "Move here", "Copy here", "Edit" };
bool menu_open = false;
enum menu_kind menu_kind = MENU_NONE;
int menu_sel = 0;

char hide_label[8];          /* "Hide" or "Unhide", set from the selected USB entry */

int menu_count(void) {
    return menu_kind == MENU_FILES ? 3 : 6;
}

const char *menu_item(int i) {
    if (menu_kind == MENU_FILES) return files_items[i];
    if (i == 4) {
        bool h = item_count > 0 && items[selected].hidden;
        strcpy(hide_label, h ? "Unhide" : "Hide");
        return hide_label;
    }
    if (i == 5) return "Send to calc";
    return actions_items[i];
}

/* Draws the popup over the list. */
/* ---------- Filter popup (Graph) ----------
 * Rows are the filters of the list on screen. Enter opens a row's dropdown,
 * Up/Down picks an option, Enter applies it. Clear (or Mode/Graph) backs out
 * one level at a time. */
bool filter_open = false;
uint8_t filter_sel = 0;         /* row the cursor is on */
bool filter_drop = false;       /* that row's dropdown is open */
uint8_t filter_drop_sel = 0;   /* option highlighted in the dropdown */

void filters_changed(void) {
    reset_view_position();
    if (location == LOC_CALC) calc_refresh();
    else drive_refresh();
    needs_redraw = true;
}

void filter_key(uint8_t key) {
    filter_row_t rows[4];
    uint8_t n = filter_rows_build(rows);
    uint8_t cnt;
    bool type_locked;

    if (filter_sel >= n) filter_sel = 0;   /* the list changed (calculator has 3 rows, USB 2) */
    cnt = rows[filter_sel].count;
    /* Type is locked to Prgm while Hidden is on, so it can't be opened. */
    type_locked = location == LOC_CALC && rows[filter_sel].val == &type_filter && hidden_filter == 2;

    if (filter_drop) {
        if (key == sk_Up) {
            filter_drop_sel = (uint8_t)((filter_drop_sel + cnt - 1) % cnt);
        } else if (key == sk_Down) {
            filter_drop_sel = (uint8_t)((filter_drop_sel + 1) % cnt);
        } else if (key == sk_Enter) {
            *rows[filter_sel].val = filter_drop_sel;
            /* Hidden only lists programs, so choosing it sets Type to Prgm. */
            if (location == LOC_CALC && rows[filter_sel].val == &hidden_filter) {
                if (hidden_filter == 2) {
                    if (!type_forced) { type_before = type_filter; type_forced = true; }
                    type_filter = TYPE_PRGM;
                } else if (type_forced) {
                    type_filter = type_before;     /* All or Visible: give Type back */
                    type_forced = false;
                }
            }
            filter_drop = false;
            filters_changed();
            return;
        } else if (key == sk_Clear || key == sk_Mode || key == sk_Graph) {
            filter_drop = false;
        }
    } else {
        if (key == sk_Up) {
            filter_sel = (uint8_t)((filter_sel + n - 1) % n);
        } else if (key == sk_Down) {
            filter_sel = (uint8_t)((filter_sel + 1) % n);
        } else if (key == sk_Enter) {
            if (!type_locked) {
                filter_drop = true;
                filter_drop_sel = *rows[filter_sel].val;
            }
        } else if (key == sk_Clear || key == sk_Mode || key == sk_Graph) {
            filter_open = false;
        }
    }
    needs_redraw = true;
}

void draw_filter(void) {
    filter_row_t rows[4];
    int n = filter_rows_build(rows);
    int x = 40, y = 40, w = 240, h = 20 + n * 18 + 4;
    int i, j;

    gfx_SetColor(T_OUT);
    gfx_FillRectangle(x, y, w, h);
    gfx_SetColor(T_BG);
    gfx_FillRectangle(x + 1, y + 1, w - 2, h - 2);
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(x + 1, y + 1, w - 2, 16);
    text_at(x + 6, y + 4, "Filters", T_TOPTXT, T_TOP);
    {
        /* Tells you what the keys do right now, in the title bar so the dropdown can't cover it. */
        const char *hint = filter_drop ? "Up/Down; Enter: set" : "Enter: change";
        text_at(x + w - 8 - (int)gfx_GetStringWidth(hint), y + 4, hint, T_TOPTXT, T_TOP);
    }
    for (i = 0; i < n; i++) {
        int ry = y + 20 + i * 18;
        int sel = i == filter_sel;
        char val[24];

        fmt(val, "%s  v", rows[i].opts[*rows[i].val]);
        if (sel) {
            gfx_SetColor(T_SEL);
            gfx_FillRectangle(x + 3, ry, w - 6, 16);
        }
        text_at(x + 8, ry + 3, rows[i].label, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        text_at(x + 90, ry + 3, val, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
    }
    if (filter_drop) {
        int dx = x + 90, dy = y + 20 + filter_sel * 18 + 16;
        int cnt = rows[filter_sel].count;
        int dw = 110, dh = cnt * 16 + 4;

        gfx_SetColor(T_OUT);
        gfx_FillRectangle(dx, dy, dw, dh);
        gfx_SetColor(T_BG);
        gfx_FillRectangle(dx + 1, dy + 1, dw - 2, dh - 2);
        for (j = 0; j < cnt; j++) {
            int jy = dy + 2 + j * 16;
            if (j == filter_drop_sel) {
                gfx_SetColor(T_SEL);
                gfx_FillRectangle(dx + 2, jy, dw - 4, 16);
                text_at(dx + 6, jy + 3, rows[filter_sel].opts[j], T_SELTXT, T_SEL);
            } else {
                text_at(dx + 6, jy + 3, rows[filter_sel].opts[j], T_TXT, T_BG);
            }
        }
    }
}

void draw_menu(void) {
    int i, n = menu_count();
    int x = 80, y = 60, w = 160, h = 20 + n * 18 + 4;

    gfx_SetColor(T_OUT);
    gfx_FillRectangle(x, y, w, h);
    gfx_SetColor(T_BG);
    gfx_FillRectangle(x + 1, y + 1, w - 2, h - 2);
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(x + 1, y + 1, w - 2, 16);
    text_at(x + 6, y + 4, menu_kind == MENU_FILES ? "Files" : "Actions", T_TOPTXT, T_TOP);
    for (i = 0; i < n; i++) {
        int ry = y + 20 + i * 18;
        if (i == menu_sel) {
            gfx_SetColor(T_SEL);
            gfx_FillRectangle(x + 3, ry, w - 6, 16);
            text_at(x + 8, ry + 3, menu_item(i), T_SELTXT, T_SEL);
        } else {
            text_at(x + 8, ry + 3, menu_item(i), T_TXT, T_BG);
        }
    }
}

void draw_key_legend(int y) {
    static const char *calc_labels[5] = { "Search", "Info", "", "Export", "Filter" };
    static const char *usb_labels[5]  = { "Search", "Files", "Actions", "Mark", "Filter" };
    const char *labels[5];
    int i, cx, w;

    /* The info panel has its own keys, so the legend shows those instead of the list's. */
    if (var_info_is_open() && !var_info_is_usb()) {
        var_info_labels(labels);                   /* Y=, Window, Zoom, Trace, Graph */
        for (i = 0; i < 5; i++) {
            if (!labels[i][0]) continue;
            cx = i * (SCREEN_W / 5) + (SCREEN_W / 5) / 2;
            w = (int)gfx_GetStringWidth(labels[i]);
            text_at(cx - w / 2, y, labels[i], T_BOTTXT, T_BOT);
        }
        return;
    }

    /* Only the keys that do something on the current list are labeled. */
    for (i = 0; i < 5; i++) {
        if (location == LOC_DRIVE) labels[i] = usb_labels[i];
        else if (location == LOC_CALC) labels[i] = calc_labels[i];
        else labels[i] = i == 0 ? "Search" : "";
    }
    /* While an export waits for its folder, Trace is the Export key. */
    if (export_picking()) {
        labels[3] = "Export";
        labels[4] = "Cancel";                        /* Graph cancels the export */
    }
    if ((location == LOC_DRIVE || location == LOC_CALC) && (pending_delete || xfer_pending())) {   /* waiting for an answer */
        for (i = 0; i < 5; i++) labels[i] = "";
        labels[0] = "Yes";                           /* Y= */
        labels[4] = "No";                            /* Graph */
    }

    for (i = 0; i < 5; i++) {
        if (!labels[i][0]) continue;
        cx = i * (SCREEN_W / 5) + (SCREEN_W / 5) / 2;
        w = (int)gfx_GetStringWidth(labels[i]);
        text_at(cx - w / 2, y, labels[i], T_BOTTXT, T_BOT);
    }
}

/* Draws a sprite centered inside the left icon box at row y. */
void draw_in_box(gfx_rletsprite_t *s, int w, int h, int y) {
    gfx_RLETSprite(s, ICON_X + (ICON_W - w) / 2, y + (ROW_H - h) / 2);
}

/* Base file icon, then the type icon on top. Archive and lock are NOT
 * drawn here; they go on the right. */
void draw_file_with_overlay(uint8_t overlay, int y) {
    draw_in_box(file, file_width, file_height, y);
    switch (overlay) {
        case OV_APPVAR: draw_in_box(appvar, appvar_width, appvar_height, y); break;
        case OV_PRGM:   draw_in_box(prgm, prgm_width, prgm_height, y);       break;
        case OV_APP:    draw_in_box(app, app_width, app_height, y);          break;
        case OV_VAR:    draw_in_box(var, var_width, var_height, y);          break;
        case OV_TEXT:   draw_in_box(text, text_width, text_height, y);       break;
        case OV_HTML:   draw_in_box(html, html_width, html_height, y);       break;
        case OV_IMAGE:  draw_in_box(image, image_width, image_height, y);    break;
        case OV_GIF:    draw_in_box(gif, gif_width, gif_height, y);          break;
        default:        break;
    }
}

bool is_prgm_type(uint8_t type) {
    return type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM;
}

/* A program's icon, read from the file: 16x16, one palette byte per pixel,
 * after a 3-byte jump, a 0x01 marker and the 0x10 0x10 size. Stored in
 * gfx_sprite_t layout (width, height, pixels) so it can be drawn directly. */
uint8_t icon_sprite[2 + ICON_PIXELS];


/* Finds a compiled program's icon the way Cesium and CEAShell do: after EF 7B, the jump C3
 * sits at offset 2 (or 3, if not at 2). The icon marker 01 is 4 bytes after the jump, then
 * the 10 10 size, and the 256 pixel bytes start 3 bytes after the marker. Returns the offset
 * of the pixels, or -1 if the program has no icon there. */
int find_icon_header(const uint8_t *buf, int n) {
    int jp = -1;

    if (n > 2 && buf[2] == 0xC3) {
        jp = 2;
    } else if (n > 3 && buf[3] == 0xC3) {
        jp = 3;
    }
    if (jp < 0 || jp + 4 >= n || buf[jp + 4] != 0x01) return -1;
    return jp + 7;
}

uint8_t basic_head[BASIC_SCAN];
uint8_t basic_pix[BASIC_PIXELS];

/* TI-BASIC icon colors, from Cesium's lut_color_basic: the OS palette index for each hex
 * digit. Digit 0 is the transparent color (see basic_color). */
const uint8_t basic_palette[16] = {
    0xFF, 0x18, 0xE0, 0x00, 0xF8, 0x24, 0xE3, 0x61,
    0x09, 0x13, 0xE6, 0xFF, 0xB5, 0x6B, 0x6A, 0x4A
};

uint16_t ti_pal[16];         /* the TI colors, copied from the startup palette */

bool icon_transparent = false;   /* true when the current icon is TI-BASIC (0xDF sees through) */

static bool is_hex_digit(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_val(uint8_t c) {
    return c <= '9' ? (uint8_t)(c - '0') : (uint8_t)(c - 'A' + 10);
}

/* Pixel value for one TI-BASIC color digit: the TI color slot, or the transparent index for 0. */
uint8_t basic_color(uint8_t ch) {
    uint8_t d = hex_val(ch);

    if (d == 0) return PRGM_TRANSPARENT;
    return basic_palette[d];
}

/* Byte i of a mono icon: the two hex digits at 2i and 2i+1, high nibble first. */
static uint8_t mono_byte(const uint8_t *s, int i) {
    return (uint8_t)((hex_val(s[2 * i]) << 4) | hex_val(s[2 * i + 1]));
}

/* Reads a TI-BASIC icon the way Cesium does. The line after the description holds ":DCS",
 * Enter, a quote, then hex digits: 256 digits for a 16x16 color icon, 64 for 16x16 mono
 * (a ":DCS6" header is also 64), 16 for 8x8 mono. In color, each digit is one pixel (0 is
 * transparent). In mono, each bit is one pixel: 1 is drawn in TI color 13, 0 is transparent.
 * The 8x8 icon is drawn 2x2 per pixel to fill the 16x16 sprite. Searches the first half of
 * the program, in pieces. */
bool read_basic_icon(const item_t *it) {
    uint8_t h;
    size_t size, limit, pos = 0;
    int got, i, run = 0, x, y, k;
    long hex_at = -1;
    bool ok = false;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    size = ti_GetSize(h);
    limit = size / 2;
    while (hex_at < 0 && pos < limit) {
        if (ti_Seek((int)pos, SEEK_SET, h) == EOF) break;
        got = (int)ti_Read(basic_head, 1, BASIC_SCAN, h);
        for (i = 0; i + 7 <= got && hex_at < 0; i++) {
            if (basic_head[i] != 0x3E || basic_head[i + 1] != 0x44 ||
                basic_head[i + 2] != 0x43 || basic_head[i + 3] != 0x53) continue;   /* ":DCS" */
            if (basic_head[i + 4] == 0x3F && basic_head[i + 5] == 0x2A) {          /* Enter, quote */
                hex_at = (long)pos + i + 6;
            } else if (basic_head[i + 4] == 0x36 && basic_head[i + 5] == 0x3F &&
                       basic_head[i + 6] == 0x2A) {                                /* "6", Enter, quote */
                hex_at = (long)pos + i + 7;
            }
        }
        if (got < 7) break;
        pos += (size_t)(got - 6);   /* overlap 6 bytes so a header across the edge is found */
    }

    if (hex_at >= 0 && ti_Seek((int)hex_at, SEEK_SET, h) != EOF) {
        got = (int)ti_Read(basic_pix, 1, BASIC_PIXELS, h);
        while (run < got && is_hex_digit(basic_pix[run])) run++;
        ok = run == 256 || run == 64 || run == 16;
    }
    ti_Close(h);
    if (!ok) return false;

    if (run == 256) {
        for (k = 0; k < 256; k++) icon_sprite[2 + k] = basic_color(basic_pix[k]);
    } else if (run == 64) {
        for (y = 0; y < 16; y++) {
            for (x = 0; x < 16; x++) {
                uint8_t b = mono_byte(basic_pix, y * 2 + x / 8);
                bool set = ((b >> (7 - x % 8)) & 1) != 0;
                icon_sprite[2 + y * 16 + x] = set ? basic_palette[13] : PRGM_TRANSPARENT;
            }
        }
    } else {
        for (y = 0; y < 8; y++) {
            uint8_t b = mono_byte(basic_pix, y);
            for (x = 0; x < 8; x++) {
                bool set = ((b >> (7 - x)) & 1) != 0;
                uint8_t v = set ? basic_palette[13] : PRGM_TRANSPARENT;
                icon_sprite[2 + (2 * y) * 16 + 2 * x] = v;
                icon_sprite[2 + (2 * y) * 16 + 2 * x + 1] = v;
                icon_sprite[2 + (2 * y + 1) * 16 + 2 * x] = v;
                icon_sprite[2 + (2 * y + 1) * 16 + 2 * x + 1] = v;
            }
        }
    }
    icon_sprite[0] = 16;
    icon_sprite[1] = 16;
    icon_transparent = true;
    return true;
}

/* Compiled programs start with EF 7B (Cesium's test); TI-BASIC programs don't. A compiled
 * program's icon is the jump header; a TI-BASIC program's icon is the *?>* comment. Only
 * the search that matches the program's kind is run. */
bool read_prgm_icon(const item_t *it) {
    uint8_t head[ICON_SCAN];
    uint8_t h;
    int n, at;
    bool ok = false;
    bool compiled;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    n = (int)ti_Read(head, 1, ICON_SCAN, h);
    compiled = n >= 2 && head[0] == 0xEF && head[1] == 0x7B;
    if (!compiled) {
        ti_Close(h);
        return read_basic_icon(it);
    }
    at = find_icon_header(head, n);
    if (at >= 0 &&
        ti_Seek(at, SEEK_SET, h) != EOF &&
        ti_Read(icon_sprite + 2, 1, ICON_PIXELS, h) == ICON_PIXELS) {
        ok = true;
    }
    ti_Close(h);
    if (ok) {
        icon_sprite[0] = 16;
        icon_sprite[1] = 16;
        icon_transparent = false;
    }
    return ok;
}

/* Program icons are cached per name and type, so scrolling doesn't reopen and rescan
 * every visible program on each redraw. A miss is cached too (no icon). The cache is
 * cleared whenever the calculator list is rebuilt, so edited programs get fresh icons. */
#define ICON_CACHE_N 16
typedef struct {
    bool used;
    bool has_icon;
    bool transparent;
    uint8_t type;
    char name[8];
    uint8_t sprite[2 + ICON_PIXELS];
} icon_cache_t;
static icon_cache_t *icon_cache;           /* heap, allocated by draw_alloc (about 4.3 KB) */
static int icon_cache_next = 0;
#define DESC_MAX 52                      /* CEAShell's limit for descriptions */
static bool desc_cached = false;        /* the selected description is loaded (selected_description) */

/* Allocates the icon cache on the heap, at startup (see mem_init). */
bool draw_alloc(void) {
    icon_cache = calloc(ICON_CACHE_N, sizeof(icon_cache_t));
    return icon_cache != NULL;
}

void icon_cache_clear(void) {
    if (icon_cache) memset(icon_cache, 0, ICON_CACHE_N * sizeof(icon_cache_t));
    icon_cache_next = 0;
    size_cached = false;
    desc_cached = false;
}

/* Loads the program's icon into icon_sprite and icon_transparent, from the cache if
 * possible. Returns false if the program has no icon. */
static bool cached_prgm_icon(const item_t *it) {
    int i;
    int slot;

    if (!icon_cache) return read_prgm_icon(it);   /* no cache: read it directly */
    for (i = 0; i < ICON_CACHE_N; i++) {
        if (icon_cache[i].used && icon_cache[i].type == it->type &&
            memcmp(icon_cache[i].name, it->name, sizeof(icon_cache[i].name)) == 0) {
            if (!icon_cache[i].has_icon) return false;
            memcpy(icon_sprite, icon_cache[i].sprite, sizeof(icon_sprite));
            icon_transparent = icon_cache[i].transparent;
            return true;
        }
    }
    slot = icon_cache_next;
    icon_cache_next = (icon_cache_next + 1) % ICON_CACHE_N;
    icon_cache[slot].used = true;
    icon_cache[slot].type = it->type;
    memcpy(icon_cache[slot].name, it->name, sizeof(icon_cache[slot].name));
    icon_cache[slot].has_icon = read_prgm_icon(it);
    icon_cache[slot].transparent = icon_transparent;
    if (icon_cache[slot].has_icon) {
        memcpy(icon_cache[slot].sprite, icon_sprite, sizeof(icon_sprite));
    }
    return icon_cache[slot].has_icon;
}

void draw_item_icon(const item_t *it, int y) {
    switch (it->kind) {
        case KIND_USB:
            if (msd_ready) {
                draw_in_box(usb, usb_width, usb_height, y);
            } else {
                draw_in_box(usbdisconnect, usbdisconnect_width,
                            usbdisconnect_height, y);
            }
            break;
        case KIND_PART:
            draw_in_box(dir, dir_width, dir_height, y);
            break;
        case KIND_DIR:
            if (it->contents == 1) {
                draw_in_box(dirfilled, dirfilled_width, dirfilled_height, y);
            } else if (it->contents == 2) {
                draw_in_box(dirdirs, dirdirs_width, dirdirs_height, y);
            } else {
                draw_in_box(dir, dir_width, dir_height, y);
            }
            break;
        case KIND_FILE:
            draw_file_with_overlay(it->overlay, y);
            break;
        case KIND_VAR:
            draw_file_with_overlay(it->overlay, y);
            break;
        case KIND_APP:
            draw_file_with_overlay(it->overlay, y);
            break;
        default:
            break;
    }
}

/* True if this row is the file marked with Trace (USB list only). */
bool row_is_marked(const item_t *it) {
    char p[PATH_MAX + 16];

    char n[16];

    if (!mark_set || location != LOC_DRIVE) return false;
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) return false;
    strncpy(n, it->name, sizeof(n) - 1);            /* folder names carry a trailing "/" */
    n[sizeof(n) - 1] = 0;
    strip_trailing_slash(n);
    if (strcmp(drive_path, "/") == 0) {
        fmt(p, "/%s", n);
    } else {
        fmt(p, "%s/%s", drive_path, n);
    }
    return strcmp(p, mark_path) == 0;
}

/* An app's own icon, read from its header in flash the way CEAShell does: the header's
 * icon offset is the 3 bytes at +0x112, counted from +0x100. The icon is either a 01 marker
 * (C apps: 00 C3 ... 01 after a jump) followed by a 16x16 size and 256 pixel bytes. Loads
 * it into icon_sprite; returns false if the app has no icon. */
static bool app_icon_load(const item_t *it) {
    const uint8_t *base;
    const uint8_t *ic;
    const uint8_t *hdr;
    uint32_t off;

    if (it->app_addr == 0) return false;
    base = (const uint8_t *)(uintptr_t)it->app_addr;
    off = (uint32_t)base[0x112] | ((uint32_t)base[0x113] << 8) | ((uint32_t)base[0x114] << 16);
    ic = base + 0x100 + off;
    if (ic[0] == 0x01) {
        hdr = ic;
    } else if (ic[0] == 0 && ic[1] == 0xC3 && ic[5] == 0x01) {
        hdr = ic + 5;
    } else {
        return false;
    }
    memcpy(icon_sprite + 2, hdr + 3, ICON_PIXELS);
    icon_sprite[0] = 16;
    icon_sprite[1] = 16;
    icon_transparent = false;
    return true;
}

void draw_row(int y, const item_t *it, const char *right, bool sel) {
    uint8_t bg = sel ? T_SEL : T_BG;
    uint8_t fg = sel ? T_SELTXT : T_TXT;
    int ty = y + (ROW_H - 8) / 2;

    gfx_SetColor(bg);
    gfx_FillRectangle(0, y, SCREEN_W, ROW_H);
    draw_item_icon(it, y);
    {
        /* The name stops before the first right-side marker (program icon, archive, or label). */
        int edge = LABEL_X;
        if (it->kind == KIND_VAR) edge = is_prgm_type(it->type) ? PRGM_ICON_X : ARCHIVE_X;
        if (it->kind == KIND_APP) edge = PRGM_ICON_X;
        /* A hidden program's name uses the Hidden color (gray by default). */
        uint8_t name_fg = row_is_marked(it) ? T_MARK : fg;
        if ((it->kind == KIND_VAR || it->kind == KIND_FILE || it->kind == KIND_DIR) &&
            it->hidden && !row_is_marked(it)) name_fg = T_HIDDEN;
        text_clip(TEXT_X, ty, it->kind == KIND_VAR ? it->shown : it->name, name_fg, bg, edge - TEXT_X - 4);
    }

    /* Right-side markers, each on its own spot. */
    if ((it->kind == KIND_VAR && it->archived) || it->kind == KIND_APP) {
        gfx_RLETSprite(archive, ARCHIVE_X, y + (ROW_H - archive_height) / 2);
    }
    if ((it->kind == KIND_VAR && it->type == OS_TYPE_PROT_PRGM) || it->kind == KIND_APP) {
        gfx_RLETSprite(lock, LOCK_X, y + (ROW_H - lock_height) / 2);
    }
    if (it->kind == KIND_VAR && is_prgm_type(it->type) && cached_prgm_icon(it)) {
        if (icon_transparent) {
            gfx_SetTransparentColor(PRGM_TRANSPARENT);
            gfx_TransparentSprite((gfx_sprite_t *)icon_sprite, PRGM_ICON_X, y + (ROW_H - 16) / 2);
        } else {
            gfx_Sprite((gfx_sprite_t *)icon_sprite, PRGM_ICON_X, y + (ROW_H - 16) / 2);
        }
    }
    if (it->kind == KIND_APP && app_icon_load(it)) {
        gfx_Sprite((gfx_sprite_t *)icon_sprite, PRGM_ICON_X, y + (ROW_H - 16) / 2);
    }
    if (right) text_at(LABEL_X, ty, right, fg, bg);
}

const char *item_right_label(const item_t *it) {
    switch (it->kind) {
        case KIND_USB:    return msd_ready ? usb_label : "no drive";
        case KIND_PART:   return "PART";
        case KIND_DIR:    return "DIR";
        case KIND_FILE:
        case KIND_VAR:    return overlay_label(it->overlay);
        case KIND_APP:    return "App";
        default:          return NULL;
    }
}

/* ---------- Battery ---------- */

/* boot_GetBatteryStatus() gives a level 0-4. The juice sprite for that level
 * is drawn first, then the frame on top: batt, or battchg while charging.
 * The percentage is the level times 25, so it is approximate. */
/* Battery, 2nd, alpha and alpha lock icons at the top right. Also drawn over
 * the text viewer (see viewer_set_overlay). */
void draw_indicators(void);

void draw_battery(void) {
    uint8_t level = batt_level;
    bool charging = batt_charging && !msd_ready;  /* data open = not charging */
    gfx_rletsprite_t *juice = NULL;
    int x = SCREEN_W - 18;     /* 12px wide icon at the right of the title bar */
    int y = 1;                 /* 18px tall, fits in the 20px title bar */

    if (level > 4) level = 4;
    switch (level) {
        case 4: juice = batthi;     break;
        case 3: juice = battmedhi;  break;
        case 2: juice = battmedlow; break;
        case 1: juice = battlow;    break;
        default: break;
    }
    if (juice) gfx_RLETSprite(juice, x, y);
    gfx_RLETSprite(charging ? battchg : batt, x, y);

}

void draw_indicators(void) {
    draw_battery();
    keys_draw(SCREEN_W - 18, 1);   /* left of the battery, which is at SCREEN_W - 18 */
}

/* Drawn over the text viewer: the icons, and the color menu when it is open. */
void draw_viewer_overlay(void) {
    draw_indicators();
    if (settings_open) draw_settings();
}

/* ---------- Descriptions ----------
 * Read the way CEAShell does: a compiled program's description follows its icon (or sits
 * where the icon would be), a TI-BASIC program's is its first line in quotes, and an
 * app's is its copyright string. Shown in the title bar for the selected item. */

/* Copies up to max bytes of printable text from src into out (NUL terminated). Stops at
 * the first byte outside 0x20-0x7E. Returns true if anything was copied. */
static bool copy_desc(const uint8_t *src, int max, char *out) {
    int i, k = 0;
    for (i = 0; i < max && k < DESC_MAX; i++) {
        uint8_t c = src[i];
        if (c < 0x20 || c >= 0x7F) break;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return k > 0;
}

/* Compiled (ASM or C) program: after EF 7B, the jump at offset 2 (or 3) and the marker 4
 * bytes past it. Marker 1 means an icon, so the text is 259 bytes after the marker's start
 * (CEAShell's getDescASM); marker 2 means text straight after it. */
static bool compiled_desc_load(const item_t *it, char *out) {
    uint8_t h;
    uint8_t head[ICON_SCAN];
    uint8_t buf[DESC_MAX];
    int n, jp = -1, got;
    long at = -1;
    bool ok = false;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    n = (int)ti_Read(head, 1, ICON_SCAN, h);
    if (n >= 4 && head[0] == 0xEF && head[1] == 0x7B) {
        if (head[2] == 0xC3) jp = 2;
        else if (n > 3 && head[3] == 0xC3) jp = 3;
        if (jp >= 0 && jp + 4 < n) {
            if (head[jp + 4] == 1) at = jp + 263;
            else if (head[jp + 4] == 2) at = jp + 5;
        }
    }
    if (at >= 0 && ti_Seek((int)at, SEEK_SET, h) != EOF) {
        got = (int)ti_Read(buf, 1, DESC_MAX, h);
        ok = copy_desc(buf, got, out);
    }
    ti_Close(h);
    return ok;
}

/* TI-BASIC program: ':' then a quote (or "::" then a quote), then the text up to Enter or
 * the closing quote. */
static bool basic_desc_load(const item_t *it, char *out) {
    uint8_t h;
    uint8_t head[64];
    int n, i, k = 0;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    n = (int)ti_Read(head, 1, 64, h);
    ti_Close(h);
    if (n < 3 || head[0] != 0x3E) return false;
    if (head[1] == 0x2A) i = 2;
    else if (head[1] == 0x3E && head[2] == 0x2A) i = 3;
    else return false;
    for (; i < n && k < DESC_MAX; i++) {
        uint8_t c = head[i];
        if (c == 0x3F || c == 0x2A || c < 0x20 || c >= 0x7F) break;   /* Enter, quote, or not text */
        out[k++] = (char)c;
    }
    out[k] = 0;
    return k > 0;
}

/* App: the copyright string. Its offset is the 3 bytes at +0x124, counted from +0x100, as
 * CEAShell's getAppCopyrightInfo reads it. Zero means there is none. */
static bool app_desc_load(const item_t *it, char *out) {
    const uint8_t *base;
    uint32_t off;

    if (it->app_addr == 0) return false;
    base = (const uint8_t *)(uintptr_t)it->app_addr;
    off = (uint32_t)base[0x124] | ((uint32_t)base[0x125] << 8) | ((uint32_t)base[0x126] << 16);
    if (off == 0) return false;
    return copy_desc(base + 0x100 + off, DESC_MAX, out);
}

static bool load_description(const item_t *it, char *out) {
    if (it->kind == KIND_APP) return app_desc_load(it, out);
    if (is_prgm_type(it->type)) {
        return compiled_desc_load(it, out) || basic_desc_load(it, out);
    }
    return false;
}

/* The selected item's description, if it has one. Cached, so redraws don't read the file again. */
static item_t desc_item;
static bool desc_found = false;
static char desc_text[DESC_MAX + 1];

bool selected_description(char *out) {
    const item_t *it;

    if (item_count == 0) return false;
    it = &items[selected];
    if (it->kind != KIND_APP && !(it->kind == KIND_VAR && is_prgm_type(it->type))) return false;
    if (!desc_cached || desc_item.kind != it->kind || desc_item.type != it->type ||
        desc_item.app_addr != it->app_addr ||
        memcmp(desc_item.name, it->name, sizeof(it->name)) != 0) {
        desc_item = *it;
        desc_found = load_description(it, desc_text);
        desc_cached = true;
    }
    if (!desc_found) return false;
    strcpy(out, desc_text);
    return true;
}

/* Draws the whole screen into the current draw target, without showing it. The title
 * intro uses this to render the screen ahead of time. */
void draw_body(void) {
    char line[64];
    char path[PATH_MAX + 8];
    int i;

    /* Background */
    gfx_FillScreen(T_BG);

    /* Title bar: top-bar text on top-bar color */
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    /* Folder icon at (2,2), then the name. The name is "FileR", or the selected item's
     * description. The name starts at x 22 (after the icon) and stops short of the
     * indicators at the right of the title bar. */
    gfx_RLETSprite(dirfilled, 2, 2);
    {
        char desc[DESC_MAX + 1];
        const char *label = "FileR";
        if (selected_description(desc)) label = desc;
        text_clip(22, 6, label, T_TOPTXT, T_TOP, SCREEN_W - 82 - 22);
    }
    draw_indicators();

    /* Path line and filters: black text on white */
    if (rename_mode) {
        fmt(line, "Rename: %s_   Enter save, Clear cancel", rename_buf);
    } else if (search_mode || search[0] != 0) {
        fmt(line, "Find: %s%s   (%d shown)", search,
                search_mode ? "_" : "", item_count - 1);
    } else if (location == LOC_CALC) {
        fmt(line, "Path: /%s",
                (store_filter || type_filter || hidden_filter) ? "   filtered" : "");
    } else if (location == LOC_PART) {
        fmt(line, "Pick a partition   (%d found)", part_count);
    } else {
        char shown[64];
        fmt(path, "/usb%s",
                strcmp(drive_path, "/") == 0 ? "/" : drive_path);
        shorten_path(shown, sizeof(shown), path);
        fmt(line, "Path: %s   (%d items)%s", shown, item_count - 1,
                (usb_kind || usb_ext || usb_hidden) ? "   filtered" : "");
    }
    {
        uint32_t fsize;
        if (!rename_mode && selected_size(&fsize)) {
            char sz[24];
            format_size(fsize, sz);
            if (strlen(line) + strlen(sz) + 6 < sizeof(line)) {
                strcat(line, "   ");
                strcat(line, sz);
            }
        }
    }
    text_clip(6, FILTER_Y + 2, line, T_TXT, T_BG, SCREEN_W - 12);

    /* List */
    if (item_count == 0) {
        text_at(6, LIST_Y + 4, "(empty)", T_TXT, T_BG);
    }
    for (i = 0; i < VISIBLE_ROWS; i++) {
        int idx = top + i;
        int y = LIST_Y + i * ROW_H;
        const item_t *it;
        if (idx >= item_count) break;
        it = &items[idx];
        draw_row(y, it, item_right_label(it), idx == selected);
    }

    if (var_info_is_open()) var_info_draw();

    /* Status bar: bottom-bar text on bottom-bar color */
    gfx_SetColor(T_BOT);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    if (status_timed) {
        text_clip(6, SCREEN_H - STATUS_H + 3, status, T_BOTTXT, T_BOT, SCREEN_W - 12);
    } else {
        draw_key_legend(SCREEN_H - STATUS_H + 3);
        if (menu_open) draw_menu();
    if (filter_open) draw_filter();
    }
    if (settings_open) draw_settings();
}

void draw(void) {
    draw_body();
    gfx_SwapDraw();
}

