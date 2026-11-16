#include "filer.h"

/* Allocates the big buffers on the heap. They are not in static RAM: a program's
 * static RAM has to stay small, or the calculator resets when it starts. */
bool state_alloc(void) {
    copy_buf = calloc(FAT_BLOCK_SIZE, 1);
    mark_path = calloc(PATH_MAX + 16, 1);
    rename_src = calloc(PATH_MAX + 16, 1);
    pending_path = calloc(PATH_MAX + 16, 1);
    add_long = calloc(ADD_LONG_MAX, 1);
    add_dir = calloc(PATH_MAX, 1);
    items = calloc(MAX_ITEMS, sizeof(item_t));
    return copy_buf && mark_path && rename_src && pending_path && add_long && add_dir && items;
}

/* ---------- State ---------- */

msd_t msd;
fat_t fat;
bool msd_open = false;     /* msd_Open succeeded */
bool msd_ready = false;    /* device open and partitions found */
bool mounted = false;      /* FAT volume open on one partition */
bool fat_open = false;     /* fat_Open succeeded and fat_Close not yet called */
bool write_ok = false;      /* true only while a file operation writes to the drive */
uint8_t *copy_buf;   /* one block, used while copying a file */
char *mark_path;      /* file marked with Trace: full path */
char mark_name[16];                 /* its name */
bool mark_set = false;
bool mark_is_dir = false;           /* the marked item is a folder */
bool copy_cancelled = false;        /* Clear stopped a folder copy */
bool rename_mode = false;           /* typing a new name for the selected file */
bool calc_rename = false;           /* the name being typed is for a calculator variable */
char calc_rename_old[9];            /* the variable's name before the rename (raw) */
uint8_t calc_rename_type = 0;       /* its OS type */
bool calc_rename_hidden = false;    /* the program's name was hidden (XOR 0x40) */
char rename_buf[13] = "";           /* the name typed so far */
bool new_file_mode = false;         /* the name being typed is for a new file */
char *rename_src;     /* full path of the file being renamed */
int cur_part = 0;                   /* partition index currently open */
bool pending_delete = false;        /* a delete is waiting for Y= (yes) or Graph (no) */
bool pending_calc = false;          /* the waiting delete is a calculator variable */
char pending_var[9];                /* its name */
uint8_t pending_var_type;           /* its OS type */
bool pending_is_dir = false;        /* what that delete would remove */
char *pending_path;   /* what that Zoom would delete */
lfn_list_t lfn_list;                /* long-name entries of the file being deleted */
bool lfn_pending = false;           /* a file was deleted; lfn_list must be cleared */
char *add_long;                 /* long name to give a copy, read from its source */
char add_short[13];                 /* the copy's 8.3 name */
char *add_dir;             /* folder the copy went into */
bool add_pending = false;           /* a copy needs its long name written */

int part_count = 0;
uint32_t part_lba[MAX_PARTS];
char part_label[MAX_PARTS][12];   /* FAT volume label, "" if none */
char usb_label[16] = "";          /* right-side text on the USB row */

char status[40] = "Waiting for USB drive...";
bool status_changed = true;

item_t *items;
int item_count = 0;

enum location location = LOC_CALC;
char drive_path[PATH_MAX] = "/";

uint8_t store_filter = STORE_ALL;   /* calculator filters, set in the Graph popup */
uint8_t type_filter = TYPE_ALL;
uint8_t hidden_filter = 0;          /* calculator: 0 all, 1 visible only, 2 hidden only (Type is then Prgm) */
uint8_t type_before = TYPE_ALL;     /* the Type filter before Hidden set it to Prgm */
bool type_forced = false;           /* Hidden set the Type filter to Prgm */
uint8_t sort_filter = 0;            /* calculator: 0 A-Z, 1 Z-A */
int selected = 0;
int top = 0;
bool needs_redraw = true;

char search[SEARCH_MAX] = "";
bool search_mode = false;   /* y= : letters type into the search */

uint32_t status_at = 0;     /* clock() value when the message was set */
bool status_timed = false;  /* message clears after STATUS_SECONDS */


/* ---------- Status ---------- */

void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    status_changed = true;
    status_at = (uint32_t)clock();
    status_timed = true;
}

