#include <tice.h>
#include <graphx.h>
#include <fileioc.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "var_info.h"

/* Palette indices, the same as main.c. */
#define VI_BLACK 0x00
#define VI_WHITE 0xFF
#define VI_BLUE  0x32

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

void var_info_open(const char *name, uint8_t type) {
    uint8_t h;

    strncpy(vname, name, sizeof(vname) - 1);
    vname[sizeof(vname) - 1] = 0;
    vtype = type;
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

    if (key == sk_Enter) {
        open_now = false;
        return false;
    }
    if (key == sk_2nd) {
        want = !varchived;
        h = ti_OpenVar(vname, "r", vtype);
        if (!h) return false;
        if (ti_SetArchiveStatus(want, h)) varchived = want;
        ti_Close(h);
        return true;
    }
    return false;
}

void var_info_draw(void) {
    char line[40];
    char type[20];

    if (!open_now) return;
    type_label(vtype, type);

    /* Black border, white body, blue title bar. */
    gfx_SetColor(VI_BLACK);
    gfx_FillRectangle(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    gfx_SetColor(VI_WHITE);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, PANEL_H - 2);
    gfx_SetColor(VI_BLUE);
    gfx_FillRectangle(PANEL_X + 1, PANEL_Y + 1, PANEL_W - 2, 16);
    vi_text(PANEL_X + 6, PANEL_Y + 5, "Variable", VI_WHITE, VI_BLUE);

    sprintf(line, "Name: %s", vname);
    vi_text(PANEL_X + 6, PANEL_Y + 24, line, VI_BLACK, VI_WHITE);
    sprintf(line, "Type: %s", type);
    vi_text(PANEL_X + 6, PANEL_Y + 40, line, VI_BLACK, VI_WHITE);
    sprintf(line, "Archived: %s", varchived ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 56, line, VI_BLACK, VI_WHITE);
    sprintf(line, "Locked: %s", vtype == OS_TYPE_PROT_PRGM ? "Yes" : "No");
    vi_text(PANEL_X + 6, PANEL_Y + 72, line, VI_BLACK, VI_WHITE);
    sprintf(line, "Size: %u bytes", (unsigned)vsize);
    vi_text(PANEL_X + 6, PANEL_Y + 88, line, VI_BLACK, VI_WHITE);

    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 30, "2nd: toggle archive", VI_BLACK, VI_WHITE);
    vi_text(PANEL_X + 6, PANEL_Y + PANEL_H - 16, "Enter: close", VI_BLACK, VI_WHITE);
}
