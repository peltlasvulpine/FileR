#include "filer.h"

/* ---------- FAT callbacks ---------- */

uint24_t read_cb(void *usr, uint32_t lba, uint24_t count, void *buf) {
    (void)usr;
    return msd_Read(&msd, lba, count, buf);
}

/* Writes reach the drive only while a file operation runs (write_ok). Any
 * other write the FAT driver makes (e.g. the dirty bit on open) is discarded. */
uint32_t writes_blocked = 0;   /* diagnostic: write requests dropped outside file operations */
uint32_t writes_done = 0;      /* diagnostic: write requests passed to the drive */

uint24_t write_cb(void *usr, uint32_t lba, uint24_t count, const void *buf) {
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
void usb_fail(const char *msg) {
    if (boot_BatteryCharging()) {
        set_status("Charging, no data received");
    } else {
        set_status(msg);
    }
}

/* ---------- Mount / unmount ---------- */

/* Closes the FAT volume if it is open. Safe to call when it isn't. */
void fat_close_volume(void) {
    if (fat_open) {
        fat_Close(&fat);
        fat_open = false;
    }
}

/* Reads each partition's FAT volume label. A partition without a readable
 * label keeps an empty string and is shown as "Part N" instead. */
void read_part_labels(void) {
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
bool open_partition(int i) {
    fat_error_t ferr;
    char buf[40];

    /* Re-opening the FAT driver switches the volume, so any previous
     * partition is no longer the one being read. */
    mounted = false;
    fat_close_volume();
    ferr = fat_Open(&fat, read_cb, write_cb, NULL, part_lba[i]);
    if (ferr != FAT_SUCCESS) {
        fmt(buf, "Drive: FAT open failed (%d)", (int)ferr);
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
void mount_device(usb_device_t dev) {
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

    fmt(usb_label, "%d part%s", (int)n, n == 1 ? "" : "s");
    read_part_labels();

    if (n == 1) {
        open_partition(0);
    } else {
        fmt(buf, "Found %d partitions, press Enter", (int)n);
        set_status(buf);
    }
}

void unmount_device(void) {
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

uint8_t batt_level = 0;       /* last value read from the calculator */
bool batt_charging = false;

/* Reads the battery from the calculator. Called now and then from the main
 * loop, not on every redraw. */
void battery_poll(void) {
    batt_level = boot_GetBatteryStatus();
    batt_charging = boot_BatteryCharging() != 0;
}

/* ---------- USB event handler ---------- */

usb_error_t usb_handler(usb_event_t event, void *event_data,
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

/* ---------- Path helpers ---------- */

void drive_go_up(void) {
    char *slash = strrchr(drive_path, '/');
    keys_alpha_off();                  /* alpha lock doesn't carry into another folder */
    if (!slash || slash == drive_path) {
        strcpy(drive_path, "/");
    } else {
        *slash = 0;
    }
}

void drive_go_down(const char *name) {
    size_t len = strlen(drive_path);
    size_t add = strlen(name);
    keys_alpha_off();                  /* alpha lock doesn't carry into another folder */
    if (!(len > 0 && drive_path[len - 1] == '/')) {
        if (len + 1 >= PATH_MAX) return;
        strcat(drive_path, "/");
        len++;
    }
    if (len + add >= PATH_MAX) return;
    strcat(drive_path, name);
}

/* Folder names are stored with a trailing "/" for display; strip it. */
void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '/') s[n - 1] = 0;
}

/* ---------- USB file operations ---------- */

/* Builds folder/name. folder is "/" or a folder path without a trailing slash.
 * The parameter is not called `dir`, because `dir` is a sprite macro. */
void make_path(char *out, const char *folder, const char *name) {
    if (strcmp(folder, "/") == 0) {
        fmt(out, "/%s", name);
    } else {
        fmt(out, "%s/%s", folder, name);
    }
}

/* Closes the volume with writes allowed, so the driver can save its closing
 * changes (clearing the dirty bit, updating free-space counts), then opens it
 * again. Called after every file operation. Writes made during browsing are
 * still discarded. */
void finish_writes(void) {
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
void new_folder(void) {
    char name[16] = "";
    char msg[96];
    fat_error_t err = FAT_ERROR_RW_FAILED;
    int n;

    if (!mounted) return;
    write_ok = true;
    for (n = 1; n <= 99 && err != FAT_SUCCESS; n++) {
        if (n == 1) strcpy(name, "NEWDIR");
        else fmt(name, "NEWDIR%d", n);
        err = fat_Create(&fat, drive_path, name, FAT_DIR);
    }
    write_ok = false;
    finish_writes();

    if (err == FAT_SUCCESS) {
        fmt(msg, "Made folder %s", name);
        set_status(msg);
        drive_refresh();
    } else {
        set_status("New folder failed");
    }
}

/* Marks the selected file (Trace). Zoom then moves it into whatever folder is open. */
void mark_selected(void) {
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

fat_error_t remove_entry(const char *path);   /* defined with the delete code */

/* Copies src_path to dst_dir/name, then deletes src_path. The original is only
 * deleted after the whole copy has been written, and a failed copy is removed.
 * Returns true if the file is now in the new place. */
bool move_file(const char *src_path, const char *dst_dir, const char *name, bool remove_src) {
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
    has_long = lfn_long_name(&msd, part_lba[cur_part], src_path, add_long, ADD_LONG_MAX);

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
        strncpy(add_dir, dst_dir, PATH_MAX - 1);
        add_dir[PATH_MAX - 1] = 0;
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
void flush_lfn(void) {
    if (!lfn_pending) return;
    lfn_pending = false;
    if (mounted) lfn_mark_deleted(&msd, &lfn_list);
}

/* Gives the copy its long name, after the volume has been flushed. */
void flush_add(void) {
    if (!add_pending) return;
    add_pending = false;
    if (mounted) lfn_add_long(&msd, part_lba[cur_part], add_dir, add_short, add_long);
}

/* True if the short entry for path is still on the drive. The driver can
 * remove the entry and still report an error, so this is checked directly
 * instead of trusting the delete's return value. */
bool still_on_drive(const char *path) {
    lfn_list_t tmp;
    return lfn_find(&msd, part_lba[cur_part], path, &tmp);
}

/* Runs move_file, then flushes the drive. Returns move_file's result. */
bool move_and_flush(const char *src, const char *dst_dir, const char *name) {
    bool moved = move_file(src, dst_dir, name, true);
    finish_writes();
    flush_lfn();
    flush_add();
    return moved;
}

/* True if the folder at path exists. */
bool path_exists(const char *path) {
    fat_dir_t d;
    if (fat_OpenDir(&fat, path, &d) != FAT_SUCCESS) return false;
    fat_CloseDir(&d);
    return true;
}

/* True if dst is src itself or inside it. A folder can't go into itself. */
bool inside_folder(const char *dst, const char *src) {
    size_t n = strlen(src);
    return strncmp(dst, src, n) == 0 && (dst[n] == '/' || dst[n] == 0);
}

/* A folder's entries, copied out so the folder can be written while it is
 * being copied (each write flushes the drive and would invalidate an open
 * directory). */

/* Fills *out with the entries of the folder at path. Returns the count, or -1. */
int list_dir(const char *path, kid_t **out) {
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
bool copy_folder(const char *src_path, const char *dst_dir, const char *name) {
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
void move_folder(const char *src_path, const char *dst_dir) {
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
void drop_marked(void) {
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
void copy_marked(void) {
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

/* ---------- Transfers between the drive and the calculator ----------
 * Prgm sends a USB file to the calculator, or exports a calculator variable to the
 * open USB folder. If the destination exists, the status line asks first:
 * Y= replaces it, Graph cancels. */
#define XFER_NONE   0
#define XFER_SEND   1
#define XFER_EXPORT 2
static int xfer_mode = XFER_NONE;
static char *xfer_path;                  /* USB file: the source (send) or destination (export); heap, see usb_alloc */
static char xfer_name[9];                /* calculator variable name, raw bytes */
static uint8_t xfer_type;
static bool xfer_arch;
static char xfer_label[13];              /* 8.3 name of the export */
static uint8_t *xfer_buf;                /* variable data read during an export; heap, see usb_alloc */

/* Allocates the transfer buffers on the heap, at startup (see mem_init). */
bool usb_alloc(void) {
    xfer_path = calloc(PATH_MAX + 16, 1);
    xfer_buf = calloc(FAT_BLOCK_SIZE, 1);
    return xfer_path && xfer_buf;
}
static uint16_t x_fill;                  /* export: bytes in copy_buf */
static uint32_t x_pos;                   /* export: bytes written so far, header included */
static uint32_t x_sum;                   /* export: checksum of bytes from offset 55 */
static bool x_ok;
static fat_file_t *x_out;
static void send_file_to_calc(const char *path, bool confirmed);
static void export_now(void);

bool xfer_pending(void) {
    return xfer_mode != XFER_NONE;
}

/* Y= while a transfer waits: do it, replacing the destination. */
void xfer_confirm(void) {
    int m = xfer_mode;

    xfer_mode = XFER_NONE;
    if (m == XFER_SEND) send_file_to_calc(xfer_path, true);
    else if (m == XFER_EXPORT) export_now();
}

/* Graph while a transfer waits: leave the destination alone. */
void xfer_cancel(void) {
    xfer_mode = XFER_NONE;
    set_status("Transfer cancelled");
}

/* Sends the TI file at path to the calculator as a program or AppVar. The header
 * layout (from cesium's tiformat.inc): type byte at 59, name at 60-67, archive flag at
 * 69 (0x80 = archived), entry length at 70-71, the variable's size word at 72-73, data
 * from 74. All little-endian. */
static void send_file_to_calc(const char *path, bool confirmed) {
    fat_file_t in;
    uint8_t type;
    uint8_t h;
    uint32_t total;
    uint32_t size;
    uint32_t left;
    size_t chunk;
    bool archived;
    bool ok = true;
    char vname[9];
    char msg[96];
    int i;

    if (fat_OpenFile(&fat, path, 0, &in) != FAT_SUCCESS) {
        set_status("Send: cannot open file");
        return;
    }
    total = fat_GetFileSize(&in);
    if (fat_ReadFile(&in, 1, copy_buf) != 1 || memcmp(copy_buf, "**TI83F*", 8) != 0) {
        fat_CloseFile(&in);
        set_status("Send: not a TI file");
        return;
    }
    type = copy_buf[59];
    if (type != OS_TYPE_PRGM && type != OS_TYPE_PROT_PRGM && type != OS_TYPE_APPVAR) {
        fat_CloseFile(&in);
        set_status("Send: only programs and AppVars");
        return;
    }
    for (i = 0; i < 8; i++) vname[i] = (char)copy_buf[60 + i];
    vname[8] = 0;
    archived = (copy_buf[69] & 0x80) != 0;
    size = (uint32_t)copy_buf[72] | ((uint32_t)copy_buf[73] << 8);
    if (size == 0 || total < 76 + size) {
        fat_CloseFile(&in);
        set_status("Send: file is damaged");
        return;
    }

    if (!confirmed) {
        h = ti_OpenVar(vname, "r", type);
        if (h) {
            ti_Close(h);
            fat_CloseFile(&in);
            strcpy(xfer_path, path);
            xfer_mode = XFER_SEND;
            fmt(msg, "Overwrite %s? Y= yes, Graph no", vname);
            set_status(msg);
            status_timed = false;
            return;
        }
    }

    h = ti_OpenVar(vname, "w", type);
    if (!h) {
        fat_CloseFile(&in);
        set_status("Send: could not create on calc");
        return;
    }
    /* First block: the data starts at 74 (after the size word at 72). */
    left = size;
    chunk = FAT_BLOCK_SIZE - 74;
    if (chunk > left) chunk = left;
    if (ti_Write(copy_buf + 74, 1, chunk, h) != chunk) ok = false;
    left -= (uint32_t)chunk;
    while (ok && left > 0) {
        if (fat_ReadFile(&in, 1, copy_buf) != 1) {
            ok = false;
            break;
        }
        chunk = left > FAT_BLOCK_SIZE ? FAT_BLOCK_SIZE : left;
        if (ti_Write(copy_buf, 1, chunk, h) != chunk) ok = false;
        left -= (uint32_t)chunk;
    }
    if (ok && archived) ti_SetArchiveStatus(true, h);
    ti_Close(h);
    fat_CloseFile(&in);

    if (!ok) {
        ti_DeleteVar(vname, type);     /* do not leave a partial program behind */
        set_status("Send: copy failed");
        return;
    }
    fmt(msg, "Sent %s to calc", vname);
    set_status(msg);
}

/* Prgm on a USB file: send it to the calculator. */
void send_to_calc(void) {
    item_t *it;
    char name[16];
    char path[PATH_MAX + 16];

    if (location != LOC_DRIVE || item_count == 0 || !mounted) return;
    it = &items[selected];
    if (it->kind != KIND_FILE) {
        set_status("Send: select a file");
        return;
    }
    strncpy(name, it->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    make_path(path, drive_path, name);
    send_file_to_calc(path, false);
}

/* Puts one byte into the export's output block. counted bytes go into the checksum. */
static void x_put(uint8_t b, bool counted) {
    if (x_fill == FAT_BLOCK_SIZE) {
        if (fat_WriteFile(x_out, 1, copy_buf) != 1) x_ok = false;
        x_fill = 0;
    }
    copy_buf[x_fill++] = b;
    if (counted) x_sum += b;
    x_pos++;
}

/* Writes the export file: the 55-byte TI header, one variable entry, and the checksum. */
static void export_now(void) {
    uint8_t h;
    uint32_t datalen;
    uint32_t left;
    size_t chunk;
    uint32_t i;
    fat_file_t out;
    const char *comment = "Exported by FileR";
    uint32_t total;
    uint32_t varlen;

    h = ti_OpenVar(xfer_name, "r", xfer_type);
    if (!h) {
        set_status("Export: cannot read variable");
        return;
    }
    datalen = ti_GetSize(h);
    if (datalen == 0 || datalen > 60000) {
        ti_Close(h);
        set_status("Export: variable is empty or too big");
        return;
    }
    total = datalen + 19;     /* entry header 2 + length 2 + 13 + data + checksum 2 */
    varlen = datalen + 2;     /* entry data = size word (2) + data */

    write_ok = true;
    if (fat_OpenFile(&fat, xfer_path, 0, &out) == FAT_SUCCESS) {   /* overwrite: remove the old file */
        fat_CloseFile(&out);
        fat_Delete(&fat, xfer_path);
    }
    x_ok = true;
    if (fat_Create(&fat, drive_path, xfer_label, 0) != FAT_SUCCESS ||
        fat_OpenFile(&fat, xfer_path, 0, &out) != FAT_SUCCESS) {
        write_ok = false;
        finish_writes();
        ti_Close(h);
        set_status("Export: could not create file");
        drive_refresh();
        return;
    }
    fat_SetFileSize(&out, 57 + total);   /* 55 header + total + 2 checksum */
    x_out = &out;
    x_fill = 0;
    x_pos = 0;
    x_sum = 0;

    /* header */
    for (i = 0; i < 8; i++) x_put((uint8_t)"**TI83F*"[i], false);
    x_put(0x1A, false); x_put(0x0A, false); x_put(0x00, false);
    for (i = 0; i < 42; i++) x_put(i < 17 ? (uint8_t)comment[i] : 0, false);
    x_put(total & 0xFF, false); x_put((total >> 8) & 0xFF, false);
    /* variable entry: counted in the checksum */
    x_put(0x0D, true); x_put(0x00, true);
    x_put(varlen & 0xFF, true); x_put((varlen >> 8) & 0xFF, true);
    x_put(xfer_type, true);
    for (i = 0; i < 8; i++) x_put(i < strlen(xfer_name) ? (uint8_t)xfer_name[i] : 0, true);
    x_put(0x00, true);                        /* version */
    x_put(xfer_arch ? 0x80 : 0x00, true);     /* flag: archived */
    x_put((datalen + 2) & 0xFF, true); x_put(((datalen + 2) >> 8) & 0xFF, true);
    x_put(datalen & 0xFF, true); x_put((datalen >> 8) & 0xFF, true);   /* size word */

    /* data */
    left = datalen;
    while (x_ok && left > 0) {
        chunk = left > FAT_BLOCK_SIZE ? FAT_BLOCK_SIZE : left;
        if (ti_Read(xfer_buf, 1, chunk, h) != chunk) {
            x_ok = false;
            break;
        }
        for (i = 0; i < chunk; i++) x_put(xfer_buf[i], true);
        left -= (uint32_t)chunk;
    }

    /* checksum: sum of bytes from offset 55 to the end of the data, 16 bits */
    x_put((uint8_t)(x_sum & 0xFF), false);
    x_put((uint8_t)((x_sum >> 8) & 0xFF), false);
    if (x_fill > 0) {
        while (x_fill < FAT_BLOCK_SIZE) copy_buf[x_fill++] = 0;
        if (fat_WriteFile(&out, 1, copy_buf) != 1) x_ok = false;
    }
    fat_CloseFile(&out);
    ti_Close(h);
    write_ok = false;
    if (!x_ok) {
        write_ok = true;
        fat_Delete(&fat, xfer_path);      /* do not leave a half-written file */
        write_ok = false;
    }
    finish_writes();
    if (x_ok) {
        char msg[96];
        fmt(msg, "Exported %s", xfer_label);
        set_status(msg);
    } else {
        set_status("Export: write failed");
    }
    drive_refresh();
}

/* Shortens a calculator name to an 8.3 file name: A-Z and 0-9 kept, other characters
 * become "_". The extension is .8XP for programs, .8XV for AppVars. */
static void calc_file_name(const char *shown, uint8_t type, char *out) {
    int n = 0;
    int i;
    char c;

    for (i = 0; shown[i] && n < 8; i++) {
        c = shown[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[n++] = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    if (n == 0) {
        strcpy(out, "VAR");
        n = 3;
    }
    out[n] = 0;
    strcat(out, type == OS_TYPE_APPVAR ? ".8XV" : ".8XP");
}

/* Trace on a calculator variable: pick a folder on the USB drive, then Trace there. */
static bool export_pick = false;
static char xfer_shown[17];              /* the name as the list shows it */

bool export_picking(void) {
    return export_pick;
}

void export_begin(void) {
    item_t *it;
    char msg[96];
    int i;

    leave_search();            /* exporting starts with an empty search and search off */
    refresh_current();         /* show the full list, not the filtered one */

    if (location != LOC_CALC || item_count == 0) {
        set_status("Export: select a calculator variable");
        return;
    }
    it = &items[selected];
    if (it->kind != KIND_VAR) {
        set_status("Export: select a calculator variable");
        return;
    }
    if (it->type != OS_TYPE_PRGM && it->type != OS_TYPE_PROT_PRGM && it->type != OS_TYPE_APPVAR) {
        set_status("Export: only programs and AppVars");
        return;
    }
    if (!msd_ready) {
        set_status("No USB drive connected");
        return;
    }
    for (i = 0; i < 8; i++) xfer_name[i] = it->name[i];
    xfer_name[8] = 0;
    xfer_type = it->type;
    xfer_arch = it->archived;
    strncpy(xfer_shown, it->shown, sizeof(xfer_shown) - 1);
    xfer_shown[sizeof(xfer_shown) - 1] = 0;

    export_pick = true;
    location = LOC_PART;             /* pick a partition, then a folder */
    part_refresh();
    reset_view_position();
    fmt(msg, "Export %s: pick a partition and folder, Trace, Clear cancels", xfer_shown);
    set_status(msg);
}

/* Trace in the USB list while picking: export the variable into the open folder. */
void export_here(void) {
    char fname[13];
    fat_file_t tmp;

    if (!export_pick) return;
    if (location != LOC_DRIVE || !mounted) {
        set_status("Export: choose a partition and folder first");
        return;
    }
    export_pick = false;
    calc_file_name(xfer_shown, xfer_type, fname);
    strcpy(xfer_label, fname);
    make_path(xfer_path, drive_path, fname);

    if (fat_OpenFile(&fat, xfer_path, 0, &tmp) == FAT_SUCCESS) {
        fat_CloseFile(&tmp);
        xfer_mode = XFER_EXPORT;
        {
            char msg[96];
            fmt(msg, "Overwrite %s? Y= yes, Graph no", fname);
            set_status(msg);
        }
        status_timed = false;
        return;
    }
    export_now();
}

/* Clear while picking a folder: cancel the export. */
void export_cancel(void) {
    export_pick = false;
    set_status("Export cancelled");
}
