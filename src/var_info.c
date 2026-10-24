#include <tice.h>
#include <graphx.h>
#include <fileioc.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "var_info.h"

/* Palette indices, the same as main.c. */
/* Theme palette slots, the same numbers as main.c, so the panel follows the
 * colors chosen in the settings menu. */
#define VI_BG     0x40   /* panel background */
#define VI_TOP    0x41   /* title bar */
#define VI_TXT    0x44   /* text */
#define VI_TOPTXT 0x46   /* title bar text */
#define VI_OUT    0x47   /* outline */

/* Panel placement: right side of the list area (list starts at y 38, 10 rows of 18). */
#define PANEL_X  176
#define PANEL_Y  38
#define PANEL_W  144
#define PANEL_H  180

static bool open_now = false;
static char vname[16];
static uint8_t vtype;
static bool varchived;
static uint16_t vsize;
static bool usb_mode = false;    /* the panel shows the USB drive, not a variable */
static int usb_parts = 0;
static bool usb_mounted = false;

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
        default:                sprintf(buf, "Type 0x%02X", (unsigned)t); break;
    }
}

bool var_info_is_open(void) {
    return open_now;
}

bool var_info_is_usb(void) {
    return open_now && usb_mode;
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

void var_info_open(const char *name, uint8_t type) {
    uint8_t h;

    strncpy(vname, name, sizeof(vname) - 1);
    vname[sizeof(vname) - 1] = 0;
    vtype = type;
    usb_mode = false;
    varchived = false;
    vsize = 0;
    h = ti_OpenVar(vname, "r", vtype);
    if (h) {
        varchived = ti_IsArchived(h) != 0;
        vsize = ti_GetSize(h);
        ti_Close(h);
    }
    open_now = true;
}

bool var_info_key(uint8_t key) {
    uint8_t h;
    bool want;

    if (key == sk_Window) {
        open_now = false;
        return false;
    }
    if (key == sk_2nd && !usb_mode) {
        want = !varchived;
        h = ti_OpenVar(vname, "r", vtype);
        if (!h) return false;
        if (ti_SetArchiveStatus(want, h)) varchived = want;
        ti_Close(h);
        return true;
    }
    return false;
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

    sprintf(line, "Mounted: %s", usb_mounted ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 24, line, VI_TXT, VI_BG);
    sprintf(line, "Partitions: %d", usb_parts);
    vi_text(PANEL_X + 6, PANEL_Y + 40, line, VI_TXT, VI_BG);

    vi_text(PANEL_X + 6, PANEL_Y + 104, "Replug to remount", VI_TXT, VI_BG);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 30, "2nd: unmount", VI_TXT, VI_BG);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 16, "Window: close", VI_TXT, VI_BG);
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

    sprintf(line, "Name: %s", vname);
    vi_text(PANEL_X + 6, PANEL_Y + 24, line, VI_TXT, VI_BG);
    sprintf(line, "Type: %s", type);
    vi_text(PANEL_X + 6, PANEL_Y + 40, line, VI_TXT, VI_BG);
    sprintf(line, "Archived: %s", varchived ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 56, line, VI_TXT, VI_BG);
    sprintf(line, "Locked: %s", vtype == OS_TYPE_PROT_PRGM ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 72, line, VI_TXT, VI_BG);
    sprintf(line, "Size: %u bytes", (unsigned)vsize);
    vi_text(PANEL_X + 6, PANEL_Y + 88, line, VI_TXT, VI_BG);

    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 30, "2nd: toggle archive", VI_TXT, VI_BG);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 16, "Window: close", VI_TXT, VI_BG);
}
