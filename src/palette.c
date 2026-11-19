#include "filer.h"

/* ---------- Theme ----------
 * Each themed UI role has its own palette slot, so changing it never touches
 * the icon colors. Each role picks one of the presets below on the Colors page
 * of the settings menu (Mode opens the menu). The choices are saved in the
 * AppVar FILERCFG. */

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
uint8_t settings_page = SET_MENU;   /* the page the settings box shows */
uint8_t settings_menu_sel = 0;      /* row the cursor is on in the settings menu */
uint8_t settings_sel = 0;           /* role the Colors page cursor is on (or the Intro page row) */
uint8_t settings_num = 0;           /* value being typed with the digit keys */
bool settings_typing = false;       /* digits typed since the last move or change */
uint8_t settings_top = 0;           /* first role shown in the Colors list (it scrolls) */
bool settings_fast = false;         /* the last change only moved the Colors cursor or value: redraw just the box */
bool intro_on = true;               /* the intro plays at startup; saved in FILERCFG */
bool intro_preview = false;         /* Intro page's Play intro now: the main loop plays it after this key */
uint8_t intro_fps = INTRO_FPS_DEF;      /* see filer.h for the units and limits */
uint8_t intro_pause = INTRO_PAUSE_DEF;
uint8_t intro_in = INTRO_IN_DEF;
uint8_t intro_out = INTRO_OUT_DEF;
static bool intro_dirty = false;    /* an intro setting changed since it was last saved */
void draw_settings(void);           /* defined with the settings code */
void make_path(char *out, const char *folder, const char *name);   /* defined with the path helpers */

/* ---------- Palette ---------- */

/* Apply theme colors only to the reserved UI slots; icon slots are untouched. */
void theme_apply(void) {
    uint16_t color[THEME_ROLES];
    int role;

    for (role = 0; role < THEME_ROLES; role++) {
        color[role] = palette256[theme_idx[role]];
        gfx_SetPalette(&color[role], sizeof(uint16_t), THEME_ROLE_SLOT[role]);
    }
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

/* ---------- Saved settings ----------
 * The AppVar FILERCFG holds a magic byte, one palette index per role, then the
 * intro setting (1 on, 0 off). A missing or unreadable AppVar means the defaults. */

/* Keeps v inside lo..hi. */
static uint8_t clamp_u8(int v, int lo, int hi) {
    if (v < lo) return (uint8_t)lo;
    if (v > hi) return (uint8_t)hi;
    return (uint8_t)v;
}

void theme_load(void) {
    uint8_t buf[THEME_SAVE_LEN];
    uint8_t h;
    size_t n = 0;
    int i;

    memcpy(theme_idx, theme_default, THEME_ROLES);
    memcpy(theme_saved, theme_default, THEME_ROLES);
    intro_on = true;
    intro_fps = INTRO_FPS_DEF;
    intro_pause = INTRO_PAUSE_DEF;
    intro_in = INTRO_IN_DEF;
    intro_out = INTRO_OUT_DEF;
    h = ti_OpenVar(THEME_VAR, "r", OS_TYPE_APPVAR);
    if (!h) return;
    n = ti_Read(buf, 1, sizeof(buf), h);
    ti_Close(h);
    /* Saves from before the Hidden role are one byte shorter: the roles they have
     * still load, and Hidden keeps its default. Every byte is a palette index. */
    if (n >= 1 + (ROLE_HIDDEN) && buf[0] == THEME_MAGIC) {
        for (i = 0; i < THEME_ROLES && i + 1 < (int)n; i++) theme_idx[i] = theme_saved[i] = buf[1 + i];
        /* Saves from before the intro settings stop early: the rest keep their defaults. */
        if (n >= INTRO_AT + 1) intro_on = buf[INTRO_AT] != 0;
        if (n >= THEME_SAVE_LEN) {
            intro_fps = clamp_u8(buf[INTRO_AT + 1], INTRO_FPS_MIN, INTRO_FPS_MAX);
            intro_pause = clamp_u8(buf[INTRO_AT + 2], 0, INTRO_PAUSE_MAX);
            intro_in = clamp_u8(buf[INTRO_AT + 3], INTRO_TIME_MIN, INTRO_TIME_MAX);
            intro_out = clamp_u8(buf[INTRO_AT + 4], INTRO_TIME_MIN, INTRO_TIME_MAX);
        }
    }
}

/* Writes the current colors and the intro settings to FILERCFG. Returns false if the
 * AppVar can't be written. */
bool theme_save(void) {
    uint8_t buf[THEME_SAVE_LEN];
    uint8_t h;
    size_t n;
    int i;

    buf[0] = THEME_MAGIC;
    for (i = 0; i < THEME_ROLES; i++) buf[1 + i] = theme_idx[i];
    buf[INTRO_AT] = intro_on ? 1 : 0;
    buf[INTRO_AT + 1] = intro_fps;
    buf[INTRO_AT + 2] = intro_pause;
    buf[INTRO_AT + 3] = intro_in;
    buf[INTRO_AT + 4] = intro_out;
    h = ti_OpenVar(THEME_VAR, "w", OS_TYPE_APPVAR);
    if (!h) return false;
    n = ti_Write(buf, 1, sizeof(buf), h);
    if (n == sizeof(buf)) ti_SetArchiveStatus(true, h);   /* keep FILERCFG in archive */
    ti_Close(h);
    return n == sizeof(buf);
}

/* ---------- Settings ----------
 * Mode opens the settings menu: Intro settings, Color settings, About. Up/Down
 * picks a row and Enter opens it. In the menu, Mode or Clear closes the box. On
 * a page, Mode or Clear goes back to the menu.
 *   Colors: Up/Down picks a role, Left/Right or 0-9 changes its preset (shown at
 *     once). Enter saves and goes back. Mode or Clear drops the unsaved changes.
 *   Intro: Left/Right or Enter turns the startup intro on or off, and saves at
 *     once. Play intro now runs it straight away, from the file list only.
 *   About: placeholder text. */

void settings_begin(void) {
    settings_open = true;
    settings_page = SET_MENU;
    settings_menu_sel = 0;
    settings_typing = false;
    needs_redraw = true;
}

/* Back from a page to the menu. */
static void settings_back_to_menu(void) {
    settings_page = SET_MENU;
    settings_typing = false;
    needs_redraw = true;
}

/* Leaves the Colors page: save keeps the changes, otherwise they are reverted. */
static void settings_colors_done(bool save) {
    if (save) {
        if (theme_save()) {
            memcpy(theme_saved, theme_idx, THEME_ROLES);
            set_status("Colors saved");
        } else {
            set_status("Colors not saved (AppVar error)");
        }
    } else {
        memcpy(theme_idx, theme_saved, THEME_ROLES);
        theme_apply();
        set_status("Colors reverted");
    }
    settings_back_to_menu();
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

static void settings_colors_key(uint8_t key) {
    int d = settings_digit(key);

    /* Digits type the value directly: 2, 5, 5 gives 255. A digit that would make
     * it over 255 starts a new value instead. */
    if (d >= 0) {
        int n = settings_typing ? settings_num * 10 + d : d;
        if (n > 255) n = d;
        settings_num = (uint8_t)n;
        settings_typing = true;
        theme_idx[settings_sel] = settings_num;
        theme_apply();
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
        theme_apply();
    } else if (key == sk_Right) {
        theme_idx[settings_sel]++;      /* wraps 255 -> 0 */
        theme_apply();
    } else if (key == sk_Enter) {
        settings_colors_done(true);
        return;
    } else if (key == sk_Mode || key == sk_Clear) {
        settings_colors_done(false);
        return;
    }
    needs_redraw = true;
    settings_fast = true;
}

/* Leaves the Intro page: the changes are saved once, here. */
static void settings_intro_leave(void) {
    if (!intro_dirty) return;
    intro_dirty = false;
    if (theme_save()) set_status("Intro settings saved");
    else set_status("Intro settings not saved (AppVar error)");
}

/* v moved by step, kept inside lo..hi. */
static uint8_t settings_step(uint8_t v, int step, int lo, int hi) {
    return clamp_u8((int)v + step, lo, hi);
}

static void settings_intro_key(uint8_t key) {
    int step = 0;   /* Left -1, Right +1 */

    if (key == sk_Up) {
        settings_sel = (uint8_t)((settings_sel + SETTINGS_INTRO_ROWS - 1) % SETTINGS_INTRO_ROWS);
    } else if (key == sk_Down) {
        settings_sel = (uint8_t)((settings_sel + 1) % SETTINGS_INTRO_ROWS);
    } else if (key == sk_Mode || key == sk_Clear) {
        settings_intro_leave();
        settings_back_to_menu();
        return;
    } else if (key == sk_Left || key == sk_Right || key == sk_Enter) {
        if (key == sk_Left) step = -1;
        if (key == sk_Right) step = 1;
        switch (settings_sel) {
            case 0:     /* on/off: Left, Right and Enter all flip it */
                intro_on = !intro_on;
                intro_dirty = true;
                break;
            case 1:     /* Play intro now: runs after this key, from the file list */
                if (key == sk_Enter) {
                    settings_intro_leave();
                    intro_preview = true;
                    settings_open = false;
                }
                break;
            case 2:     /* frame rate, whole frames a second */
                if (step) {
                    intro_fps = settings_step(intro_fps, step, INTRO_FPS_MIN, INTRO_FPS_MAX);
                    intro_dirty = true;
                }
                break;
            case 3:     /* pause on the title, tenths of a second */
                if (step) {
                    intro_pause = settings_step(intro_pause, step, 0, INTRO_PAUSE_MAX);
                    intro_dirty = true;
                }
                break;
            case 4:     /* in time: the letters fly in, tenths of a second */
                if (step) {
                    intro_in = settings_step(intro_in, step, INTRO_TIME_MIN, INTRO_TIME_MAX);
                    intro_dirty = true;
                }
                break;
            default:    /* out time: subtitle and icon move out, tenths of a second */
                if (step) {
                    intro_out = settings_step(intro_out, step, INTRO_TIME_MIN, INTRO_TIME_MAX);
                    intro_dirty = true;
                }
                break;
        }
    }
    needs_redraw = true;
}

static void settings_about_key(uint8_t key) {
    if (key == sk_Enter || key == sk_Mode || key == sk_Clear) settings_back_to_menu();
}

static void settings_menu_key(uint8_t key) {
    if (key == sk_Up) {
        settings_menu_sel = (uint8_t)((settings_menu_sel + SETTINGS_MENU_ROWS - 1) % SETTINGS_MENU_ROWS);
    } else if (key == sk_Down) {
        settings_menu_sel = (uint8_t)((settings_menu_sel + 1) % SETTINGS_MENU_ROWS);
    } else if (key == sk_Enter) {
        if (settings_menu_sel == 0) {
            settings_page = SET_INTRO;
            settings_sel = 0;
        } else if (settings_menu_sel == 1) {
            settings_page = SET_COLORS;
            settings_sel = 0;
            settings_top = 0;
            settings_typing = false;
        } else {
            settings_page = SET_ABOUT;
        }
    } else if (key == sk_Mode || key == sk_Clear) {
        settings_open = false;
    }
    needs_redraw = true;
}

/* Every key while the settings box is open goes here. */
void settings_key(uint8_t key) {
    switch (settings_page) {
        case SET_COLORS: settings_colors_key(key); break;
        case SET_INTRO:  settings_intro_key(key);  break;
        case SET_ABOUT:  settings_about_key(key);  break;
        default:         settings_menu_key(key);   break;
    }
}

/* One row of the settings box: the highlight when it is selected, then the label. */
static void settings_row(int x, int w, int ry, const char *label, int sel) {
    if (sel) {
        gfx_SetColor(T_SEL);
        gfx_FillRectangle(x + 3, ry, w - 6, 16);
    }
    text_at(x + 8, ry + 4, label, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
}

/* The Colors page rows: each row's swatch is the role's current palette slot, so it
 * shows the color as it is now. Only the rows from settings_top down fit; the arrows
 * show more above or below. */
static void draw_colors_rows(int x, int y, int w) {
    int i, ry, last;

    last = settings_top + SETTINGS_VISIBLE;
    if (last > THEME_ROLES) last = THEME_ROLES;
    for (i = settings_top; i < last; i++) {
        int sel = i == settings_sel;
        ry = y + 20 + (i - settings_top) * 18;
        settings_row(x, w, ry, THEME_ROLE_NAME[i], sel);
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
}

/* The Intro page rows. The values sit in one column at the right. */
static void draw_intro_rows(int x, int y, int w) {
    static const char *const names[SETTINGS_INTRO_ROWS] = {
        "Intro at startup", "Play intro now", "Frame rate", "Pause", "In time", "Out time",
    };
    int i, ry;

    for (i = 0; i < SETTINGS_INTRO_ROWS; i++) {
        int sel = i == settings_sel;
        char val[16] = "";
        ry = y + 20 + i * 18;
        settings_row(x, w, ry, names[i], sel);
        switch (i) {
            case 0: fmt(val, "%s", intro_on ? "On" : "Off"); break;
            case 2: fmt(val, "%d fps", intro_fps); break;
            case 3: fmt(val, "%d.%d s", intro_pause / 10, intro_pause % 10); break;
            case 4: fmt(val, "%d.%d s", intro_in / 10, intro_in % 10); break;
            case 5: fmt(val, "%d.%d s", intro_out / 10, intro_out % 10); break;
            default: break;   /* Play intro now has no value */
        }
        if (val[0]) text_at(x + w - 90, ry + 4, val, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
    }
}

/* Draws the settings box over the screen. Every page is the same size, so one page
 * never leaves pixels of another behind. */
void draw_settings(void) {
    int x = 16, y = 4, w = SCREEN_W - 32;
    int h = SETTINGS_BOX_H;

    gfx_SetColor(T_OUT);
    gfx_FillRectangle(x, y, w, h);
    gfx_SetColor(T_BG);
    gfx_FillRectangle(x + 1, y + 1, w - 2, h - 2);
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(x + 1, y + 1, w - 2, 16);

    switch (settings_page) {
        case SET_COLORS:
            text_at(x + 6, y + 4, "Settings: Colors", T_TOPTXT, T_TOP);
            draw_colors_rows(x, y, w);
            text_at(x + 6, y + h - 26, "Up/Down: Pick  Left/Right or 0-9: Value", T_TXT, T_BG);
            text_at(x + 6, y + h - 13, "Enter: Save   Mode/Clear: Cancel", T_TXT, T_BG);
            break;
        case SET_INTRO:
            text_at(x + 6, y + 4, "Settings: Intro", T_TOPTXT, T_TOP);
            draw_intro_rows(x, y, w);
            text_at(x + 6, y + h - 26, "Up/Down: Pick  Left/Right: Change", T_TXT, T_BG);
            text_at(x + 6, y + h - 13, "Enter: Toggle/Play   Mode/Clear: Back", T_TXT, T_BG);
            break;
        case SET_ABOUT:
            text_at(x + 6, y + 4, "Settings: About", T_TOPTXT, T_TOP);
            text_at(x + 8, y + 24, "FileR", T_TXT, T_BG);
            {
                char version[16];
                fmt(version, "Version %d.%d.%d",
                    APP_VER_MAJ,
                    APP_VER_MIN,
                    APP_VER_PTCH);
                text_at(x + 8, y + 42, version, T_TXT, T_BG);
            }
            text_at(x + 8, y + 60, "File manager for the TI-84 Plus CE", T_TXT, T_BG);
            text_at(x + 8, y + 69, "(or similar) with USB support!", T_TXT, T_BG);
            text_at(x + 8, y + 87, "Developed by Peltlas", T_TXT, T_BG);
            text_at(x + 8, y + 105, "Special thanks to CEAshell & Cesium!", T_TXT, T_BG);
            text_at(x + 8, y + 114, "This wouldn't be possible without them.", T_TXT, T_BG);
            text_at(x + 8, y + 132, "(C) 2026 Peltlas Vulpine", T_TXT, T_BG);
            text_at(x + 8, y + 150, "https://github.com/peltlasvulpine/FileR", T_TXT, T_BG);
            text_at(x + 6, y + h - 13, "Enter, Mode or Clear: Back", T_TXT, T_BG);
            break;
        default:
            text_at(x + 6, y + 4, "Settings", T_TOPTXT, T_TOP);
            settings_row(x, w, y + 20, "Intro settings", settings_menu_sel == 0);
            settings_row(x, w, y + 38, "Color settings", settings_menu_sel == 1);
            settings_row(x, w, y + 56, "About", settings_menu_sel == 2);
            text_at(x + 6, y + h - 26, "Up/Down: Pick  Enter: Open", T_TXT, T_BG);
            text_at(x + 6, y + h - 13, "Mode/Clear: Close", T_TXT, T_BG);
            break;
    }
}
