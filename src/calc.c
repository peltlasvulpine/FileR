#include "filer.h"

/* ---------- Labels and overlays ---------- */

const char *overlay_label(uint8_t ov) {
    switch (ov) {
        case OV_APPVAR: return "AppVar";
        case OV_PRGM:   return "Prgm";
        case OV_APP:    return "App";
        case OV_VAR:    return "Var";
        case OV_TEXT:   return "Text";
        case OV_HTML:   return "HTML";
        case OV_IMAGE:  return "Image";
        case OV_GIF:    return "GIF";
        default:        return NULL;
    }
}

/* Calculator variable -> overlay, from its OS type. */
uint8_t overlay_for_os_type(uint8_t type) {
    if (type == OS_TYPE_APPVAR) return OV_APPVAR;
    if (type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM) return OV_PRGM;
    return OV_VAR;
}

/* True if the extension after the last dot equals ext (any case). */
bool ext_equals(const char *dot, const char *ext) {
    size_t n = strlen(ext);
    size_t i;

    if (strlen(dot) != n + 1) return false;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)dot[1 + i]) != ext[i]) return false;
    }
    return true;
}

/* Drive file -> overlay, from its extension (any case). */
uint8_t overlay_for_filename(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return OV_NONE;
    if (ext_equals(dot, "txt")) return OV_TEXT;
    if (ext_equals(dot, "html") || ext_equals(dot, "htm")) return OV_HTML;
    if (ext_equals(dot, "png") || ext_equals(dot, "bmp")) return OV_IMAGE;
    if (ext_equals(dot, "gif")) return OV_GIF;
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

static void add_app_items(void);

bool type_matches(uint8_t type) {
    switch (type_filter) {
        case TYPE_APPVAR: return type == OS_TYPE_APPVAR;
        case TYPE_PRGM:   return type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM;
        case TYPE_VAR:    return overlay_for_os_type(type) == OV_VAR;
        case TYPE_APP:    return false;      /* apps are listed separately (add_app_items) */
        default:          return true;
    }
}

/* Looks up whether a variable is in the archive. Always called, so the
 * archive marker can be shown on every row. */
bool is_archived(const char *name, uint8_t type) {
    uint8_t h;
    bool archived;

    h = ti_OpenVar(name, "r", type);
    if (!h) return false;
    archived = ti_IsArchived(h) != 0;
    ti_Close(h);
    return archived;
}

bool storage_matches(bool archived) {
    switch (store_filter) {
        case STORE_ARCHIVE: return archived;
        case STORE_RAM:     return !archived;
        default:            return true;
    }
}

/* ---------- Filters ----------
 * Calculator: Store and Type. USB drive: Show (all, folders or files) and Ext
 * (any, TXT or BMP). All are set in the Graph popup. */
uint8_t usb_kind = 0;    /* 0 all, 1 folders only, 2 files only */
uint8_t usb_ext = 0;     /* 0 any, 1 .TXT, 2 .HTML, 3 .HTM, 4 .BMP, 5 .GIF */
uint8_t usb_hidden = 0;  /* USB: 0 all, 1 visible only, 2 hidden only (FAT hidden attribute) */
uint8_t usb_sort = 0;    /* USB: 0 A-Z, 1 Z-A */
const char *const store_opts[] = { "All", "Archive", "RAM" };
const char *const type_opts[]  = { "All", "AppVar", "Prgm", "Var", "App" };
const char *const hidden_opts[] = { "All", "Visible", "Hidden" };
const char *const sort_opts[] = { "A-Z", "Z-A" };
const char *const kind_opts[]  = { "All", "Folders", "Files" };
const char *const ext_opts[]   = { "Any", "TXT", "HTML", "HTM", "BMP", "GIF" };
const char *const ext_want[]   = { NULL, "TXT", "HTML", "HTM", "BMP", "GIF" };


/* Fills rows with the filters for the list on screen and returns how many. */
uint8_t filter_rows_build(filter_row_t *rows) {
    if (location == LOC_CALC) {
        rows[0] = (filter_row_t){ "Store", store_opts, STORE_COUNT, &store_filter };
        rows[1] = (filter_row_t){ "Type", type_opts, TYPE_COUNT, &type_filter };
        rows[2] = (filter_row_t){ "Hidden", hidden_opts, 3, &hidden_filter };
        rows[3] = (filter_row_t){ "Sort", sort_opts, 2, &sort_filter };
        return 4;
    }
    rows[0] = (filter_row_t){ "Show", kind_opts, 3, &usb_kind };
    rows[1] = (filter_row_t){ "Ext", ext_opts, EXT_COUNT, &usb_ext };
    rows[2] = (filter_row_t){ "Hidden", hidden_opts, 3, &usb_hidden };
    rows[3] = (filter_row_t){ "Sort", sort_opts, 2, &usb_sort };
    return 4;
}

/* Called when moving to another folder or drive: the USB filters go back to All. */
void reset_usb_filters(void) {
    usb_kind = 0;
    usb_ext = 0;
    usb_hidden = 0;
}

/* True if a drive item passes the USB filters. Folders stay visible unless only
 * files are wanted, so you can still get into them. */
bool usb_matches(enum kind k, const char *name) {
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
bool contains_ci(const char *hay, const char *needle) {
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

/* Position of a name byte in the sort order: A-Z, then theta (0x40 in program names,
 * 0x5B in variables), then 0-9, then everything else by its byte value. */
int sort_rank(uint8_t c) {
    /* AppVars can start with a lowercase letter: it sorts with its capital. */
    if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c == 0x40 || c == 0x5B) return 27;
    if (c >= '0' && c <= '9') return 30 + (c - '0');
    return 100 + c;
}

/* Compares two shown names by sort_rank, a shorter name first when one is a prefix. */
int name_cmp(const char *a, const char *b) {
    int i;

    for (i = 0;; i++) {
        int ra = a[i] ? sort_rank((uint8_t)a[i]) : 0;
        int rb = b[i] ? sort_rank((uint8_t)b[i]) : 0;
        if (ra != rb) return ra - rb;
        if (!a[i]) return 0;
    }
}

/* Sorts the calculator's variables (items 1 and up) by their shown names, A-Z or Z-A.
 * Insertion sort: the list is short. */
void sort_items(bool desc, bool use_shown) {
    int i, j;

    for (i = 2; i < item_count; i++) {
        item_t tmp = items[i];
        j = i;
        while (j > 1) {
            int c = use_shown ? name_cmp(tmp.shown, items[j - 1].shown)
                              : name_cmp(tmp.name, items[j - 1].name);
            bool before = desc ? c > 0 : c < 0;
            if (!before) break;
            items[j] = items[j - 1];
            j--;
        }
        items[j] = tmp;
    }
}

/* Keeps row 0 (the USB or ".." row) and only the rows whose names match. */
void apply_search(void) {
    int i, j = 1;

    if (search[0] == 0) return;
    for (i = 1; i < item_count; i++) {
        if (contains_ci(items[i].kind == KIND_VAR ? items[i].shown : items[i].name, search)) {
            if (j != i) items[j] = items[i];
            j++;
        }
    }
    item_count = j;
}

void clear_search(void) {
    search[0] = 0;
}

/* Called whenever the folder or list changes: empties the search and turns search
 * mode off, so the next folder starts with a clean screen. */
void leave_search(void) {
    clear_search();
    search_mode = false;
}

/* ---------- Data refresh ---------- */

void clamp_selection(void) {
    if (selected >= item_count) selected = item_count > 0 ? item_count - 1 : 0;
    if (selected < 0) selected = 0;
    if (top > selected) top = selected;
    if (selected >= top + VISIBLE_ROWS) top = selected - VISIBLE_ROWS + 1;
    if (top < 0) top = 0;
}

void reset_view_position(void) {
    selected = 0;
    top = 0;
}

/* A-Z and 0-9: the characters a TI variable name can start with. */
bool name_char_ok(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

/* A-Z only: a variable name can't start with a digit or a lowercase letter. */
bool letter_ok(uint8_t c) {
    return c >= 'A' && c <= 'Z';
}

/* Fills out with the name as the list should show it.
 * - A program whose first byte is not A-Z or 0-9, but is A-Z or 0-9 after XOR with
 *   0x64, is hidden: the shown first byte is the XOR result. Only programs are checked.
 * - A list name starts with 0x5D: 0x5D then 0x01-0x06 is L1-L6, and 0x5D then a
 *   name is a custom list. Both are shown with the small L glyph 0x1C in place of 0x5D
 *   (L1-L6 as 0x1C then the digit). */
void shown_name(const char *raw, uint8_t type, char *out, size_t sz, bool *hidden) {
    uint8_t c = (uint8_t)raw[0];
    uint8_t x = c ^ 0x64;
    int i;

    strncpy(out, raw, sz - 1);
    out[sz - 1] = 0;
    *hidden = false;
    /* A variable name starts with a letter, so a program whose first byte is not A-Z is
     * hidden when that byte is a letter's index (A = 0x01 ... Z = 0x1A), or when XOR 0x64
     * gives a letter. */
    if (type == OS_TYPE_PRGM || type == OS_TYPE_PROT_PRGM) {
        if (!letter_ok(c)) {
            if (c >= 0x01 && c <= 0x1A) {
                out[0] = (char)(c + 'A' - 1);
                *hidden = true;
            } else if (c == 0x1B) {
                /* theta, hidden: 0x1B is theta's 0x5B XOR 0x40 */
                out[0] = (char)0x40;
                *hidden = true;
            } else if (letter_ok(x)) {
                out[0] = (char)x;
                *hidden = true;
            }
        }
    }
    /* Theta is stored as 0x5B and shown with the 0x40 glyph, in programs and in
     * AppVars (AppVar names can contain theta anywhere). Real and list names have no
     * theta, so this changes nothing for them. */
    for (i = 0; out[i]; i++) {
        if ((uint8_t)out[i] == 0x5B) out[i] = (char)0x40;
    }
    if ((uint8_t)out[0] == 0x5D) {
        uint8_t n = (uint8_t)out[1];
        if (n >= 0x01 && n <= 0x06) {
            out[0] = (char)0x1C;          /* L1-L6: the glyph, then the digit */
            out[1] = (char)('0' + n);
            out[2] = 0;
        } else if (name_char_ok(n) || n == 0x40) {   /* 0x40 is theta, already mapped from 0x5B above */
            out[0] = (char)0x1C;
        }
    }
}

/* The VAT bounds, the same addresses CEaShell uses. progPtr is where the walk starts
 * and pTemp is where it stops; the walk goes down in memory. */

/* Calculator root: the USB entry first, then the variables that pass the filters.
 * The VAT is walked directly, as CEaShell does, instead of ti_DetectAny, which skips
 * entries whose first name byte is not a valid name start (hidden programs). Each
 * entry's type byte is at p. The name is stored backwards from p-7, its length is at
 * p-6, and the next entry is p-7-len below. */
void calc_refresh(void) {
    icon_cache_clear();
    uint8_t *p;
    uint8_t *end;
    uint8_t type;
    uint8_t len;
    char raw[9];
    bool archived;
    item_t *it;
    int i;

    it = &items[0];
    strcpy(it->name, "usb/");
    it->kind = KIND_USB;
    it->type = 0;
    it->overlay = OV_NONE;
    it->archived = false;
    item_count = 1;

    p = VAT_PROG_PTR;
    end = VAT_PTEMP;
    while (p > end && item_count < MAX_ITEMS) {
        len = p[-6];
        type = p[0] & 0x1F;
        if (len >= 1 && len <= 8) {
            for (i = 0; i < len; i++) raw[i] = (char)p[-7 - i];
            raw[len] = 0;
            p = p - 7 - len;
        } else {
            p = p - 7 - len;
            continue;
        }
        if (!type_matches(type)) continue;
        archived = is_archived(raw, type);
        if (!storage_matches(archived)) continue;

        it = &items[item_count];
        strncpy(it->name, raw, 8);
        it->name[8] = 0;
        shown_name(it->name, type, it->shown, sizeof(it->shown), &it->hidden);
        if (hidden_filter == 1 && it->hidden) continue;    /* Visible: leave the hidden ones out */
        if (hidden_filter == 2 && !it->hidden) continue;   /* Hidden: only the hidden ones */
        it->kind = KIND_VAR;
        it->type = type;
        it->overlay = overlay_for_os_type(type);
        it->archived = archived;
        item_count++;
    }
    /* Apps are not in the variable table, so they are added here. They show under All and
     * App; they are left out of RAM (they live in flash) and under Hidden (apps are never hidden). */
    if ((type_filter == TYPE_ALL || type_filter == TYPE_APP) &&
        store_filter != STORE_RAM && hidden_filter != 2) {
        add_app_items();
    }
    sort_items(sort_filter != 0, true);
    apply_search();
    clamp_selection();
}

/* ---------- Apps ----------
 * Apps are not in the variable table, so they are found with the TI-OS app search,
 * the same calls CEAShell makes (src/asm/apps.s there, addresses from its ti84pceg.inc,
 * checked on OS 5.4.0.0034). Each search step saves and restores every register, so
 * the OS and this code are left as they were. Results go to these globals. */
volatile uint32_t app_last;     /* flash address of the app the last step found */
volatile uint8_t app_end;       /* $FF once the search has no more apps */

static void app_search_start(void) {
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "call $020384\n"        /* ZeroOP1 */
        "ld a, $14\n"           /* AppObj: the type the search looks for */
        "ld ($0D005F8), a\n"    /* OP1 */
        "pop iy\n"
        "pop ix\n"
        "pop hl\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
}

static void app_search_next(void) {
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "ld a, $0D\n"
        "call $0217C0\n"        /* FindAppCustom: carry when there are no more apps */
        "ld (_app_last), de\n"  /* ld does not change the flags */
        "sbc a, a\n"            /* A = $FF on carry (none left), 0 otherwise */
        "ld (_app_end), a\n"
        "pop iy\n"
        "pop ix\n"
        "pop hl\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
}

/* Adds one row per installed app. The name is in the app's header, 0x103 bytes past
 * its address (CEAShell's getAppName). Apps are read-only: no hide, lock, rename,
 * archive or delete. */
static void add_app_items(void) {
    int tries = 0;

    app_search_start();
    while (tries++ < 64 && item_count < MAX_ITEMS) {
        item_t *it;
        const char *p;

        app_search_next();
        if (app_end) break;
        p = (const char *)(uintptr_t)(app_last & 0xFFFFFF);
        it = &items[item_count];
        strncpy(it->name, p + 0x103, 8);
        it->name[8] = 0;
        strncpy(it->shown, it->name, sizeof(it->shown) - 1);
        it->shown[sizeof(it->shown) - 1] = 0;
        it->hidden = false;
        it->kind = KIND_APP;
        it->type = 0;
        it->overlay = OV_APP;            /* file icon with the app icon on top */
        it->app_addr = app_last & 0xFFFFFF;
        it->archived = true;    /* apps live in flash: always archived, and locked (drawn so) */
        item_count++;
    }
}

/* Partition list: ".." first, then one row per partition. */
void part_refresh(void) {
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
            fmt(it->name, "%s/", part_label[i]);
        } else {
            fmt(it->name, "Part %d/", i + 1);
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
uint8_t folder_contents(const char *path) {
    fat_dir_t d;
    fat_dir_entry_t e;
    bool any_file = false, any_dir = false;

    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return 0;
    while (fat_ReadDir(&d, &e) == FAT_SUCCESS && e.name[0] != 0) {
        if (e.name[0] == '.' || (e.attrib & FAT_VOLLABEL)) continue;
        /* Only entries the Hidden filter lets through count, so a folder whose
         * contents are all hidden shows as empty under Visible, and so on. */
        if (usb_hidden == 1 && (e.attrib & FAT_HIDDEN)) continue;
        if (usb_hidden == 2 && !(e.attrib & FAT_HIDDEN)) continue;
        if (e.attrib & FAT_DIR) any_dir = true;
        else any_file = true;
        if (any_file) break;
    }
    fat_CloseDir(&d);
    if (any_file) return 1;
    return any_dir ? 2 : 0;
}

void drive_refresh(void) {
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
    it->hidden = false;
    item_count = 1;

    if (fat_OpenDir(&fat, drive_path, &fdir) != FAT_SUCCESS) {
        set_status("Cannot open folder");
        clamp_selection();
        return;
    }
    while (item_count < MAX_ITEMS &&
           fat_ReadDir(&fdir, &entry) == FAT_SUCCESS &&
           entry.name[0] != 0) {
        if (entry.attrib & FAT_VOLLABEL) continue;
        /* Hidden filter, as on the calculator: All, Visible (not hidden), Hidden (only hidden). */
        if (usb_hidden == 1 && (entry.attrib & FAT_HIDDEN)) continue;
        if (usb_hidden == 2 && !(entry.attrib & FAT_HIDDEN)) continue;
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
        it->hidden = (entry.attrib & FAT_HIDDEN) != 0;   /* drawn in the Hidden color */
        it->type = 0;
        it->archived = false;
        item_count++;
    }
    fat_CloseDir(&fdir);

    sort_items(usb_sort != 0, false);

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
void refresh_current(void) {
    switch (location) {
        case LOC_CALC:  calc_refresh(); break;
        case LOC_PART:  part_refresh(); break;
        case LOC_DRIVE: drive_refresh(); break;
    }
}

