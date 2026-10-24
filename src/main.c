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
#include <keypadc.h>      /* kb_IsDown: tests 2nd + Mode directly */
#include <sys/power.h>   /* boot_GetBatteryStatus, boot_BatteryCharging */
#include "icon/gfx.h"   /* convimg output: all icon sprites + palette */
#include "text_viewer.h"    /* text viewer for .txt files */
#include "image_viewer.h"   /* image viewer for .bmp files */
#include "var_info.h"     /* info panel for calculator variables */
#include "keys.h"         /* 2nd and alpha modifiers */
#include "lfn_fix.h"      /* removes long-name leftovers after a delete */
#include <time.h>          /* clock(): status message timeouts */
#include <stdlib.h>        /* malloc/free: folder listings during a folder copy */

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


/* ---------- Theme ----------
 * Each themed UI role has its own palette slot, so changing it never touches
 * the icon colors. Each role picks one of the presets below in the settings
 * menu (Mode). The choice is saved in the AppVar FILERCFG. */
#define T_BG     0x40   /* screen background */
#define T_TOP    0x41   /* title bar */
#define T_SEL    0x42   /* selected row and popup selection */
#define T_BOT    0x43   /* bottom (status) bar */
#define T_TXT    0x44   /* text on the background */
#define T_BOTTXT 0x45   /* text on the bottom bar */
#define T_TOPTXT 0x46   /* text on the title bar */
#define T_OUT    0x47   /* outlines of popup menus and the variable panel */
#define T_MARK   0x48   /* name of the file marked with Trace */
#define T_SELTXT 0x49   /* text on the selected row */
#define THEME_ROLES   10
#define THEME_PRESETS 256   /* every entry of the calculator's palette, 0-255 */

/* Starting colors, 8-bit RGB. At startup each one is matched to the nearest
 * palette entry (see theme_find_defaults). */
static const uint8_t THEME_DEFAULT_RGB[THEME_ROLES][3] = {
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
static uint16_t palette256[THEME_PRESETS];   /* the calculator's palette at startup, RGB1555 */
static const char *const THEME_ROLE_NAME[THEME_ROLES] = {
    "Background", "Top bar", "Selector", "Bottom bar", "Text", "Bottom text", "Top text", "Popup outline", "Marked file", "Selected text",
};
static const uint8_t THEME_ROLE_SLOT[THEME_ROLES] = { T_BG, T_TOP, T_SEL, T_BOT, T_TXT, T_BOTTXT, T_TOPTXT, T_OUT, T_MARK, T_SELTXT };
static uint8_t theme_default[THEME_ROLES];   /* palette index per role, found at startup */
static uint8_t theme_idx[THEME_ROLES];     /* live preset per role (may hold unsaved changes) */
static uint8_t theme_saved[THEME_ROLES];   /* preset per role as last saved */
static bool settings_open = false;
static uint8_t settings_sel = 0;           /* role the settings cursor is on */
static uint8_t settings_num = 0;           /* value being typed with the digit keys */
static bool settings_typing = false;       /* digits typed since the last move or change */
static void draw_settings(void);           /* defined with the settings code */
static void make_path(char *out, const char *folder, const char *name);   /* defined with the path helpers */
static void restore_palette(void);         /* defined with the palette code */

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
    uint8_t contents;   /* folders only: 0 empty, 1 holds a file, 2 holds folders only */
} item_t;


/* ---------- State ---------- */

static msd_t msd;
static fat_t fat;
static bool msd_open = false;     /* msd_Open succeeded */
static bool msd_ready = false;    /* device open and partitions found */
static bool mounted = false;      /* FAT volume open on one partition */
static bool fat_open = false;     /* fat_Open succeeded and fat_Close not yet called */
static bool write_ok = false;      /* true only while a file operation writes to the drive */
static uint8_t copy_buf[FAT_BLOCK_SIZE];   /* one block, used while copying a file */
static char mark_path[PATH_MAX + 16];      /* file marked with Trace: full path */
static char mark_name[16];                 /* its name */
static bool mark_set = false;
static bool mark_is_dir = false;           /* the marked item is a folder */
static bool copy_cancelled = false;        /* Clear stopped a folder copy */
static bool rename_mode = false;           /* typing a new name for the selected file */
static char rename_buf[13] = "";           /* the name typed so far */
static char rename_src[PATH_MAX + 16];     /* full path of the file being renamed */
static int cur_part = 0;                   /* partition index currently open */
static bool pending_delete = false;        /* a delete is waiting for Y= (yes) or Graph (no) */
static bool pending_calc = false;          /* the waiting delete is a calculator variable */
static char pending_var[9];                /* its name */
static uint8_t pending_var_type;           /* its OS type */
static bool pending_is_dir = false;        /* what that delete would remove */
static char pending_path[PATH_MAX + 16];   /* what that Zoom would delete */
static lfn_list_t lfn_list;                /* long-name entries of the file being deleted */
static bool lfn_pending = false;           /* a file was deleted; lfn_list must be cleared */
static char add_long[256];                 /* long name to give a copy, read from its source */
static char add_short[13];                 /* the copy's 8.3 name */
static char add_dir[PATH_MAX];             /* folder the copy went into */
static bool add_pending = false;           /* a copy needs its long name written */

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

static uint8_t store_filter = STORE_ALL;   /* calculator filters, set in the Graph popup */
static uint8_t type_filter = TYPE_ALL;
static int selected = 0;
static int top = 0;
static bool needs_redraw = true;

static char search[SEARCH_MAX] = "";
static bool search_mode = false;   /* y= : letters type into the search */

#define STATUS_MS 2500             /* how long a status message stays, in milliseconds */
static uint32_t status_at = 0;     /* clock() value when the message was set */
static bool status_timed = false;  /* message clears after STATUS_SECONDS */


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

/* Writes reach the drive only while a file operation runs (write_ok). Any
 * other write the FAT driver makes (e.g. the dirty bit on open) is discarded. */
static uint32_t writes_blocked = 0;   /* diagnostic: write requests dropped outside file operations */
static uint32_t writes_done = 0;      /* diagnostic: write requests passed to the drive */

static uint24_t write_cb(void *usr, uint32_t lba, uint24_t count, const void *buf) {
    (void)usr;
    if (!write_ok) {
        writes_blocked++;
        return count;
    }
    writes_done++;
    return msd_Write(&msd, lba, count, buf);
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
        if (fat_Open(&fat, read_cb, write_cb, NULL, part_lba[i]) != FAT_SUCCESS) continue;
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
    ferr = fat_Open(&fat, read_cb, write_cb, NULL, part_lba[i]);
    if (ferr != FAT_SUCCESS) {
        sprintf(buf, "Drive: FAT open failed (%d)", (int)ferr);
        usb_fail(buf);
        return false;
    }
    fat_open = true;
    mounted = true;
    cur_part = i;
    mark_set = false;   /* a mark from another partition would point at the wrong drive */
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
    mark_set = false;
    rename_mode = false;
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

/* ---------- Filters ----------
 * Calculator: Store and Type. USB drive: Show (all, folders or files) and Ext
 * (any, TXT or BMP). All are set in the Graph popup. */
static uint8_t usb_kind = 0;    /* 0 all, 1 folders only, 2 files only */
static uint8_t usb_ext = 0;     /* 0 any, 1 .TXT, 2 .HTML, 3 .HTM, 4 .BMP */
static const char *const store_opts[] = { "All", "Archive", "RAM" };
static const char *const type_opts[]  = { "All", "AppVar", "Prgm", "Var" };
static const char *const kind_opts[]  = { "All", "Folders", "Files" };
static const char *const ext_opts[]   = { "Any", "TXT", "HTML", "HTM", "BMP" };
#define EXT_COUNT 5
static const char *const ext_want[]   = { NULL, "TXT", "HTML", "HTM", "BMP" };

typedef struct {
    const char *label;
    const char *const *opts;
    uint8_t count;
    uint8_t *val;
} filter_row_t;

/* Fills rows with the filters for the list on screen and returns how many. */
static uint8_t filter_rows_build(filter_row_t *rows) {
    if (location == LOC_CALC) {
        rows[0] = (filter_row_t){ "Store", store_opts, STORE_COUNT, &store_filter };
        rows[1] = (filter_row_t){ "Type", type_opts, TYPE_COUNT, &type_filter };
    } else {
        rows[0] = (filter_row_t){ "Show", kind_opts, 3, &usb_kind };
        rows[1] = (filter_row_t){ "Ext", ext_opts, EXT_COUNT, &usb_ext };
    }
    return 2;
}

/* Called when moving to another folder or drive: the USB filters go back to All. */
static void reset_usb_filters(void) {
    usb_kind = 0;
    usb_ext = 0;
}

/* True if a drive item passes the USB filters. Folders stay visible unless only
 * files are wanted, so you can still get into them. */
static bool usb_matches(enum kind k, const char *name) {
    const char *dot;
    const char *want;
    size_t len, i;

    if (k == KIND_DIR) return usb_kind != 2;
    if (usb_kind == 1) return false;
    if (usb_ext == 0) return true;
    dot = strrchr(name, '.');
    if (!dot) return false;
    want = ext_want[usb_ext];
    len = strlen(want);
    for (i = 0; i < len; i++) {
        if (dot[1 + i] == 0 || toupper((unsigned char)dot[1 + i]) != want[i]) return false;
    }
    return dot[1 + len] == 0;
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
/* What a folder holds, for its icon: 0 nothing, 1 at least one file, 2 folders only. */
static uint8_t folder_contents(const char *path) {
    fat_dir_t d;
    fat_dir_entry_t e;
    bool any_file = false, any_dir = false;

    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return 0;
    while (fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
        if (e.name[0] == '.' || (e.attrib & (FAT_HIDDEN | FAT_VOLLABEL))) continue;
        if (e.attrib & FAT_DIR) any_dir = true;
        else any_file = true;
        if (any_file) break;
    }
    fat_CloseDir(&d);
    if (any_file) return 1;
    return any_dir ? 2 : 0;
}

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
        if (!usb_matches(it->kind, it->name)) continue;
        it->type = 0;
        it->archived = false;
        item_count++;
    }
    fat_CloseDir(&fdir);

    /* Folder icons depend on what is inside. Checked after the listing is closed,
     * so only one directory is open at a time. */
    {
        int i;
        for (i = 1; i < item_count; i++) {
            char nm[16], sub[PATH_MAX + 16];
            size_t nl;
            if (items[i].kind != KIND_DIR) continue;
            nl = strlen(items[i].name);
            if (nl > 0 && items[i].name[nl - 1] == '/') nl--;   /* folder names end in "/" */
            memcpy(nm, items[i].name, nl);
            nm[nl] = 0;
            make_path(sub, drive_path, nm);
            items[i].contents = folder_contents(sub);
        }
    }
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

/* Size of the selected item in bytes: a calculator variable (its VAT data
 * size), or a USB file. Returns false when nothing is selected or sized. */
static bool selected_size(uint32_t *size) {
    item_t *it;
    uint8_t h;

    if (item_count == 0) return false;
    it = &items[selected];
    if (location == LOC_CALC && it->kind == KIND_VAR) {
        h = ti_OpenVar(it->name, "r", it->type);
        if (!h) return false;
        *size = (uint32_t)ti_GetSize(h);
        ti_Close(h);
        return true;
    }
    return selected_drive_file_size(size);
}

/* Writes a byte count in the largest unit that fits: bytes, KB or MB, with
 * one decimal place (for example "1.5 KB"). Integer math only. */
static void format_size(uint32_t n, char *out) {
    if (n < 1024UL) {
        sprintf(out, "%lu bytes", (unsigned long)n);
    } else if (n < 1024UL * 1024UL) {
        sprintf(out, "%lu.%lu KB",
                (unsigned long)(n / 1024UL),
                (unsigned long)(((n % 1024UL) * 10UL) / 1024UL));
    } else {
        sprintf(out, "%lu.%lu MB",
                (unsigned long)(n / (1024UL * 1024UL)),
                (unsigned long)(((n % (1024UL * 1024UL)) * 10UL) / (1024UL * 1024UL)));
    }
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

/* Shortens a path for the path line. Up to 4 levels show in full. Deeper paths
 * show the first two levels, "...", then the last two: /a/b/.../d/e */
static void shorten_path(char *out, size_t outsz, const char *full) {
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
        snprintf(out, outsz, "/");
    } else if (n <= 4) {
        int i;
        out[0] = 0;
        for (i = 0; i < n; i++) {
            strncat(out, "/", outsz - strlen(out) - 1);
            strncat(out, part[i], outsz - strlen(out) - 1);
        }
    } else {
        snprintf(out, outsz, "/%s/%s/.../%s/%s", part[0], part[1], part[n - 2], part[n - 1]);
    }
}

/* Like text_at, but cut short with "..." so it fits in maxw pixels. Long names
 * and paths would otherwise wrap onto the next line. */
static void text_clip(int x, int y, const char *s, uint8_t fg, uint8_t bg, int maxw) {
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
enum menu_kind { MENU_NONE, MENU_FILES, MENU_ACTIONS };
static const char *files_items[2]   = { "New folder", "Delete" };
static const char *actions_items[3] = { "Rename", "Move here", "Copy here" };
static bool menu_open = false;
static enum menu_kind menu_kind = MENU_NONE;
static int menu_sel = 0;

static int menu_count(void) {
    return menu_kind == MENU_FILES ? 2 : 3;
}

static const char *menu_item(int i) {
    return menu_kind == MENU_FILES ? files_items[i] : actions_items[i];
}

/* Draws the popup over the list. */
/* ---------- Filter popup (Graph) ----------
 * Rows are the filters of the list on screen. Enter opens a row's dropdown,
 * Up/Down picks an option, Enter applies it. Clear (or Mode/Graph) backs out
 * one level at a time. */
static bool filter_open = false;
static uint8_t filter_sel = 0;         /* row the cursor is on */
static bool filter_drop = false;       /* that row's dropdown is open */
static uint8_t filter_drop_sel = 0;   /* option highlighted in the dropdown */

static void filters_changed(void) {
    reset_view_position();
    if (location == LOC_CALC) calc_refresh();
    else drive_refresh();
    needs_redraw = true;
}

static void filter_key(uint8_t key) {
    filter_row_t rows[2];
    uint8_t n = filter_rows_build(rows);
    uint8_t cnt = rows[filter_sel].count;

    if (filter_drop) {
        if (key == sk_Up) {
            filter_drop_sel = (uint8_t)((filter_drop_sel + cnt - 1) % cnt);
        } else if (key == sk_Down) {
            filter_drop_sel = (uint8_t)((filter_drop_sel + 1) % cnt);
        } else if (key == sk_Enter) {
            *rows[filter_sel].val = filter_drop_sel;
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
            filter_drop = true;
            filter_drop_sel = *rows[filter_sel].val;
        } else if (key == sk_Clear || key == sk_Mode || key == sk_Graph) {
            filter_open = false;
        }
    }
    needs_redraw = true;
}

static void draw_filter(void) {
    filter_row_t rows[2];
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
        const char *hint = filter_drop ? "Up/Down, then Enter: set" : "Enter: change";
        text_at(x + w - 8 - (int)gfx_GetStringWidth(hint), y + 4, hint, T_TOPTXT, T_TOP);
    }
    for (i = 0; i < n; i++) {
        int ry = y + 20 + i * 18;
        int sel = i == filter_sel;
        char val[24];

        sprintf(val, "%s  v", rows[i].opts[*rows[i].val]);
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

static void draw_menu(void) {
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

static void draw_key_legend(int y) {
    static const char *calc_labels[5] = { "Search", "Info", "", "", "Filter" };
    static const char *usb_labels[5]  = { "Search", "Files", "Actions", "Mark", "Filter" };
    const char *labels[5];
    int i, cx, w;

    /* Only the keys that do something on the current list are labeled. */
    for (i = 0; i < 5; i++) {
        if (location == LOC_DRIVE) labels[i] = usb_labels[i];
        else if (location == LOC_CALC) labels[i] = calc_labels[i];
        else labels[i] = i == 0 ? "Search" : "";
    }
    if ((location == LOC_DRIVE || location == LOC_CALC) && pending_delete) {   /* waiting for an answer */
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
        default:
            break;
    }
}

/* True if this row is the file marked with Trace (USB list only). */
static bool row_is_marked(const item_t *it) {
    char p[PATH_MAX + 16];

    char n[16];

    if (!mark_set || location != LOC_DRIVE) return false;
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) return false;
    strncpy(n, it->name, sizeof(n) - 1);            /* folder names carry a trailing "/" */
    n[sizeof(n) - 1] = 0;
    strip_trailing_slash(n);
    if (strcmp(drive_path, "/") == 0) {
        sprintf(p, "/%s", n);
    } else {
        sprintf(p, "%s/%s", drive_path, n);
    }
    return strcmp(p, mark_path) == 0;
}

static void draw_row(int y, const item_t *it, const char *right, bool sel) {
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
        text_clip(TEXT_X, ty, it->name, row_is_marked(it) ? T_MARK : fg, bg, edge - TEXT_X - 4);
    }

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
/* Battery, 2nd, alpha and alpha lock icons at the top right. Also drawn over
 * the text viewer (see viewer_set_overlay). */
static void draw_indicators(void);

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

static void draw_indicators(void) {
    draw_battery();
    keys_draw(SCREEN_W - 18, 1);   /* left of the battery, which is at SCREEN_W - 18 */
}

/* Drawn over the text viewer: the icons, and the color menu when it is open. */
static void draw_viewer_overlay(void) {
    draw_indicators();
    if (settings_open) draw_settings();
}

static void draw(void) {
    char line[64];
    char path[PATH_MAX + 8];
    int i;

    /* Background */
    gfx_FillScreen(T_BG);

    /* Title bar: top-bar text on top-bar color */
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(0, 0, SCREEN_W, TITLE_H);
    text_at(6, 6,
            location == LOC_CALC ? "Calculator" :
            location == LOC_PART ? "Partitions" : "USB Drive",
            T_TOPTXT, T_TOP);
    draw_indicators();

    /* Path line and filters: black text on white */
    if (rename_mode) {
        sprintf(line, "Rename: %s_   Enter save, Clear cancel", rename_buf);
    } else if (search_mode || search[0] != 0) {
        sprintf(line, "Find: %s%s   (%d shown)", search,
                search_mode ? "_" : "", item_count - 1);
    } else if (location == LOC_CALC) {
        sprintf(line, "Path: /%s",
                (store_filter || type_filter) ? "   filtered" : "");
    } else if (location == LOC_PART) {
        sprintf(line, "Pick a partition   (%d found)", part_count);
    } else {
        char shown[64];
        sprintf(path, "/usb%s",
                strcmp(drive_path, "/") == 0 ? "/" : drive_path);
        shorten_path(shown, sizeof(shown), path);
        sprintf(line, "Path: %s   (%d items)%s", shown, item_count - 1,
                (usb_kind || usb_ext) ? "   filtered" : "");
    }
    text_clip(6, FILTER_Y + 2, line, T_TXT, T_BG, SCREEN_W - 12);
    {
        uint32_t fsize;
        if (selected_size(&fsize)) {
            char sz[24];
            format_size(fsize, sz);
            /* Selected file or variable size goes in the title bar, after the title. */
            text_at(100, 6, sz, T_TOPTXT, T_TOP);
        }
    }

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

    gfx_SwapDraw();
}

/* ---------- USB file operations ---------- */

/* Builds folder/name. folder is "/" or a folder path without a trailing slash.
 * The parameter is not called `dir`, because `dir` is a sprite macro. */
static void make_path(char *out, const char *folder, const char *name) {
    if (strcmp(folder, "/") == 0) {
        sprintf(out, "/%s", name);
    } else {
        sprintf(out, "%s/%s", folder, name);
    }
}

/* Closes the volume with writes allowed, so the driver can save its closing
 * changes (clearing the dirty bit, updating free-space counts), then opens it
 * again. Called after every file operation. Writes made during browsing are
 * still discarded. */
static void finish_writes(void) {
    if (!mounted) return;
    write_ok = true;
    fat_close_volume();
    write_ok = false;
    if (fat_Open(&fat, read_cb, write_cb, NULL, part_lba[cur_part]) == FAT_SUCCESS) {
        fat_open = true;
    } else {
        mounted = false;
        set_status("Drive: reopen failed, replug");
    }
}

/* Makes NEWDIR in the current folder, or NEWDIR2, NEWDIR3... if that name is taken. */
static void new_folder(void) {
    char name[16] = "";
    char msg[40];
    fat_error_t err = FAT_ERROR_RW_FAILED;
    int n;

    if (!mounted) return;
    write_ok = true;
    for (n = 1; n <= 99 && err != FAT_SUCCESS; n++) {
        if (n == 1) strcpy(name, "NEWDIR");
        else sprintf(name, "NEWDIR%d", n);
        err = fat_Create(&fat, drive_path, name, FAT_DIR);
    }
    write_ok = false;
    finish_writes();

    if (err == FAT_SUCCESS) {
        sprintf(msg, "Made folder %s", name);
        set_status(msg);
        drive_refresh();
    } else {
        set_status("New folder failed");
    }
}

/* Marks the selected file (Trace). Zoom then moves it into whatever folder is open. */
static void mark_selected(void) {
    item_t *it;

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Mark: select a file or folder");
        return;
    }
    if (row_is_marked(it)) {          /* Trace on the marked item takes the mark off */
        mark_set = false;
        set_status("Unmarked");
        return;
    }
    strncpy(mark_name, it->name, sizeof(mark_name) - 1);   /* folder names carry a trailing "/" */
    mark_name[sizeof(mark_name) - 1] = 0;
    strip_trailing_slash(mark_name);
    make_path(mark_path, drive_path, mark_name);
    mark_is_dir = it->kind == KIND_DIR;
    mark_set = true;
    set_status(mark_is_dir ? "Marked folder. Zoom moves it here" : "Marked. Zoom moves it here");
}

static fat_error_t remove_entry(const char *path);   /* defined with the delete code */

/* Copies src_path to dst_dir/name, then deletes src_path. The original is only
 * deleted after the whole copy has been written, and a failed copy is removed.
 * Returns true if the file is now in the new place. */
static bool move_file(const char *src_path, const char *dst_dir, const char *name, bool remove_src) {
    char dst_path[PATH_MAX + 16];
    fat_file_t in, out;
    uint32_t left;
    bool ok = false;
    bool has_long;
    bool cancelled = false;

    make_path(dst_path, dst_dir, name);
    if (fat_OpenFile(&fat, dst_path, 0, &in) == FAT_SUCCESS) {   /* name already used */
        fat_CloseFile(&in);
        set_status("Move: name already used here");
        return false;
    }
    if (fat_OpenFile(&fat, src_path, 0, &in) != FAT_SUCCESS) {
        set_status("Move: cannot read file");
        return false;
    }
    left = fat_GetFileSize(&in);
    has_long = lfn_long_name(&msd, part_lba[cur_part], src_path, add_long, sizeof(add_long));

    write_ok = true;
    if (fat_Create(&fat, dst_dir, name, 0) == FAT_SUCCESS) {
        if (fat_OpenFile(&fat, dst_path, 0, &out) == FAT_SUCCESS) {
            ok = fat_SetFileSize(&out, left) == FAT_SUCCESS;
            while (ok && left > 0) {
                if (os_GetCSC() == sk_Clear) {      /* Clear stops a long copy */
                    cancelled = true;
                    ok = false;
                    break;
                }
                if (fat_ReadFile(&in, 1, copy_buf) != 1 ||
                    fat_WriteFile(&out, 1, copy_buf) != 1) {
                    ok = false;
                } else {
                    left = left > FAT_BLOCK_SIZE ? left - FAT_BLOCK_SIZE : 0;
                }
            }
            fat_CloseFile(&out);
        }
    }
    fat_CloseFile(&in);

    if (!ok) {
        /* Copy failed: remove whatever was written; the original is untouched. */
        fat_Delete(&fat, dst_path);
        write_ok = false;
        set_status(cancelled ? "Cancelled, original kept" : "Move failed, original kept");
        return false;
    }
    if (has_long) {                     /* the copy gets the long name once the volume is flushed */
        add_pending = true;
        strncpy(add_short, name, sizeof(add_short) - 1);
        add_short[sizeof(add_short) - 1] = 0;
        strncpy(add_dir, dst_dir, sizeof(add_dir) - 1);
        add_dir[sizeof(add_dir) - 1] = 0;
    }
    if (!remove_src) {                  /* copy only: the original stays */
        write_ok = false;
        set_status("Copied");
        return true;
    }
    /* The source's handles are closed by now, so remove_entry can flush safely. */
    if (remove_entry(src_path) != FAT_SUCCESS) {
        write_ok = false;
        set_status("Copied, original not deleted");
        return true;
    }
    write_ok = false;
    set_status("Moved");
    return true;
}

/* Removes the long-name leftovers of the file just deleted. Call after
 * finish_writes, so the driver's cache can't overwrite the change. */
static void flush_lfn(void) {
    if (!lfn_pending) return;
    lfn_pending = false;
    if (mounted) lfn_mark_deleted(&msd, &lfn_list);
}

/* Gives the copy its long name, after the volume has been flushed. */
static void flush_add(void) {
    if (!add_pending) return;
    add_pending = false;
    if (mounted) lfn_add_long(&msd, part_lba[cur_part], add_dir, add_short, add_long);
}

/* True if the short entry for path is still on the drive. The driver can
 * remove the entry and still report an error, so this is checked directly
 * instead of trusting the delete's return value. */
static bool still_on_drive(const char *path) {
    lfn_list_t tmp;
    return lfn_find(&msd, part_lba[cur_part], path, &tmp);
}

/* Runs move_file, then flushes the drive. Returns move_file's result. */
static bool move_and_flush(const char *src, const char *dst_dir, const char *name) {
    bool moved = move_file(src, dst_dir, name, true);
    finish_writes();
    flush_lfn();
    flush_add();
    return moved;
}

/* True if the folder at path exists. */
static bool path_exists(const char *path) {
    fat_dir_t d;
    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return false;
    fat_CloseDir(&d);
    return true;
}

/* True if dst is src itself or inside it. A folder can't go into itself. */
static bool inside_folder(const char *dst, const char *src) {
    size_t n = strlen(src);
    return strncmp(dst, src, n) == 0 && (dst[n] == '/' || dst[n] == 0);
}

/* A folder's entries, copied out so the folder can be written while it is
 * being copied (each write flushes the drive and would invalidate an open
 * directory). */
typedef struct {
    char name[13];
    bool is_dir;
} kid_t;

/* Fills *out with the entries of the folder at path. Returns the count, or -1. */
static int list_dir(const char *path, kid_t **out) {
    fat_dir_t d;
    fat_dir_entry_t e;
    kid_t *kids;
    int n = 0, i = 0;

    *out = NULL;
    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return -1;
    while (fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
        if (e.name[0] == '.' || (e.attrib & FAT_VOLLABEL)) continue;
        n++;
    }
    fat_CloseDir(&d);
    if (n == 0) return 0;

    kids = malloc(sizeof(kid_t) * (size_t)n);
    if (!kids) return -1;
    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) {
        free(kids);
        return -1;
    }
    while (i < n && fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
        if (e.name[0] == '.' || (e.attrib & FAT_VOLLABEL)) continue;
        strncpy(kids[i].name, e.name, sizeof(kids[i].name) - 1);
        kids[i].name[sizeof(kids[i].name) - 1] = 0;
        kids[i].is_dir = (e.attrib & FAT_DIR) != 0;
        i++;
    }
    fat_CloseDir(&d);
    *out = kids;
    return i;
}

/* Copies the folder src_path into dst_dir as name, with everything in it.
 * Files are copied one at a time and flushed as they go. Clear or any failure
 * stops the copy and leaves what was already written; the original is never
 * changed. */
static bool copy_folder(const char *src_path, const char *dst_dir, const char *name) {
    char dst_path[PATH_MAX + 16];
    char child_src[PATH_MAX + 16];
    char lname[256];
    kid_t *kids = NULL;
    fat_error_t err;
    int n, i;
    bool ok = true;

    n = list_dir(src_path, &kids);
    if (n < 0) return false;

    write_ok = true;
    err = fat_Create(&fat, dst_dir, name, FAT_DIR);
    write_ok = false;
    finish_writes();                    /* flush the new folder before editing it raw */
    if (err != FAT_SUCCESS) {
        free(kids);
        return false;
    }

    /* Long name for the new folder, if the original had one. */
    if (lfn_long_name(&msd, part_lba[cur_part], src_path, lname, sizeof(lname))) {
        lfn_add_long(&msd, part_lba[cur_part], dst_dir, name, lname);
        finish_writes();
    }

    make_path(dst_path, dst_dir, name);
    for (i = 0; ok && i < n; i++) {
        if (os_GetCSC() == sk_Clear) {
            copy_cancelled = true;
            ok = false;
            break;
        }
        make_path(child_src, src_path, kids[i].name);
        if (kids[i].is_dir) {
            ok = copy_folder(child_src, dst_path, kids[i].name);
        } else {
            ok = move_file(child_src, dst_path, kids[i].name, false);
            finish_writes();
            flush_add();
        }
    }
    free(kids);
    return ok;
}

/* Moves the folder src_path into dst_dir by changing its directory entries
 * only, so no data is copied and it is quick however big the folder is. */
static void move_folder(const char *src_path, const char *dst_dir) {
    bool ok;

    if (inside_folder(dst_dir, src_path)) {
        set_status("Move: can't put a folder inside itself");
        return;
    }
    ok = lfn_move_entry(&msd, part_lba[cur_part], src_path, dst_dir);
    finish_writes();
    if (ok) {
        mark_set = false;               /* the marked path no longer exists */
        set_status("Moved folder");
    } else {
        set_status("Move failed: name used there?");
    }
}

/* Moves the marked file or folder into the open folder (Zoom). */
static void drop_marked(void) {
    if (!mark_set || !mounted) return;
    if (mark_is_dir) {
        move_folder(mark_path, drive_path);
        drive_refresh();
        return;
    }
    if (move_and_flush(mark_path, drive_path, mark_name)) {
        mark_set = false;
        drive_refresh();
    }
}

/* Copies the marked file or folder into the open folder (Zoom menu, Copy here).
 * The original stays. */
static void copy_marked(void) {
    char dst[PATH_MAX + 16];

    if (!mark_set || !mounted) return;
    if (mark_is_dir) {
        make_path(dst, drive_path, mark_name);
        if (inside_folder(drive_path, mark_path)) {
            set_status("Copy: can't put a folder inside itself");
        } else if (path_exists(dst)) {
            set_status("Copy: name already used here");
        } else {
            copy_cancelled = false;
            if (copy_folder(mark_path, drive_path, mark_name)) {
                set_status("Copied folder");
            } else {
                set_status(copy_cancelled ? "Cancelled, copy partly done"
                                          : "Copy failed, copy partly done");
            }
            finish_writes();
        }
        drive_refresh();
        return;
    }
    move_file(mark_path, drive_path, mark_name, false);
    finish_writes();
    flush_add();
    drive_refresh();
}

/* ---------- Delete ---------- */

/* True if the folder at path holds nothing apart from . and .. */
static bool dir_is_empty(const char *path) {
    fat_dir_t d;
    fat_dir_entry_t e;
    bool empty = true;

    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return false;   /* unknown: not empty */
    while (fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
        if (e.name[0] == '.' || (e.attrib & FAT_VOLLABEL)) continue;
        empty = false;
        break;
    }
    fat_CloseDir(&d);
    return empty;
}

/* Deletes one file or folder and removes its long-name leftovers. The leftovers
 * have to go in the same step: the driver counts them as entries, so a folder
 * that still has them can't be deleted. The volume is flushed before the
 * leftovers are marked, so the driver can't overwrite the change. Returns
 * FAT_SUCCESS if the entry is gone, even if the driver reported an error. */
static fat_error_t remove_entry(const char *path) {
    fat_error_t err;
    bool gone;

    lfn_find(&msd, part_lba[cur_part], path, &lfn_list);
    err = fat_Delete(&fat, path);
    finish_writes();
    write_ok = true;                    /* finish_writes clears it; the caller still needs it */
    gone = !still_on_drive(path);
    lfn_pending = gone;
    flush_lfn();
    if (gone) return FAT_SUCCESS;
    return err == FAT_SUCCESS ? FAT_ERROR_RW_FAILED : err;
}

/* Deletes everything inside the folder at path, then the folder. Each item is
 * removed on its own and the folder is re-read after every removal, so it is
 * never changed while it is being read. The guard stops a loop if the driver
 * reports success without really removing anything. */
static bool delete_tree(const char *path) {
    fat_dir_t d;
    fat_dir_entry_t e;
    char child[PATH_MAX + 16];
    bool found;
    int guard = 0;

    for (;;) {
        if (++guard > 4096) return false;
        found = false;
        if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return false;
        while (fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
            if (e.name[0] == '.' || (e.attrib & FAT_VOLLABEL)) continue;
            found = true;
            break;
        }
        fat_CloseDir(&d);
        if (!found) break;

        make_path(child, path, e.name);
        if (e.attrib & FAT_DIR) {
            if (!delete_tree(child)) return false;
        } else if (remove_entry(child) != FAT_SUCCESS) {
            return false;
        }
    }
    return remove_entry(path) == FAT_SUCCESS;
}

/* Deletes a file, or a folder with everything in it. */
static void perform_delete(const char *path, bool is_dir) {
    size_t n = strlen(path);
    fat_error_t code = FAT_SUCCESS;
    char msg[40];
    bool ok;

    if (!is_dir && mounted) lfn_find(&msd, part_lba[cur_part], path, &lfn_list);
    write_ok = true;
    if (is_dir) {
        ok = delete_tree(path);
    } else {
        code = remove_entry(path);
        ok = code == FAT_SUCCESS;
    }
    write_ok = false;
    finish_writes();

    /* The mark is gone if the marked file was the one deleted or was inside it. */
    if (mark_set && strncmp(mark_path, path, n) == 0 &&
        (mark_path[n] == '/' || mark_path[n] == 0)) {
        mark_set = false;
    }
    if (ok) {
        set_status("Deleted");
    } else if (is_dir) {
        set_status("Delete failed, may be partly done");
    } else {
        sprintf(msg, "Delete failed (FAT error %d)", (int)code);
        set_status(msg);
    }
    drive_refresh();
}

/* Zoom on the USB list with nothing marked. An empty folder is deleted at once.
 * A file, or a folder with contents, needs a second Zoom to confirm. */
static void zoom_delete(void) {
    item_t *it;
    char name[16];
    char path[PATH_MAX + 16];
    char msg[40];
    bool is_dir;

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Delete: select a file or folder");
        return;
    }
    is_dir = it->kind == KIND_DIR;
    strncpy(name, it->name, sizeof(name) - 1);   /* folder names carry a trailing "/" */
    name[sizeof(name) - 1] = 0;
    strip_trailing_slash(name);
    make_path(path, drive_path, name);

    if (is_dir && dir_is_empty(path)) {
        perform_delete(path, true);
        return;
    }
    strcpy(pending_path, path);
    pending_is_dir = is_dir;
    pending_delete = true;
    sprintf(msg, "Delete %s?", name);
    set_status(msg);
    status_timed = false;      /* stays until Y= or Graph answers it */
}

/* Del on a calculator variable (root of the calculator list). Asks first, same
 * as a drive delete. The USB row and folders aren't variables, so they're refused. */
static void calc_delete_prompt(void) {
    item_t *it;
    char msg[40];

    if (location != LOC_CALC || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_VAR) {
        set_status("Delete: select a variable");
        return;
    }
    /* FileR can't delete its own program. Deleting FILERCFG is allowed: it resets the colors. */
    if (strcmp(it->name, "FILER") == 0) {
        set_status("Delete: that's FileR itself");
        return;
    }
    strncpy(pending_var, it->name, sizeof(pending_var) - 1);
    pending_var[sizeof(pending_var) - 1] = 0;
    pending_var_type = it->type;
    pending_calc = true;
    pending_delete = true;
    sprintf(msg, "Delete %s?", pending_var);
    set_status(msg);
    status_timed = false;      /* stays until Y= or Graph answers it */
}

/* Deletes the calculator variable waiting in pending_var. The calculator refuses
 * some types (protected programs), so the result is checked. */
static void perform_calc_delete(void) {
    int ok = ti_DeleteVar(pending_var, pending_var_type);

    if (ok && strcmp(pending_var, "FILERCFG") == 0) {
        /* No saved colors now: go back to the defaults, on screen as well. */
        memcpy(theme_idx, theme_default, THEME_ROLES);
        memcpy(theme_saved, theme_default, THEME_ROLES);
        restore_palette();
        set_status("Colors reset to default");
    } else {
        set_status(ok ? "Deleted variable" : "Delete failed (protected?)");
    }
    calc_refresh();
}

/* Y= while a delete is waiting: delete it. */
static void confirm_delete(void) {
    pending_delete = false;
    if (pending_calc) {
        pending_calc = false;
        perform_calc_delete();
        return;
    }
    perform_delete(pending_path, pending_is_dir);
}

/* Graph while a delete is waiting: leave it alone. */
static void cancel_delete(void) {
    pending_delete = false;
    pending_calc = false;
    set_status("Delete cancelled");
}

/* Shows a message on screen right away, before work that takes a moment. */
static void show_now(const char *msg) {
    set_status(msg);
    draw();
}

/* Flushes and closes the drive so it can be unplugged safely. The device stays
 * connected, so replugging is what mounts it again. */
static void unmount_usb(void) {
    if (!msd_ready) {
        set_status("No USB drive to unmount");
        return;
    }
    show_now("Unmounting...");
    write_ok = true;
    fat_close_volume();
    write_ok = false;
    if (mounted) lfn_fix_fsinfo(&msd, part_lba[cur_part]);   /* free count, once per session */
    unmount_device();
    {
        char msg[40];
        sprintf(msg, "Unmounted. dropped %lu, written %lu",
                (unsigned long)writes_blocked, (unsigned long)writes_done);
        set_status(msg);
    }
}

/* ---------- Rename ---------- */

/* The character a key types while renaming, or 0. Call before keys_process so
 * the alpha state is the one from before this key. Letters are uppercase. */
static char rename_char(uint8_t key) {
    char c;
    if (keys_alpha_active()) {
        c = keys_letter(key);
        if (!c) return 0;
        /* Alpha lock types lowercase, unless 2nd is held (2nd + alpha lock is uppercase).
         * A single alpha press is uppercase. keys_2nd_active() must be read before keys_process. */
        if (keys_alock_active() && !keys_2nd_active()) return (char)tolower((unsigned char)c);
        return (char)toupper((unsigned char)c);
    }
    switch (key) {
        case sk_0: return '0';
        case sk_1: return '1';
        case sk_2: return '2';
        case sk_3: return '3';
        case sk_4: return '4';
        case sk_5: return '5';
        case sk_6: return '6';
        case sk_7: return '7';
        case sk_8: return '8';
        case sk_9: return '9';
        case sk_DecPnt: return '.';
        default: return 0;
    }
}

/* True if s fits FAT 8.3: 1 to 8 characters, then optionally a dot and 1 to 3. */
static bool rename_name_ok(const char *s) {
    const char *dot = strchr(s, '.');
    size_t base = dot ? (size_t)(dot - s) : strlen(s);
    size_t ext;

    if (base < 1 || base > 8) return false;
    if (!dot) return true;
    ext = strlen(dot + 1);
    return ext >= 1 && ext <= 3 && strchr(dot + 1, '.') == NULL;
}

/* Graph on a USB file: start typing the new name. */
static void start_rename(void) {
    item_t *it;
    char name[16];

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Rename: select a file or folder");
        return;
    }
    strncpy(name, it->name, sizeof(name) - 1);   /* folder names carry a trailing "/" */
    name[sizeof(name) - 1] = 0;
    strip_trailing_slash(name);
    make_path(rename_src, drive_path, name);
    rename_buf[0] = 0;
    rename_mode = true;
}

/* Enter while renaming: the entry is renamed in place. Nothing is copied, so
 * this works for folders and for big files alike. */
static void finish_rename(void) {
    if (!rename_name_ok(rename_buf)) {
        set_status("Name: 1-8 letters, optional .ext");
        return;
    }
    rename_mode = false;
    finish_writes();                  /* the driver has nothing pending before the raw edit */
    if (lfn_rename(&msd, part_lba[cur_part], rename_src, rename_buf)) {
        if (strcmp(mark_path, rename_src) == 0) mark_set = false;   /* the marked entry is renamed */
        set_status("Renamed");
    } else {
        set_status("Rename failed: name taken?");
    }
    drive_refresh();
}

/* Keys while typing a new name. typed is the character this key gave, or 0. */
static void rename_key(uint8_t key, char typed) {
    size_t n = strlen(rename_buf);

    if (typed) {
        if (n < 12) {
            rename_buf[n] = typed;
            rename_buf[n + 1] = 0;
        }
    } else if (key == sk_Left || key == sk_Del) {     /* Del deletes the last character */
        if (n > 0) rename_buf[n - 1] = 0;
    } else if (key == sk_Enter) {
        finish_rename();
    } else if (key == sk_Clear) {
        rename_mode = false;
        set_status("Rename cancelled");
    }
    needs_redraw = true;
}

/* ---------- Input ---------- */

static void open_menu(enum menu_kind kind) {
    menu_kind = kind;
    menu_sel = 0;
    menu_open = true;
    needs_redraw = true;
}

static void close_menu(void) {
    menu_open = false;
    needs_redraw = true;
}

/* Runs the item the menu has selected, then closes the menu. */
static void menu_run(void) {
    enum menu_kind kind = menu_kind;
    int sel = menu_sel;

    close_menu();
    if (kind == MENU_FILES) {
        if (sel == 0) new_folder();
        else zoom_delete();
    } else {
        if (sel == 0) {
            start_rename();
        } else if (sel == 1) {
            if (mark_set) drop_marked();
            else set_status("Move: mark a file with Trace first");
        } else {
            if (mark_set) copy_marked();
            else set_status("Copy: mark a file with Trace first");
        }
    }
}

/* Keys while a menu is open: Up and Down move, Enter runs, Clear or Del closes. */
static void menu_key(uint8_t key) {
    int n = menu_count();

    if (key == sk_Up) menu_sel = (menu_sel + n - 1) % n;
    else if (key == sk_Down) menu_sel = (menu_sel + 1) % n;
    else if (key == sk_Enter) menu_run();
    else if (key == sk_Clear || key == sk_Del) close_menu();
    else if ((key == sk_Window && menu_kind == MENU_FILES) ||
             (key == sk_Zoom && menu_kind == MENU_ACTIONS)) close_menu();   /* same key closes it */
    needs_redraw = true;
}

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
            reset_usb_filters();
            drive_refresh();
        }
        reset_view_position();
    }
}

/* Drops the status message at once, back to the key legend. */
static void clear_status_message(void) {
    status[0] = 0;
    status_timed = false;
    status_changed = true;
    pending_delete = false;   /* a pending delete is dropped with its message */
}

/* Alpha + Up/Down: moves one screen of rows (no wrap). */
static void page_selection(int step) {
    clear_status_message();
    if (item_count == 0) return;
    selected += step * VISIBLE_ROWS;
    if (selected < 0) selected = 0;
    if (selected > item_count - 1) selected = item_count - 1;
    clamp_selection();
    needs_redraw = true;
}

/* Up at the top wraps to the bottom; Down at the bottom wraps to the top. */
static void move_selection(int delta) {
    clear_status_message();
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
    reset_usb_filters();
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
                reset_usb_filters();
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
            reset_usb_filters();
            drive_refresh();
            reset_view_position();
            break;

        case KIND_FILE:
            open_file(it);
            break;

        case KIND_VAR:
            break;   /* the info panel opens with Window */
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

/* Icon palette, the theme colors, and the marked-file color. The image viewer
 * replaces the whole palette, so this runs again when an image closes. */
static void restore_palette(void) {
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
static void theme_snapshot(void) {
    memcpy(palette256, gfx_palette, sizeof(palette256));
}

/* Sets theme_default to the palette entry closest to each starting color. */
static void theme_find_defaults(void) {
    int r, i;

    for (r = 0; r < THEME_ROLES; r++) {
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
}

/* ---------- Saved colors ----------
 * The AppVar FILERCFG holds a magic byte, then one palette index per role. A
 * missing or unreadable AppVar means the defaults. */
#define THEME_VAR   "FILERCFG"
#define THEME_MAGIC 0x47   /* 'G': palette indices. Older 'F' saves are ignored. */

static void theme_load(void) {
    uint8_t buf[1 + THEME_ROLES];
    uint8_t h;
    int i;
    bool good = false;

    memcpy(theme_idx, theme_default, THEME_ROLES);
    memcpy(theme_saved, theme_default, THEME_ROLES);
    h = ti_OpenVar(THEME_VAR, "r", OS_TYPE_APPVAR);
    if (!h) return;
    if (ti_Read(buf, 1, sizeof(buf), h) == sizeof(buf) && buf[0] == THEME_MAGIC) {
        good = true;   /* every byte is a palette index, so all are valid */
    }
    ti_Close(h);
    if (good) {
        for (i = 0; i < THEME_ROLES; i++) theme_idx[i] = theme_saved[i] = buf[1 + i];
    }
}

/* Writes the current colors to FILERCFG. Returns false if the AppVar can't be written. */
static bool theme_save(void) {
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
static void settings_begin(void) {
    settings_open = true;
    settings_sel = 0;
    needs_redraw = true;
}

static void settings_close(bool save) {
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
static int settings_digit(uint8_t key) {
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

static void settings_key(uint8_t key) {
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
        return;
    }
    settings_typing = false;

    if (key == sk_Up) {
        settings_sel = (uint8_t)((settings_sel + THEME_ROLES - 1) % THEME_ROLES);
    } else if (key == sk_Down) {
        settings_sel = (uint8_t)((settings_sel + 1) % THEME_ROLES);
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
}

/* Draws the settings box over the screen. Each row's swatch is the role's
 * current palette slot, so it shows the color as it is now. */
static void draw_settings(void) {
    int x = 16, y = 4, w = SCREEN_W - 32;    /* high enough that all rows fit on screen */
    int h = 20 + THEME_ROLES * 18 + 30;
    int i, ry;

    gfx_SetColor(T_OUT);
    gfx_FillRectangle(x, y, w, h);
    gfx_SetColor(T_BG);
    gfx_FillRectangle(x + 1, y + 1, w - 2, h - 2);
    gfx_SetColor(T_TOP);
    gfx_FillRectangle(x + 1, y + 1, w - 2, 16);
    text_at(x + 6, y + 4, "Colors", T_TOPTXT, T_TOP);

    for (i = 0; i < THEME_ROLES; i++) {
        int sel = i == settings_sel;
        ry = y + 20 + i * 18;
        if (sel) {
            gfx_SetColor(T_SEL);
            gfx_FillRectangle(x + 3, ry, w - 6, 16);
        }
        text_at(x + 8, ry + 4, THEME_ROLE_NAME[i], sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        {
            char num[5];
            sprintf(num, "%3d", theme_idx[i]);             /* the value, so typed digits show */
            text_at(x + w - 100, ry + 4, num, sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        }
        text_at(x + w - 62, ry + 4, "<", sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
        gfx_SetColor(THEME_ROLE_SLOT[i]);
        gfx_FillRectangle(x + w - 50, ry + 3, 26, 10);
        gfx_SetColor(T_OUT);
        gfx_Rectangle(x + w - 50, ry + 3, 26, 10);
        text_at(x + w - 18, ry + 4, ">", sel ? T_SELTXT : T_TXT, sel ? T_SEL : T_BG);
    }
    text_at(x + 6, y + h - 26, "Up/Down: pick  Left/Right or 0-9: value", T_TXT, T_BG);
    text_at(x + 6, y + h - 13, "Enter: save   Mode/Clear: cancel", T_TXT, T_BG);
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
    char typed = 0;            /* character typed by this key while renaming */
    bool last_ready = false;
    bool second_held = false;  /* 2nd is on (tapped or held), read before this key changes it */

    gfx_Begin();
    theme_snapshot();          /* before any palette change */
    theme_find_defaults();
    status_at = (uint32_t)clock();
    status_timed = true;
    {
        int i;
        for (i = 0; i < 16; i++) ti_pal[i] = gfx_palette[basic_palette[i]];  /* before FileR overwrites it */
    }
    theme_load();
    viewer_set_overlay(draw_viewer_overlay);
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
        /* 2nd is sticky after a tap, so either the modifier or the key being held counts. */
        second_held = keys_2nd_active() || kb_IsDown(kb_Key2nd);
        /* 2nd + Mode exits. Mode alone opens the color menu, so this check must come first. */
        if (key == sk_Mode && second_held && !rename_mode && !search_mode) break;
        /* A key counts once per press. Holding it down doesn't run the modifiers again. */
        pressed = key != 0 && key != last_key;
        last_key = key;
        letter = 0;
        typed = 0;
        page_key = false;
        if (pressed) {
            typing_now = search_mode;                       /* only y= feeds the search */
            letter = search_mode ? keys_char(key) : 0;
            typed = rename_mode ? rename_char(key) : 0;      /* before the modifiers change */
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
            /* Mode opens the color menu here too; while it is open it takes the keys. */
            if (settings_open) {
                if (pressed) settings_key(key);
            } else if (pressed && key == sk_Mode && !second_held) {
                settings_begin();
            } else if (viewer_key(key)) {
                needs_redraw = true;
            }
        } else if (var_info_is_usb() && pressed && key == sk_2nd) {
            var_info_close();          /* 2nd on the USB panel unmounts the drive */
            unmount_usb();
            needs_redraw = true;
        } else if (var_info_is_open()) {
            if (var_info_key(key)) calc_refresh();
            needs_redraw = true;
        } else if (rename_mode) {
            if (pressed) rename_key(key, typed);
        } else if (settings_open) {
            if (pressed) settings_key(key);
        } else if (menu_open) {
            if (pressed) menu_key(key);
        } else if (filter_open) {
            if (pressed) filter_key(key);
        } else if (letter) {
            type_letter(letter);
        } else {
            switch (key) {
                case sk_Up:    if (page_key) page_selection(-1); else move_selection(-1); break;
                case sk_Down:  if (page_key) page_selection(1);  else move_selection(1);  break;
                case sk_Enter: activate_selected(); break;
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
                case sk_Mode:
                    if (pressed && !search_mode && !second_held) settings_begin();   /* 2nd + Mode quits, handled above */
                    break;
                case sk_Del:
                    if (location == LOC_DRIVE) zoom_delete();   /* Del deletes; 2nd + Mode quits */
                    else if (location == LOC_CALC) calc_delete_prompt();
                    break;
                case sk_Yequ:
                    if (pending_delete) {
                        confirm_delete();
                    } else {
                        search_mode = !search_mode;
                        set_status(search_mode ? "y=: search on, y= to stop" : "y=: search off");
                    }
                    break;
                case sk_Window:
                    if (location == LOC_CALC) {
                        if (item_count > 0 && items[selected].kind == KIND_VAR) {
                            var_info_open(items[selected].name, items[selected].type);
                        } else if (item_count > 0 && items[selected].kind == KIND_USB) {
                            var_info_open_usb(part_count, msd_ready);
                        } else {
                            set_status("Info: select a calculator variable");
                        }
                    } else if (location == LOC_DRIVE) {
                        open_menu(MENU_FILES);
                    }
                    break;
                case sk_Zoom:
                    if (location == LOC_DRIVE) {
                        open_menu(MENU_ACTIONS);
                    } else {
                        set_status("Zoom: unassigned");
                    }
                    break;
                case sk_Trace:
                    if (location == LOC_DRIVE) mark_selected();
                    else set_status("Trace: unassigned");
                    break;
                case sk_Graph:
                    if (pending_delete) {
                        cancel_delete();
                    } else if (location == LOC_DRIVE || location == LOC_CALC) {
                        filter_open = true;
                        filter_sel = 0;
                        filter_drop = false;
                        needs_redraw = true;
                    } else {
                        set_status("Graph: unassigned");
                    }
                    break;
                default:
                    break;
            }
        }

        if (status_timed && (uint32_t)clock() - status_at >= (uint32_t)STATUS_MS * CLOCKS_PER_SEC / 1000UL) {
            clear_status_message();
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

    /* Every way out of the loop (Del, or Clear at the calculator root) lands here.
     * Close any open viewer first, since it holds a file on the drive, then the
     * drive itself. Writes are allowed for the final close so the drive is saved
     * cleanly. */
    show_now("Exiting...");
    if (viewer_is_open()) viewer_close();
    if (image_is_open()) image_close();
    /* No drive unmount here: every operation already flushes the volume, and the
     * FSInfo recount is slow. */
    mounted = false;
    msd_ready = false;
    if (msd_open) {
        msd_Close(&msd);
        msd_open = false;
    }
    usb_Cleanup();
    gfx_End();
    return 0;
}
