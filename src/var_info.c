#include <tice.h>
#include <graphx.h>
#include <fileioc.h>
#include <ti/vars.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "fmt.h"
#include "var_info.h"

/* Theme palette slots, the same numbers as main.c, so the panel follows the
 * colors chosen in the settings menu. */
#define VI_BG     0x40   /* panel background */
#define VI_TOP    0x41   /* title bar */
#define VI_TXT    0x44   /* text */
#define VI_TOPTXT 0x46   /* title bar text */
#define VI_OUT    0x47   /* outline */
#define VI_HIDDEN 0x4A   /* name of a hidden program, the same slot as T_HIDDEN in main.c */

/* Panel placement: right side of the list area (list starts at y 38, 10 rows of 18). */
#define PANEL_X  176
#define PANEL_Y  38
#define PANEL_W  144
#define PANEL_H  180

static bool open_now = false;
static char vname[16];
static uint8_t vtype;
static bool varchived;             /* the archive state the variable is in now */
static uint16_t vsize;
static bool usb_mode = false;      /* the panel shows the USB drive, not a variable */
static int usb_parts = 0;
static bool usb_mounted = false;
static bool vhidden = false;       /* the hidden state the program is in now */
static char vshown[16];            /* the name as the list shows it (main.c works it out) */
static bool compiled = false;      /* a compiled program: always locked, can't be unlocked */

/* The choices shown in the panel. They are applied when the panel closes, so a choice
 * changed and changed back does nothing, and the archive is only touched when its
 * choice really differs from the current state. */
static bool want_arch = false;
static bool want_lock = false;
static bool want_hidden = false;

static void vi_text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    gfx_SetTextFGColor(fg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextTransparentColor(bg);
    gfx_PrintStringXY(s, x, y);
}

/* Writes the type name into buf. */
static void type_label(uint8_t t, char *buf) {
    switch (t) {
        case OS_TYPE_PRGM:      strcpy(buf, "Program");      break;
        case OS_TYPE_PROT_PRGM: strcpy(buf, "Program");      break;
        case OS_TYPE_APPVAR:    strcpy(buf, "AppVar");       break;
        case OS_TYPE_STR:       strcpy(buf, "String");       break;
        case OS_TYPE_EQU:       strcpy(buf, "Equation");     break;
        case OS_TYPE_CPLX:      strcpy(buf, "Complex");      break;
        case OS_TYPE_REAL_LIST: strcpy(buf, "Real list");    break;
        case OS_TYPE_CPLX_LIST: strcpy(buf, "Complex list"); break;
        default:                fmt(buf, "Type 0x%02X", (unsigned)t); break;
    }
}

bool var_info_is_open(void) {
    return open_now;
}

bool var_info_is_usb(void) {
    return open_now && usb_mode;
}

static bool is_prgm(uint8_t t) {
    return t == OS_TYPE_PRGM || t == OS_TYPE_PROT_PRGM;
}

bool var_info_is_prgm(void) {
    return open_now && !usb_mode && is_prgm(vtype);
}

/* Only TI-BASIC programs can be locked or unlocked. */
static bool can_lock(void) {
    return is_prgm(vtype) && !compiled;
}

void var_info_close(void) {
    open_now = false;
}

void var_info_open_usb(int parts, bool mounted) {
    usb_mode = true;
    usb_parts = parts;
    usb_mounted = mounted;
    open_now = true;
}

/* A compiled program starts with EF 7B, the same test Cesium uses. TI-BASIC programs
 * don't. */
static bool read_compiled(const char *name, uint8_t type) {
    uint8_t h;
    uint8_t b[2];
    bool c = false;

    h = ti_OpenVar(name, "r", type);
    if (!h) return false;
    if (ti_Read(b, 1, 2, h) == 2 && b[0] == 0xEF && b[1] == 0x7B) c = true;
    ti_Close(h);
    return c;
}

void var_info_open(const char *name, uint8_t type, const char *shown, bool hidden) {
    uint8_t h;

    strncpy(vname, name, sizeof(vname) - 1);
    vname[sizeof(vname) - 1] = 0;
    strncpy(vshown, shown, sizeof(vshown) - 1);
    vshown[sizeof(vshown) - 1] = 0;
    vtype = type;
    usb_mode = false;
    varchived = false;
    vsize = 0;
    vhidden = hidden;
    compiled = is_prgm(type) && read_compiled(vname, type);
    h = ti_OpenVar(vname, "r", vtype);
    if (h) {
        varchived = ti_IsArchived(h) != 0;
        vsize = ti_GetSize(h);
        ti_Close(h);
    }
    want_arch = varchived;
    want_lock = vtype == OS_TYPE_PROT_PRGM;
    want_hidden = vhidden;
    open_now = true;
}

/* Hides or shows the program by XOR 0x40 on its first name byte, in place in the VAT,
 * as CEaShell does. That gives A <-> 0x01 ... Z <-> 0x1A, and theta 0x5B <-> 0x1B.
 * The entry pointer from os_ChkFindSym is the type byte, so the name's first byte is
 * 7 bytes below it. An archived program is taken out of the archive first and stays
 * in RAM after (CEaShell does the same). */
static bool toggle_hidden(void) {
    void *entry = NULL;
    void *data = NULL;
    uint8_t *name0;
    uint8_t h;
    uint8_t c;
    bool was_arch = varchived;

    if (was_arch) {
        h = ti_OpenVar(vname, "r", vtype);
        if (h) { ti_SetArchiveStatus(false, h); ti_Close(h); }
    }
    if (!os_ChkFindSym(vtype, vname, &entry, &data) || !entry) {
        if (was_arch) {
            h = ti_OpenVar(vname, "r", vtype);
            if (h) { ti_SetArchiveStatus(true, h); ti_Close(h); }
        }
        return false;
    }
    varchived = false;
    name0 = (uint8_t *)entry - 7;
    *name0 ^= 0x40;
    c = *name0;
    vname[0] = (char)c;
    vhidden = !vhidden;
    if (c >= 0x01 && c <= 0x1A) vshown[0] = (char)(c + 'A' - 1);
    else if (c == 0x1B || c == 0x5B) vshown[0] = (char)0x40;
    else vshown[0] = (char)c;
    return true;
}

/* Locks (0x06) or unlocks (0x05) the program by changing its VAT type byte. The caller
 * has taken the program out of the archive first. */
static bool set_lock(bool lock) {
    void *entry = NULL;
    void *data = NULL;
    uint8_t *tb;
    uint8_t want = lock ? OS_TYPE_PROT_PRGM : OS_TYPE_PRGM;
    bool ok = false;

    if (os_ChkFindSym(vtype, vname, &entry, &data) && entry) {
        tb = (uint8_t *)entry;
        *tb = (uint8_t)((*tb & 0xE0) | want);
        vtype = want;
        ok = true;
    }
    return ok;
}

/* Moves the variable into (want true) or out of the archive, and records the result. */
static void set_archived(bool want) {
    uint8_t h = ti_OpenVar(vname, "r", vtype);
    if (!h) return;
    if (ti_SetArchiveStatus(want, h)) varchived = want;
    ti_Close(h);
}

/* Applies the pending choices. Hide and lock first, while the variable is out of the
 * archive (one move at most), then the archive choice if it still differs. Returns true
 * if anything changed. */
static bool apply_pending(void) {
    bool changed = false;

    if (want_hidden != vhidden && is_prgm(vtype)) {
        if (varchived) set_archived(false);
        if (toggle_hidden()) changed = true;
    }
    if (can_lock() && want_lock != (vtype == OS_TYPE_PROT_PRGM)) {
        if (varchived) set_archived(false);
        if (set_lock(want_lock)) changed = true;
    }
    if (want_arch != varchived) {
        set_archived(want_arch);
        changed = true;
    }
    return changed;
}

/* Applies the pending choices and closes the panel. Returns true if the list changed, so
 * the caller can refresh it. Called on Window and Clear, and before a rename. */
bool var_info_apply(void) {
    bool changed = false;

    if (open_now && !usb_mode) changed = apply_pending();
    open_now = false;
    return changed;
}

/* Handles one key while the panel is open. Changes only the pending choices; they take
 * effect when the panel closes. Returns true if the list needs a refresh. */
bool var_info_key(uint8_t key) {
    if (key == sk_Window || key == sk_Clear) return var_info_apply();
    if (usb_mode) return false;
    if (key == sk_Zoom) {
        want_arch = !want_arch;
    } else if (key == sk_Trace && can_lock()) {
        want_lock = !want_lock;
    } else if (key == sk_Graph && is_prgm(vtype)) {
        want_hidden = !want_hidden;
    }
    return false;
}

/* Bottom-bar labels for the panel's five keys: Y=, Window, Zoom, Trace, Graph. An empty
 * label means the key does nothing for this item. */
void var_info_labels(const char **lab) {
    lab[0] = "Rename";
    lab[1] = "Close";
    lab[2] = want_arch ? "Unarch" : "Archive";
    lab[3] = can_lock() ? (want_lock ? "Unlock" : "Lock") : "";
    lab[4] = is_prgm(vtype) ? (want_hidden ? "Show" : "Hide") : "";
}

/* The USB drive panel. */
static void draw_usb(void) {
    char line[40];

    gfx_SetColor(VI_OUT);
    gfx_FillRectangle(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    gfx_SetColor(VI_BG);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, PANEL_H - 2);
    gfx_SetColor(VI_TOP);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, 16);
    vi_text(PANEL_X + 6, PANEL_Y + 5, "USB drive", VI_TOPTXT, VI_TOP);

    fmt(line, "Mounted: %s", usb_mounted ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 24, line, VI_TXT, VI_BG);
    fmt(line, "Partitions: %d", usb_parts);
    vi_text(PANEL_X + 6, PANEL_Y + 40, line, VI_TXT, VI_BG);

    vi_text(PANEL_X + 6, PANEL_Y + 104, "Replug to remount", VI_TXT, VI_BG);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 30, "2nd: Unmount", VI_TXT, VI_BG);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 16, "Window: Close", VI_TXT, VI_BG);
}

void var_info_draw(void) {
    char line[40];
    char type[20];

    if (!open_now) return;
    if (usb_mode) {
        draw_usb();
        return;
    }
    type_label(vtype, type);

    /* Black border, white body, blue title bar. */
    gfx_SetColor(VI_OUT);
    gfx_FillRectangle(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    gfx_SetColor(VI_BG);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, PANEL_H - 2);
    gfx_SetColor(VI_TOP);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, 16);
    vi_text(PANEL_X + 6, PANEL_Y + 5, "Variable", VI_TOPTXT, VI_TOP);

    fmt(line, "Name: %s", vshown);
    vi_text(PANEL_X + 6, PANEL_Y + 24, line, want_hidden ? VI_HIDDEN : VI_TXT, VI_BG);
    fmt(line, "Type: %s", type);
    vi_text(PANEL_X + 6, PANEL_Y + 40, line, VI_TXT, VI_BG);
    fmt(line, "Archived: %s", want_arch ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 56, line, VI_TXT, VI_BG);
    fmt(line, "Locked: %s", want_lock ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 72, line, VI_TXT, VI_BG);
    fmt(line, "Size: %u bytes", (unsigned)vsize);
    vi_text(PANEL_X + 6, PANEL_Y + 88, line, VI_TXT, VI_BG);
    fmt(line, "Hidden: %s", want_hidden ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 104, line, VI_TXT, VI_BG);
    if (is_prgm(vtype)) {
        fmt(line, "Compiled: %s", compiled ? "Yes" : "No");
        vi_text(PANEL_X + 6, PANEL_Y + 120, line, VI_TXT, VI_BG);
    }

    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 16, "Window: Close", VI_TXT, VI_BG);
}
