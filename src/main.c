#include <tice.h>
#include <graphx.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <usbdrvce.h>
#include <msddrvce.h>
#include <fatdrvce.h>
#include <fileioc.h>
#include <sys/power.h>   /* boot_GetBatteryStatus, boot_BatteryCharging */
#include "icon/gfx.h"   /* convimg output: all icon sprites + palette */
#include "text_viewer.h"    /* text viewer for .txt files */
#include "image_viewer.h"   /* image viewer for .bmp files */
#include "var_info.h"     /* info panel for calculator variables */
#include "keys.h"         /* 2nd and alpha modifiers */
#include <time.h>          /* clock(): status message timeouts */

/* ---------- Layout ---------- */

#define SCREEN_W      320
#define SCREEN_H      240
#define TITLE_H       20
#define FILTER_Y      24
#define LIST_Y        38
#define ROW_H         18          /* tall enough for the 18px icons */
#define VISIBLE_ROWS  10
#define STATUS_H      14
#define MAX_ITEMS     200         /* how many rows the list can hold */
#define MAX_PARTS     4
#define SEARCH_MAX    16          /* search text length, including room for NUL */
#define PATH_MAX      128
#define ICON_X        4           /* left icon column */
#define ICON_W        16          /* icon box: centers each sprite inside */
#define ICON_H        18
#define TEXT_X        24          /* names start after the icon column */

/* Right-side markers. They sit next to the type label and are never
 * drawn on top of the left icon. */
#define ARCHIVE_X     (SCREEN_W - 100)
#define LOCK_X        (SCREEN_W - 80)
#define LABEL_X       (SCREEN_W - 60)
#define PRGM_ICON_X   (ARCHIVE_X - ICON_W - 2)   /* program's own icon, left of archive */
#define PRGM_TRANSPARENT 0xDF   /* palette index treated as see-through in program icons */

/* Palette indices. The icon palette (mypalette) fills entries 0..13 and
 * leaves 0x00 black and 0xFF white as they are; 0x10 is the default blue. */
#define C_BLACK  0x00
#define C_WHITE  0xFF
/* Palette slots outside mypalette (it uses 0..36). They are set at startup
 * in restore_palette(). Using a slot inside mypalette would overwrite an icon color. */
#define C_BLUE   0x32          /* title bar */
#define C_SELECT 0x7B          /* selected row */

/* Colors, as 8-bit RGB: { red, green, blue }. Change these to recolor the title bar and selected row. */
static const uint8_t TITLE_RGB[3]  = { 44, 100, 212 };   /* title bar */
static const uint8_t SELECT_RGB[3] = { 130, 182, 245 };  /* selected row */

/* ---------- Types ---------- */

enum location { LOC_CALC, LOC_PART, LOC_DRIVE };
enum kind { KIND_USB, KIND_PARENT, KIND_PART, KIND_DIR, KIND_FILE, KIND_VAR };
enum storage_filter { STORE_ALL, STORE_ARCHIVE, STORE_RAM, STORE_COUNT };
enum type_filter { TYPE_ALL, TYPE_APPVAR, TYPE_PRGM, TYPE_VAR, TYPE_COUNT };

/* Which icon goes on top of the base file icon. */
enum overlay { OV_NONE, OV_APPVAR, OV_PRGM, OV_APP, OV_VAR, OV_TEXT, OV_HTML, OV_IMAGE };

typedef struct {
    char name[16];
    uint8_t kind;
    uint8_t type;       /* OS type for calculator variables; partition index for KIND_PART */
    uint8_t overlay;    /* enum overlay: icon drawn on top of the file icon */
    bool archived;      /* calculator variables only */
} item_t;


/* ---------- State ---------- */

static msd_t msd;
static fat_t fat;
static bool msd_open = false;     /* msd_Open succeeded */
static bool msd_ready = false;    /* device open and partitions found */
static bool mounted = false;      /* FAT volume open on one partition */
static bool fat_open = false;     /* fat_Open succeeded and fat_Close not yet called */

static int part_count = 0;
static uint32_t part_lba[MAX_PARTS];
static char part_label[MAX_PARTS][12];   /* FAT volume label, "" if none */
static char usb_label[16] = "";          /* right-side text on the USB row */

static char status[40] = "Waiting for USB drive...";
static bool status_changed = true;

static item_t items[MAX_ITEMS];
static int item_count = 0;

static enum location location = LOC_CALC;
static char drive_path[PATH_MAX] = "/";

static enum storage_filter store_filter = STORE_ALL;
static enum type_filter type_filter = TYPE_ALL;
static int selected = 0;
static int top = 0;
static bool needs_redraw = true;

static char search[SEARCH_MAX] = "";
static bool search_mode = false;   /* y= : letters type into the search */

#define STATUS_SECONDS 5
static uint32_t status_at = 0;     /* clock() value when the message was set */
static bool status_timed = false;  /* message clears after STATUS_SECONDS */

static const char *storage_names[] = { "All", "Archive", "RAM" };
static const char *type_names[]    = { "All", "AppVar", "Prgm", "Var" };

/* ---------- Status ---------- */

static void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    status_changed = true;
    status_at = (uint32_t)clock();
    status_timed = true;
}

/* ---------- FAT callbacks ---------- */

static uint24_t read_cb(void *usr, uint32_t lba, uint24_t count, void *buf) {
    (void)usr;
    return msd_Read(&msd, lba, count, buf);
}

/* Read-only: accept writes the FAT driver makes internally (e.g. the dirty
 * bit on open) but discard the data, so the drive is never modified. */
static uint24_t write_stub(void *usr, uint32_t lba, uint24_t count, const void *buf) {
    (void)usr; (void)lba; (void)buf;
    return count;
}

/* Failure to open the drive. If the calculator is charging and no data is
 * open, say so, since the cable is only supplying power. */
static void usb_fail(const char *msg) {
    if (boot_BatteryCharging()) {
        set_status("Charging, no data received");
    } else {
        set_status(msg);
    }
}

/* ---------- Mount / unmount ---------- */

/* Closes the FAT volume if it is open. Safe to call when it isn't. */
static void fat_close_volume(void) {
    if (fat_open) {
        fat_Close(&fat);
        fat_open = false;
    }
}

/* Reads each partition's FAT volume label. A partition without a readable
 * label keeps an empty string and is shown as "Part N" instead. */
static void read_part_labels(void) {
    char label[13];
    int i, n;

    fat_close_volume();
    for (i = 0; i < part_count; i++) {
        part_label[i][0] = 0;
        if (fat_Open(&fat, read_cb, write_stub, NULL, part_lba[i]) != FAT_SUCCESS) continue;
        fat_open = true;
        if (fat_GetVolumeLabel(&fat, label) == FAT_SUCCESS) {
            label[12] = 0;
            n = (int)strlen(label);
            while (n > 0 && label[n - 1] == ' ') label[--n] = 0;
            strncpy(part_label[i], label, 11);
            part_label[i][11] = 0;
        }
        fat_close_volume();
    }
}

/* Opens the FAT volume on one partition. Returns true on success. */
static bool open_partition(int i) {
    fat_error_t ferr;
    char buf[40];

    /* Re-opening the FAT driver switches the volume, so any previous
     * partition is no longer the one being read. */
    mounted = false;
    fat_close_volume();
    ferr = fat_Open(&fat, read_cb, write_stub, NULL, part_lba[i]);
    if (ferr != FAT_SUCCESS) {
        sprintf(buf, "Drive: FAT open failed (%d)", (int)ferr);
        usb_fail(buf);
        return false;
    }
    fat_open = true;
    mounted = true;
    set_status("USB drive mounted");
    return true;
}

/* Called only once the device is enabled (addressed and configured).
 * Finds the partitions. One partition is opened right away; with several,
 * the user picks one from the partition list. */
static void mount_device(usb_device_t dev) {
    msd_partition_t parts[MAX_PARTS];
    uint8_t n, i;
    char buf[40];

    fat_close_volume();
    if (msd_open) {                  /* enable without a disconnect: start clean */
        msd_Close(&msd);
        msd_open = false;
    }
    if (msd_Open(&msd, dev) != MSD_SUCCESS) {
        usb_fail("Drive: msd_Open failed");
        return;
    }
    msd_open = true;

    n = msd_FindPartitions(&msd, parts, MAX_PARTS);
    if (n == 0) {
        usb_fail("Drive: no partition found");
        return;
    }

    part_count = n;
    for (i = 0; i < n; i++) part_lba[i] = parts[i].first_lba;
    msd_ready = true;

    sprintf(usb_label, "%d part%s", (int)n, n == 1 ? "" : "s");
    read_part_labels();

    if (n == 1) {
        open_partition(0);
    } else {
        sprintf(buf, "Found %d partitions, press Enter", (int)n);
        set_status(buf);
    }
}

static void unmount_device(void) {
    fat_close_volume();
    mounted = false;
    msd_ready = false;
    part_count = 0;
    usb_label[0] = 0;
    if (msd_open) {
        msd_Close(&msd);
        msd_open = false;
    }
}

static uint8_t batt_level = 0;       /* last value read from the calculator */
static bool batt_charging = false;

/* Reads the battery from the calculator. Called now and then from the main
 * loop, not on every redraw. */
static void battery_poll(void) {
    batt_level = boot_GetBatteryStatus();
    batt_charging = boot_BatteryCharging() != 0;
}

/* ---------- USB event handler ---------- */

static usb_error_t usb_handler(usb_event_t event, void *event_data,
                               void *callback_data) {
    (void)callback_data;

    switch (event) {
        case USB_DEVICE_CONNECTED_EVENT:
            battery_poll();
            /* Reset first; the device is set up in USB_DEVICE_ENABLED_EVENT. */
            usb_ResetDevice((usb_device_t)event_data);
            set_status("USB device found, resetting...");
            break;

        case USB_DEVICE_ENABLED_EVENT:
            battery_poll();
            mount_device((usb_device_t)event_data);
            break;

        case USB_DEVICE_DISCONNECTED_EVENT:
            battery_poll();
            unmount_device();
            set_status("USB drive removed");
            break;

        default:
            break;
    }
    return USB_SUCCESS;
}

/* ---------- Labels and overlays ---------- */

static const char *overlay_label(uint8_t ov) {
    switch (ov) {
        case OV_APPVAR: return "AppVar";
        case OV_PRGM:   return "Prgm";
        case OV_APP:    return "App";
        case OV_VAR:    return "Var";
        case OV_TEXT:   return "Text";
        case OV_HTML:   return "HTML";
        case OV_IMAGE:  return "Image";
        default:        return NULL;
    }
}

/* Calculator variable -> overlay, from its OS type. */
static uint8_t overlay_for_os_type(uint8_t type) {
    if (type == OS_TYPE_APPVAR) return OV_APPVAR;
    if (type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM) return OV_PRGM;
    return OV_VAR;
}

/* True if the extension after the last dot equals ext (any case). */
static bool ext_equals(const char *dot, const char *ext) {
    size_t n = strlen(ext);
    size_t i;

    if (strlen(dot) != n + 1) return false;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)dot[1 + i]) != ext[i]) return false;
    }
    return true;
}

/* Drive file -> overlay, from its extension (any case). */
static uint8_t overlay_for_filename(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return OV_NONE;
    if (ext_equals(dot, "txt")) return OV_TEXT;
    if (ext_equals(dot, "html") || ext_equals(dot, "htm")) return OV_HTML;
    if (ext_equals(dot, "png") || ext_equals(dot, "bmp")) return OV_IMAGE;
    if (tolower((unsigned char)dot[1]) != '8') return OV_NONE;
    if (tolower((unsigned char)dot[2]) != 'x') return OV_NONE;
    switch (tolower((unsigned char)dot[3])) {
        case 'v': return OV_APPVAR;
        case 'p': return OV_PRGM;
        case 'k': return OV_APP;
        default:  return OV_NONE;
    }
}

/* ---------- Calculator helpers ---------- */

static bool type_matches(uint8_t type) {
    switch (type_filter) {
        case TYPE_APPVAR: return type == OS_TYPE_APPVAR;
        case TYPE_PRGM:   return type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM;
        case TYPE_VAR:    return overlay_for_os_type(type) == OV_VAR;
        default:          return true;
    }
}

/* Looks up whether a variable is in the archive. Always called, so the
 * archive marker can be shown on every row. */
static bool is_archived(const char *name, uint8_t type) {
    uint8_t h;
    bool archived;

    h = ti_OpenVar(name, "r", type);
    if (!h) return false;
    archived = ti_IsArchived(h) != 0;
    ti_Close(h);
    return archived;
}

static bool storage_matches(bool archived) {
    switch (store_filter) {
        case STORE_ARCHIVE: return archived;
        case STORE_RAM:     return !archived;
        default:            return true;
    }
}

/* ---------- Search ---------- */

/* Case-insensitive "needle is inside hay". needle must already be lowercase. */
static bool contains_ci(const char *hay, const char *needle) {
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    size_t i, j;

    if (nn == 0) return true;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            if (tolower((unsigned char)hay[i + j]) != needle[j]) break;
        }
        if (j == nn) return true;
    }
    return false;
}

/* Keeps row 0 (the USB or ".." row) and only the rows whose names match. */
static void apply_search(void) {
    int i, j = 1;

    if (search[0] == 0) return;
    for (i = 1; i < item_count; i++) {
        if (contains_ci(items[i].name, search)) {
            if (j != i) items[j] = items[i];
            j++;
        }
    }
    item_count = j;
}

static void clear_search(void) {
    search[0] = 0;
}

/* ---------- Data refresh ---------- */

static void clamp_selection(void) {
    if (selected >= item_count) selected = item_count > 0 ? item_count - 1 : 0;
    if (selected < 0) selected = 0;
    if (top > selected) top = selected;
    if (selected >= top + VISIBLE_ROWS) top = selected - VISIBLE_ROWS + 1;
    if (top < 0) top = 0;
}

static void reset_view_position(void) {
    selected = 0;
    top = 0;
}

/* Calculator root: the USB entry first, then the variables that pass the filters. */
static void calc_refresh(void) {
    void *vat = NULL;
    uint8_t type;
    char *name;
    bool archived;
    item_t *it;

    it = &items[0];
    strcpy(it->name, "usb/");
    it->kind = KIND_USB;
    it->type = 0;
    it->overlay = OV_NONE;
    it->archived = false;
    item_count = 1;

    while (item_count < MAX_ITEMS && (name = ti_DetectAny(&vat, NULL, &type))) {
        if (!type_matches(type)) continue;
        archived = is_archived(name, type);
        if (!storage_matches(archived)) continue;

        it = &items[item_count];
        strncpy(it->name, name, 8);
        it->name[8] = 0;
        it->kind = KIND_VAR;
        it->type = type;
        it->overlay = overlay_for_os_type(type);
        it->archived = archived;
        item_count++;
    }
    apply_search();
    clamp_selection();
}

/* Partition list: ".." first, then one row per partition. */
static void part_refresh(void) {
    item_t *it;
    int i;

    it = &items[0];
    strcpy(it->name, "..");
    it->kind = KIND_PARENT;
    it->type = 0;
    it->overlay = OV_NONE;
    it->archived = false;
    item_count = 1;

    for (i = 0; i < part_count && item_count < MAX_ITEMS; i++) {
        it = &items[item_count];
        /* Shown like a folder: trailing "/", named by the volume label if there is one. */
        if (part_label[i][0]) {
            sprintf(it->name, "%s/", part_label[i]);
        } else {
            sprintf(it->name, "Part %d/", i + 1);
        }
        it->kind = KIND_PART;
        it->type = (uint8_t)i;
        it->overlay = OV_NONE;
        it->archived = false;
        item_count++;
    }
    apply_search();
    clamp_selection();
}

/* Drive folder: ".." first, then folders (with trailing "/") and files.
 * The local is named `fdir`, not `dir`, because `dir` is a sprite macro. */
static void drive_refresh(void) {
    fat_dir_t fdir;
    fat_dir_entry_t entry;
    item_t *it;

    item_count = 0;
    if (!mounted) {
        clamp_selection();
        return;
    }

    /* ".." always first; at drive root it returns to the calculator root. */
    it = &items[0];
    strcpy(it->name, "..");
    it->kind = KIND_PARENT;
    it->type = 0;
    it->overlay = OV_NONE;
    it->archived = false;
    item_count = 1;

    if (fat_OpenDir(&fat, drive_path, &fdir) != FAT_SUCCESS) {
        set_status("Drive: cannot open folder");
        clamp_selection();
        return;
    }
    while (item_count < MAX_ITEMS &&
           fat_ReadDir(&fdir, &entry) == FAT_SUCCESS &&
           entry.name[0] != 0) {
        if (entry.attrib & (FAT_HIDDEN | FAT_VOLLABEL)) continue;
        if (entry.name[0] == '.') continue;

        it = &items[item_count];
        strncpy(it->name, entry.name, 12);
        it->name[12] = 0;
        if (entry.attrib & FAT_DIR) {
            strcat(it->name, "/");
            it->kind = KIND_DIR;
            it->overlay = OV_NONE;
        } else {
            it->kind = KIND_FILE;
            it->overlay = overlay_for_filename(it->name);
        }
        it->type = 0;
        it->archived = false;
        item_count++;
    }
    fat_CloseDir(&fdir);
    apply_search();
    clamp_selection();
}

/* Rebuilds whichever list is on screen, so the search applies to it. */
static void refresh_current(void) {
    switch (location) {
        case LOC_CALC:  calc_refresh(); break;
        case LOC_PART:  part_refresh(); break;
        case LOC_DRIVE: drive_refresh(); break;
    }
}

/* ---------- Path helpers ---------- */

static void drive_go_up(void) {
    char *slash = strrchr(drive_path, '/');
    if (!slash || slash == drive_path) {
        strcpy(drive_path, "/");
    } else {
        *slash = 0;
    }
}

static void drive_go_down(const char *name) {
    size_t len = strlen(drive_path);
    size_t add = strlen(name);
    if (!(len > 0 && drive_path[len - 1] == '/')) {
        if (len + 1 >= PATH_MAX) return;
        strcat(drive_path, "/");
        len++;
    }
    if (len + add >= PATH_MAX) return;
    strcat(drive_path, name);
}

/* Folder names are stored with a trailing "/" for display; strip it. */
static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '/') s[n - 1] = 0;
}

/* ---------- Drawing ---------- */

/* Size of the selected USB file, if the list is on the drive and a file is highlighted. */
static bool selected_drive_file_size(uint32_t *size) {
    fat_file_t f;
    char fpath[PATH_MAX + 16];
    item_t *it;

    if (location != LOC_DRIVE || item_count == 0) return false;
    it = &items[selected];
    if (it->kind != KIND_FILE) return false;
    if (strcmp(drive_path, "/") == 0) {
        sprintf(fpath, "/%s", it->name);
    } else {
        sprintf(fpath, "%s/%s", drive_path, it->name);
    }
    if (fat_OpenFile(&fat, fpath, 0, &f) != FAT_SUCCESS) return false;
    *size = fat_GetFileSize(&f);
    fat_CloseFile(&f);
    return true;
}

/* Sets fg, bg and transparent color together so text never inherits a
 * stale color. The transparent color equals bg, so text is drawn over the
 * background that was filled first. */
static void text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    gfx_SetTextFGColor(fg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextTransparentColor(bg);
    gfx_PrintStringXY(s, x, y);
}

/* Key labels across the bottom bar, each centered under its top key (y=, Window,
 * Zoom, Trace, Graph) with the screen split into five equal columns. */
static void draw_key_legend(int y) {
    static const char *labels[5] = { "Search", "", "", "", "Store" };
    int i, cx, w;

    for (i = 0; i < 5; i++) {
        if (!labels[i][0]) continue;
        cx = i * (SCREEN_W / 5) + (SCREEN_W / 5) / 2;
        w = (int)gfx_GetStringWidth(labels[i]);
        text_at(cx - w / 2, y, labels[i], C_WHITE, C_BLACK);
    }
}

/* Draws a sprite centered inside the left icon box at row y. */
static void draw_in_box(gfx_rletsprite_t *s, int w, int h, int y) {
    gfx_RLETSprite(s, ICON_X + (ICON_W - w) / 2, y + (ROW_H - h) / 2);
}

/* Base file icon, then the type icon on top. Archive and lock are NOT
 * drawn here; they go on the right. */
static void draw_file_with_overlay(uint8_t overlay, int y) {
    draw_in_box(file, file_width, file_height, y);
    switch (overlay) {
        case OV_APPVAR: draw_in_box(appvar, appvar_width, appvar_height, y); break;
        case OV_PRGM:   draw_in_box(prgm, prgm_width, prgm_height, y);       break;
        case OV_APP:    draw_in_box(app, app_width, app_height, y);          break;
        case OV_VAR:    draw_in_box(var, var_width, var_height, y);          break;
        case OV_TEXT:   draw_in_box(text, text_width, text_height, y);       break;
        case OV_HTML:   draw_in_box(html, html_width, html_height, y);       break;
        case OV_IMAGE:  draw_in_box(image, image_width, image_height, y);    break;
        default:        break;
    }
}

static bool is_prgm_type(uint8_t type) {
    return type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM;
}

/* A program's icon, read from the file: 16x16, one palette byte per pixel,
 * after a 3-byte jump, a 0x01 marker and the 0x10 0x10 size. Stored in
 * gfx_sprite_t layout (width, height, pixels) so it can be drawn directly. */
#define ICON_PIXELS 256
static uint8_t icon_sprite[2 + ICON_PIXELS];

#define ICON_SCAN 32   /* how far into the program to look for the icon header */

/* Finds the icon in the first ICON_SCAN bytes. A jump (C3) is followed within a
 * few bytes by the marker 0x01 and the size 0x10 0x10; the 256 pixel bytes start
 * right after that. Returns the offset of the pixels, or -1 if not found. */
static int find_icon_header(const uint8_t *buf, int n) {
    int i, j;
    for (i = 0; i < n; i++) {
        if (buf[i] != 0xC3) continue;
        for (j = i + 3; j + 3 <= n && j <= i + 8; j++) {
            if (buf[j] == 0x01 && buf[j + 1] == 16 && buf[j + 2] == 16) {
                return j + 3;
            }
        }
    }
    return -1;
}

/* TI-BASIC icon: the marker 2A 3F 3E 2A within the first 128 bytes, then 256
 * bytes, one ASCII hex digit per pixel ('0'-'9', 'A'-'F'). Each digit selects a
 * palette index from basic_palette. */
#define BASIC_SCAN   128
#define BASIC_PIXELS 256
static uint8_t basic_head[BASIC_SCAN];
static uint8_t basic_pix[BASIC_PIXELS];

static const uint8_t basic_palette[16] = {
    0xDF, 0x18, 0xE0, 0x00, 0xF8, 0x06, 0xE4, 0x60,
    0x10, 0x1D, 0xE7, 0xFF, 0xDE, 0xBD, 0x94, 0x4A
};

#define TI_PAL_BASE 0x80            /* slots 0x80..0x8F hold the 16 TI-BASIC colors */
static uint16_t ti_pal[16];         /* the TI colors, copied from the startup palette */

/* Pixel value for one TI-BASIC icon character: the TI color slot, or the
 * transparent index where the color is 0xDF. White if it isn't a hex digit. */
static uint8_t basic_color(uint8_t ch) {
    uint8_t d;
    if (ch >= '0' && ch <= '9') d = ch - '0';
    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
    else return C_WHITE;
    if (basic_palette[d] == PRGM_TRANSPARENT) return PRGM_TRANSPARENT;
    return TI_PAL_BASE + d;
}

static bool icon_transparent = false;   /* true when the current icon is TI-BASIC (0xDF sees through) */

/* Searches the first half of the program for the marker, a chunk at a time. */
static bool read_basic_icon(const item_t *it) {
    uint8_t h;
    size_t size, limit, pos = 0;
    int got, i;
    long marker = -1;
    bool found = false;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    size = ti_GetSize(h);
    limit = size / 2;
    while (!found && pos < limit) {
        if (ti_Seek((int)pos, SEEK_SET, h) == EOF) break;
        got = (int)ti_Read(basic_head, 1, BASIC_SCAN, h);
        for (i = 0; i + 4 <= got; i++) {
            if (basic_head[i] == 0x2A && basic_head[i + 1] == 0x3F &&
                basic_head[i + 2] == 0x3E && basic_head[i + 3] == 0x2A) {
                marker = (long)pos + i;
                found = true;
                break;
            }
        }
        if (got < 4 || got - 3 <= 0) break;
        pos += (size_t)(got - 3);   /* overlap 3 bytes so a marker across the edge is found */
    }
    if (found && ti_Seek((int)marker + 4, SEEK_SET, h) != EOF &&
        ti_Read(basic_pix, 1, BASIC_PIXELS, h) == BASIC_PIXELS) {
        int k;
        for (k = 0; k < BASIC_PIXELS; k++) {
            icon_sprite[2 + k] = basic_color(basic_pix[k]);
        }
        icon_sprite[0] = 16;
        icon_sprite[1] = 16;
        icon_transparent = true;
    } else {
        found = false;
    }
    ti_Close(h);
    return found;
}

static bool read_prgm_icon(const item_t *it) {
    uint8_t head[ICON_SCAN];
    uint8_t h;
    int n, at;
    bool ok = false;

    h = ti_OpenVar(it->name, "r", it->type);
    if (!h) return false;
    n = (int)ti_Read(head, 1, ICON_SCAN, h);
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
        return true;
    }
    return read_basic_icon(it);
}

static void draw_item_icon(const item_t *it, int y) {
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
            draw_in_box(dir, dir_width, dir_height, y);
            break;
        case KIND_FILE:
            draw_file_with_overlay(it->overlay, y);
            break;
        case KIND_VAR:
            draw_file_with_overlay(it->overlay, y);
            break;
        default:
            break;
    }
}

static void draw_row(int y, const item_t *it, const char *right, bool sel) {
    uint8_t bg = sel ? C_SELECT : C_WHITE;
    uint8_t fg = sel ? C_WHITE : C_BLACK;
    int ty = y + (ROW_H - 8) / 2;

    gfx_SetColor(bg);
    gfx_FillRectangle(0, y, SCREEN_W, ROW_H);
    draw_item_icon(it, y);
    text_at(TEXT_X, ty, it->name, fg, bg);

    /* Right-side markers, each on its own spot. */
    if (it->kind == KIND_VAR && it->archived) {
        gfx_RLETSprite(archive, ARCHIVE_X, y + (ROW_H - archive_height) / 2);
    }
    if (it->kind == KIND_VAR && it->type == OS_TYPE_PROT_PRGM) {
        gfx_RLETSprite(lock, LOCK_X, y + (ROW_H - lock_height) / 2);
    }
    if (it->kind == KIND_VAR && is_prgm_type(it->type) && read_prgm_icon(it)) {
        if (icon_transparent) {
            gfx_SetTransparentColor(PRGM_TRANSPARENT);
            gfx_TransparentSprite((gfx_sprite_t *)icon_sprite, PRGM_ICON_X, y + (ROW_H - 16) / 2);
        } else {
            gfx_Sprite((gfx_sprite_t *)icon_sprite, PRGM_ICON_X, y + (ROW_H - 16) / 2);
        }
    }
    if (right) text_at(LABEL_X, ty, right, fg, bg);
}

static const char *item_right_label(const item_t *it) {
    switch (it->kind) {
        case KIND_USB:    return msd_ready ? usb_label : "no drive";
        case KIND_PART:   return "PART";
        case KIND_DIR:    return "DIR";
        case KIND_FILE:
        case KIND_VAR:    return overlay_label(it->overlay);
        default:          return NULL;
    }
}

/* ---------- Battery ---------- */

/* boot_GetBatteryStatus() gives a level 0-4. The juice sprite for that level
 * is drawn first, then the frame on top: batt, or battchg while charging.
 * The percentage is the level times 25, so it is approximate. */
static void draw_battery(void) {
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

static void draw(void) {
    char line[48];
    char path[PATH_MAX + 8];
    int i;

    /* Background */
    gfx_FillScreen(C_WHITE);

    /* Title bar: white text on blue */
    gfx_SetColor(C_BLUE);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    text_at(6, 6,
            location == LOC_CALC ? "Calculator" :
            location == LOC_PART ? "Partitions" : "USB Drive",
            C_WHITE, C_BLUE);
    draw_battery();
    keys_draw(SCREEN_W - 18, 1);   /* left of the battery, which is at SCREEN_W - 18 */

    /* Path line and filters: black text on white */
    if (search_mode || search[0] != 0) {
        sprintf(line, "Find: %s%s   (%d shown)", search,
                search_mode ? "_" : "", item_count - 1);
    } else if (location == LOC_CALC) {
        sprintf(line, "Path: /   Store: %s   Type: %s",
                storage_names[store_filter], type_names[type_filter]);
    } else if (location == LOC_PART) {
        sprintf(line, "Pick a partition   (%d found)", part_count);
    } else {
        sprintf(path, "/usb%s",
                strcmp(drive_path, "/") == 0 ? "/" : drive_path);
        sprintf(line, "Path: %s   (%d items)", path, item_count - 1);
    }
    text_at(6, FILTER_Y + 2, line, C_BLACK, C_WHITE);
    {
        uint32_t fsize;
        if (selected_drive_file_size(&fsize)) {
            char sz[24];
            sprintf(sz, "%lu bytes", (unsigned long)fsize);
            text_at(SCREEN_W - 80, FILTER_Y + 2, sz, C_BLACK, C_WHITE);
        }
    }

    /* List */
    if (item_count == 0) {
        text_at(6, LIST_Y + 4, "(empty)", C_BLACK, C_WHITE);
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

    /* Status bar: white text on black */
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle(0, SCREEN_H - STATUS_H, SCREEN_W, STATUS_H);
    if (status_timed) {
        text_at(6, SCREEN_H - STATUS_H + 3, status, C_WHITE, C_BLACK);
    } else {
        draw_key_legend(SCREEN_H - STATUS_H + 3);
    }

    gfx_SwapDraw();
}

/* ---------- Input ---------- */

/* Moves up one level: a folder goes to its parent, a partition root goes to the
 * partition list (or the calculator if there's only one), a partition list goes
 * to the calculator. Does nothing at the calculator root. */
static void go_up_level(void) {
    if (location == LOC_PART) {
        location = LOC_CALC;
        calc_refresh();
        reset_view_position();
    } else if (location == LOC_DRIVE) {
        if (strcmp(drive_path, "/") == 0) {
            if (part_count > 1) {
                location = LOC_PART;
                part_refresh();
            } else {
                location = LOC_CALC;
                calc_refresh();
            }
        } else {
            drive_go_up();
            drive_refresh();
        }
        reset_view_position();
    }
}

/* Alpha + Up/Down: moves one screen of rows (no wrap). */
static void page_selection(int step) {
    if (item_count == 0) return;
    selected += step * VISIBLE_ROWS;
    if (selected < 0) selected = 0;
    if (selected > item_count - 1) selected = item_count - 1;
    clamp_selection();
    needs_redraw = true;
}

/* Up at the top wraps to the bottom; Down at the bottom wraps to the top. */
static void move_selection(int delta) {
    if (item_count == 0) return;

    if (delta < 0 && selected == 0) {
        selected = item_count - 1;
    } else if (delta > 0 && selected == item_count - 1) {
        selected = 0;
    } else {
        selected += delta;
    }
    clamp_selection();
    needs_redraw = true;
}

/* ---------- Text viewer glue ---------- */

/* Builds the drive path to the selected file and opens it in the viewer
 * that matches its extension. */
static void open_file(const item_t *it) {
    char fpath[PATH_MAX + 16];

    if (viewer_can_open(it->name) || image_can_open(it->name)) {
        if (strcmp(drive_path, "/") == 0) {
            sprintf(fpath, "/%s", it->name);
        } else {
            sprintf(fpath, "%s/%s", drive_path, it->name);
        }
    }
    if (viewer_can_open(it->name)) {
        if (!viewer_open(&fat, fpath, it->name)) set_status("Could not open file");
    } else if (image_can_open(it->name)) {
        if (!image_open(&fat, fpath, it->name)) {
            set_status("Could not open image (BMP, 8 or 24-bit, max 640 wide)");
        }
    } else {
        set_status("Only .txt, .html, .bmp files open");
    }
}

/* Enter on the USB row: open the drive, or the partition list if there are several. */
static void enter_drive(void) {
    if (!msd_ready) {
        set_status("No USB drive connected");
        return;
    }
    /* Several partitions: always go through the partition list. */
    if (part_count > 1) {
        location = LOC_PART;
        part_refresh();
        reset_view_position();
        return;
    }
    if (!mounted && !open_partition(0)) return;
    location = LOC_DRIVE;
    strcpy(drive_path, "/");
    drive_refresh();
    reset_view_position();
}

static void activate_selected(void) {
    item_t *it;
    bool navigates;

    if (item_count == 0) return;
    it = &items[selected];

    /* Moving between lists starts fresh, so a search never carries over. */
    navigates = it->kind != KIND_FILE && it->kind != KIND_VAR;
    if (navigates) clear_search();

    switch (it->kind) {
        case KIND_USB:
            enter_drive();
            break;

        case KIND_PART:
            if (open_partition(it->type)) {
                location = LOC_DRIVE;
                strcpy(drive_path, "/");
                drive_refresh();
                reset_view_position();
            }
            break;

        case KIND_PARENT:
            go_up_level();
            break;

        case KIND_DIR:
            strip_trailing_slash(it->name);
            drive_go_down(it->name);
            drive_refresh();
            reset_view_position();
            break;

        case KIND_FILE:
            open_file(it);
            break;

        case KIND_VAR:
            var_info_open(it->name, it->type);
            break;
    }
    needs_redraw = true;
}

/* Adds one typed letter (from alpha or alpha lock) to the search. */
static void type_letter(char c) {
    size_t n = strlen(search);
    if (n < SEARCH_MAX - 1) {
        search[n] = c;
        search[n + 1] = 0;
        reset_view_position();
        refresh_current();
    }
    needs_redraw = true;
}

/* ---------- Palette ---------- */

/* Icon palette, plus the title bar blue in its own slot. The image viewer
 * replaces the whole palette, so this runs again when an image closes. */
static void restore_palette(void) {
    uint16_t title  = gfx_RGBTo1555(TITLE_RGB[0], TITLE_RGB[1], TITLE_RGB[2]);
    uint16_t select = gfx_RGBTo1555(SELECT_RGB[0], SELECT_RGB[1], SELECT_RGB[2]);
    gfx_SetPalette(mypalette, sizeof_mypalette, 0);  /* icon colors */
    gfx_SetPalette(&title,  sizeof(title),  C_BLUE);    /* title bar */
    gfx_SetPalette(&select, sizeof(select), C_SELECT);  /* selected row */
    gfx_SetPalette(ti_pal, sizeof(ti_pal), TI_PAL_BASE); /* TI-BASIC icon colors */
}

/* ---------- Main ---------- */

int main(void) {
    uint8_t key;
    bool quit = false;
    uint8_t last_key = 0;
    bool pressed;
    bool typing_now = false;   /* search was being typed when this key went down */
    bool page_key = false;     /* alpha + Up/Down, on the frame the key went down */
    char letter;
    bool last_ready = false;

    gfx_Begin();
    status_at = (uint32_t)clock();
    status_timed = true;
    {
        int i;
        for (i = 0; i < 16; i++) ti_pal[i] = gfx_palette[basic_palette[i]];  /* before FileR overwrites it */
    }
    restore_palette();
    gfx_SetDrawBuffer();

    battery_poll();
    usb_Init(usb_handler, NULL, NULL, USB_DEFAULT_INIT_FLAGS);
    calc_refresh();

    while (!quit) {
        /* Dispatch pending USB events so connect/enable callbacks run. */
        usb_HandleEvents();

        /* Unplugging while inside the drive or partition list drops back to the root. */
        if (!msd_ready && location != LOC_CALC) {
            location = LOC_CALC;
            strcpy(drive_path, "/");
            clear_search();
            calc_refresh();
            needs_redraw = true;
        }
        /* The viewer reads the drive, so leave it when the drive goes. */
        if (!msd_ready && viewer_is_open()) {
            viewer_close();
            needs_redraw = true;
        }
        if (!msd_ready && image_is_open()) {
            image_close();
            restore_palette();
            needs_redraw = true;
        }
        if (msd_ready != last_ready) {
            last_ready = msd_ready;
            reset_view_position();
            needs_redraw = true;
        }

        key = os_GetCSC();
        if (key == sk_Del) break;   /* Del exits from anywhere */
        /* A key counts once per press. Holding it down doesn't run the modifiers again. */
        pressed = key != 0 && key != last_key;
        last_key = key;
        letter = 0;
        page_key = false;
        if (pressed) {
            typing_now = search_mode;                       /* only y= feeds the search */
            letter = search_mode ? keys_char(key) : 0;
            page_key = keys_alpha_active() && (key == sk_Up || key == sk_Down);   /* before the modifiers change */
            /* With the info panel open, 2nd is its archive toggle, not the shift modifier. */
            if (!(key == sk_2nd && var_info_is_open())) keys_process(key);
            needs_redraw = true;         /* indicators may have changed */
        }

        if (image_is_open()) {
            if (image_key(key)) {
                restore_palette();
                needs_redraw = true;
            }
        } else if (viewer_is_open()) {
            if (viewer_key(key)) needs_redraw = true;
        } else if (var_info_is_open()) {
            if (var_info_key(key)) calc_refresh();
            needs_redraw = true;
        } else if (letter) {
            type_letter(letter);
        } else {
            switch (key) {
                case sk_Up:    if (page_key) page_selection(-1); else move_selection(-1); break;
                case sk_Down:  if (page_key) page_selection(1);  else move_selection(1);  break;
                case sk_Enter: activate_selected(); break;
                case sk_Left:
                    if (location == LOC_CALC) {
                        type_filter = (type_filter + TYPE_COUNT - 1) % TYPE_COUNT;
                        calc_refresh();
                        reset_view_position();
                        needs_redraw = true;
                    }
                    break;
                case sk_Right:
                    if (location == LOC_CALC) {
                        type_filter = (type_filter + 1) % TYPE_COUNT;
                        calc_refresh();
                        reset_view_position();
                        needs_redraw = true;
                    }
                    break;
                case sk_Clear:
                    if (typing_now) {
                        clear_search();   /* while typing a search, Clear empties it */
                        refresh_current();
                        reset_view_position();
                        needs_redraw = true;
                    } else if (location == LOC_CALC) {
                        quit = true;      /* at the calculator root, Clear exits */
                    } else {
                        clear_search();
                        go_up_level();
                        needs_redraw = true;
                    }
                    break;
                case sk_Yequ:
                    search_mode = !search_mode;
                    set_status(search_mode ? "y=: search on, y= to stop" : "y=: search off");
                    break;
                case sk_Window:
                    set_status("Window: unassigned");
                    break;
                case sk_Zoom:
                    set_status("Zoom: unassigned");
                    break;
                case sk_Trace:
                    set_status("Trace: unassigned");
                    break;
                case sk_Graph:
                    set_status("Graph: store filter");
                    if (location == LOC_CALC) {
                        store_filter = (store_filter + 1) % STORE_COUNT;
                        calc_refresh();
                        reset_view_position();
                        needs_redraw = true;
                    }
                    break;
                default:
                    break;
            }
        }

        if (status_timed && (uint32_t)clock() - status_at >= (uint32_t)STATUS_SECONDS * CLOCKS_PER_SEC) {
            status[0] = 0;
            status_timed = false;
            status_changed = true;
        }
        if (status_changed) {
            status_changed = false;
            needs_redraw = true;
        }
        if (needs_redraw) {
            if (viewer_is_open()) viewer_draw();
            else if (!image_is_open()) draw();   /* image is drawn once, on open */
            needs_redraw = false;
        }
    }

    fat_close_volume();
    if (msd_open) msd_Close(&msd);
    usb_Cleanup();
    gfx_End();
    return 0;
}
