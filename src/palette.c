#include "filer.h"

/* ---------- Theme ----------
 * Each themed UI role has its own palette slot, so changing it never touches
 * the icon colors. Each role picks one of the presets below in the settings
 * menu (Mode). The choice is saved in the AppVar FILERCFG. */

/* Starting colors, 8-bit RGB. At startup each one is matched to the nearest
 * palette entry (see theme_find_defaults). The Hidden role has no RGB: it starts
 * at THEME_HIDDEN_DEFAULT. */
const uint8_t THEME_DEFAULT_RGB[ROLE_HIDDEN][3] = {
    { 255, 255, 255 },   /* background */
    { 44, 100, 212 },    /* top bar */
    { 130, 182, 245 },   /* selector */
    { 0, 0, 0 },         /* bottom bar */
    { 0, 0, 0 },         /* text */
    { 255, 255, 255 },   /* bottom bar text */
    { 255, 255, 255 },   /* top bar text */
    { 0, 0, 0 },         /* popup outline */
    { 112, 64, 189 },    /* marked file */
    { 255, 255, 255 },   /* selected text */
};
uint16_t *palette256;   /* the calculator's palette at startup, RGB1555; heap, see palette_alloc */

/* Allocates the saved palette on the heap, at startup (see mem_init). */
bool palette_alloc(void) {
    palette256 = calloc(THEME_PRESETS, sizeof(uint16_t));
    return palette256 != NULL;
}
const char *const THEME_ROLE_NAME[THEME_ROLES] = {
    "Background", "Top bar", "Selector", "Bottom bar", "Text", "Bottom text", "Top text", "Popup outline", "Marked file", "Selected text", "Hidden",
};
const uint8_t THEME_ROLE_SLOT[THEME_ROLES] = { T_BG, T_TOP, T_SEL, T_BOT, T_TXT, T_BOTTXT, T_TOPTXT, T_OUT, T_MARK, T_SELTXT, T_HIDDEN };
uint8_t theme_default[THEME_ROLES];   /* palette index per role, found at startup */
uint8_t theme_idx[THEME_ROLES];     /* live preset per role (may hold unsaved changes) */
uint8_t theme_saved[THEME_ROLES];   /* preset per role as last saved */
bool settings_open = false;
uint8_t settings_sel = 0;           /* role the settings cursor is on */
uint8_t settings_num = 0;           /* value being typed with the digit keys */
bool settings_typing = false;       /* digits typed since the last move or change */
uint8_t settings_top = 0;           /* first role shown in the list (it scrolls) */
bool settings_fast = false;         /* the last change only moved the color menu: redraw just it */
void draw_settings(void);           /* defined with the settings code */
void make_path(char *out, const char *folder, const char *name);   /* defined with the path helpers */
void restore_palette(void);         /* defined with the palette code */

/* ---------- Palette ---------- */

/* Icon palette, the theme colors, and the marked-file color. The image viewer
 * replaces the whole palette, so this runs again when an image closes. */
void restore_palette(void) {
    uint16_t theme[THEME_ROLES];
    int r;

    gfx_SetPalette(mypalette, sizeof_mypalette, 0);  /* icon colors */
    for (r = 0; r < THEME_ROLES; r++) {
        theme[r] = palette256[theme_idx[r]];
        gfx_SetPalette(&theme[r], sizeof(uint16_t), THEME_ROLE_SLOT[r]);
    }
    gfx_SetPalette(ti_pal, sizeof(ti_pal), TI_PAL_BASE); /* TI-BASIC icon colors */
}

/* Copies the calculator's palette before the app changes any of it. Call once,
 * right after gfx_Begin. */
void theme_snapshot(void) {
    memcpy(palette256, gfx_palette, THEME_PRESETS * sizeof(uint16_t));
}

/* Sets theme_default to the palette entry closest to each starting color. */
void theme_find_defaults(void) {
    int r, i;

    for (r = 0; r < ROLE_HIDDEN; r++) {
        uint16_t want = gfx_RGBTo1555(THEME_DEFAULT_RGB[r][0], THEME_DEFAULT_RGB[r][1], THEME_DEFAULT_RGB[r][2]);
        long best_d = -1;
        theme_default[r] = 0;
        for (i = 0; i < THEME_PRESETS; i++) {
            long dr = ((palette256[i] >> 10) & 31) - ((want >> 10) & 31);
            long dg = ((palette256[i] >> 5) & 31) - ((want >> 5) & 31);
            long db = (palette256[i] & 31) - (want & 31);
            long d = dr * dr + dg * dg + db * db;
            if (best_d < 0 || d < best_d) {
                best_d = d;
                theme_default[r] = (uint8_t)i;
            }
        }
    }
    theme_default[ROLE_HIDDEN] = THEME_HIDDEN_DEFAULT;
}

/* ---------- Saved colors ----------
 * The AppVar FILERCFG holds a magic byte, then one palette index per role. A
 * missing or unreadable AppVar means the defaults. */

void theme_load(void) {
    uint8_t buf[1 + THEME_ROLES];
    uint8_t h;
    size_t n = 0;
    int i;

    memcpy(theme_idx, theme_default, THEME_ROLES);
    memcpy(theme_saved, theme_default, THEME_ROLES);
    h = ti_OpenVar(THEME_VAR, "r", OS_TYPE_APPVAR);
    if (!h) return;
    n = ti_Read(buf, 1, sizeof(buf), h);
    ti_Close(h);
    /* Saves from before the Hidden role are one byte shorter: the roles they have
     * still load, and Hidden keeps its default. Every byte is a palette index. */
    if (n >= 1 + (ROLE_HIDDEN) && buf[0] == THEME_MAGIC) {
        for (i = 0; i < THEME_ROLES && i + 1 < (int)n; i++) theme_idx[i] = theme_saved[i] = buf[1 + i];
    }
}

/* Writes the current colors to FILERCFG. Returns false if the AppVar can't be written. */
bool theme_save(void) {
    uint8_t buf[1 + THEME_ROLES];
    uint8_t h;
    size_t n;
    int i;

    buf[0] = THEME_MAGIC;
    for (i = 0; i < THEME_ROLES; i++) buf[1 + i] = theme_idx[i];
    h = ti_OpenVar(THEME_VAR, "w", OS_TYPE_APPVAR);
    if (!h) return false;
    n = ti_Write(buf, 1, sizeof(buf), h);
    if (n == sizeof(buf)) ti_SetArchiveStatus(true, h);   /* keep FILERCFG in archive */
    ti_Close(h);
    return n == sizeof(buf);
}

/* ---------- Settings (colors) ----------
 * Mode opens it. Up/Down picks a role, Left/Right changes its preset (shown at
 * once). Enter saves. Mode or Clear closes it and drops unsaved changes. */
void settings_begin(void) {
    settings_open = true;
    settings_sel = 0;
    settings_top = 0;
    needs_redraw = true;
}

void settings_close(bool save) {
    if (save) {
        if (theme_save()) {
            memcpy(theme_saved, theme_idx, THEME_ROLES);
            set_status("Colors saved");
        } else {
            set_status("Colors not saved (AppVar error)");
        }
    } else {
        memcpy(theme_idx, theme_saved, THEME_ROLES);
        restore_palette();
        set_status("Colors reverted");
    }
    settings_open = false;
    needs_redraw = true;
}

/* The digit a key types, or -1. */
int settings_digit(uint8_t key) {
    switch (key) {
        case sk_0: return 0;
        case sk_1: return 1;
        case sk_2: return 2;
        case sk_3: return 3;
        case sk_4: return 4;
        case sk_5: return 5;
        case sk_6: return 6;
        case sk_7: return 7;
        case sk_8: return 8;
        case sk_9: return 9;
        default:   return -1;
    }
}

/* Keeps the selected role inside the visible rows. */
void settings_follow(void) {
    if (settings_sel < settings_top) {
        settings_top = settings_sel;
    } else if (settings_sel >= settings_top + SETTINGS_VISIBLE) {
        settings_top = (uint8_t)(settings_sel - SETTINGS_VISIBLE + 1);
    }
}

void settings_key(uint8_t key) {
    int d = settings_digit(key);

    /* Digits type the value directly: 2, 5, 5 gives 255. A digit that would make
     * it over 255 starts a new value instead. */
    if (d >= 0) {
        int n = settings_typing ? settings_num * 10 + d : d;
        if (n > 255) n = d;
        settings_num = (uint8_t)n;
        settings_typing = true;
        theme_idx[settings_sel] = settings_num;
        restore_palette();
        needs_redraw = true;
        settings_fast = true;
        return;
    }
    settings_typing = false;

    if (key == sk_Up) {
        settings_sel = (uint8_t)((settings_sel + THEME_ROLES - 1) % THEME_ROLES);
        settings_follow();
    } else if (key == sk_Down) {
        settings_sel = (uint8_t)((settings_sel + 1) % THEME_ROLES);
        settings_follow();
    } else if (key == sk_Left) {
        theme_idx[settings_sel]--;      /* wraps 0 -> 255 */
        restore_palette();
    } else if (key == sk_Right) {
        theme_idx[settings_sel]++;      /* wraps 255 -> 0 */
        restore_palette();
    } else if (key == sk_Enter) {
        settings_close(true);
        return;
    } else if (key == sk_Mode || key == sk_Clear) {
        settings_close(false);
        return;
    }
    needs_redraw = true;
    settings_fast = true;
}

/* Draws the settings box over the screen. Each row's swatch is the role's
 * current palette slot, so it shows the color as it is now. */
void draw_settings(void) {
    int x = 16, y = 4, w = SCREEN_W - 32;
    int h = 20 + SETTINGS_VISIBLE * 18 + 30;
    int i, ry, last;

    gfx_SetColor(T_OUT);
    gfx_FillRectangle(x, y, w, h);
    gfx_SetColor(T_BG);
    gfx_FillRectangle(x + 1, y + 1, w - 2, h - 2);
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(x + 1, y + 1, w - 2, 16);
    text_at(x + 6, y + 4, "Colors", T_TOPTXT, T_TOP);

    /* Only the rows from settings_top down fit; the arrows show more above or below. */
    last = settings_top + SETTINGS_VISIBLE;
    if (last > THEME_ROLES) last = THEME_ROLES;
    for (i = settings_top; i < last; i++) {
        int sel = i == settings_sel;
        ry = y + 20 + (i - settings_top) * 18;
        if (sel) {
            gfx_SetColor(T_SEL);
            gfx_FillRectangle(x + 3, ry, w - 6, 16);
        }
        text_at(x + 8, ry + 4, THEME_ROLE_NAME[i], sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        {
            char num[5];
            fmt(num, "%3d", theme_idx[i]);             /* the value, so typed digits show */
            text_at(x + w - 100, ry + 4, num, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        }
        text_at(x + w - 62, ry + 4, "<", sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        gfx_SetColor(THEME_ROLE_SLOT[i]);
        gfx_FillRectangle(x + w - 50, ry + 3, 26, 10);
        gfx_SetColor(T_OUT);
        gfx_Rectangle(x + w - 50, ry + 3, 26, 10);
        text_at(x + w - 18, ry + 4, ">", sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
    }
    /* Small arrows at the right edge: a row above or below the list. */
    if (settings_top > 0) {
        int on = settings_top == settings_sel;   /* the arrow sits on that row's background */
        text_at(x + w - 10, y + 20 + 4, "^", on ? T_SELTXT : T_TXT, on ? T_SEL : T_BG);
    }
    if (last < THEME_ROLES) {
        int row = last - 1;
        int on = row == settings_sel;
        text_at(x + w - 10, y + 20 + (row - settings_top) * 18 + 4, "v", on ? T_SELTXT : T_TXT, on ? T_SEL : T_BG);
    }
    text_at(x + 6, y + h - 26, "Up/Down: Pick  Left/Right or 0-9: Value", T_TXT, T_BG);
    text_at(x + 6, y + h - 13, "Enter: Save   Mode/Clear: Cancel", T_TXT, T_BG);
}

