#include "filer.h"

/* ---------- Delete ---------- */

/* True if the folder at path holds nothing apart from . and .. */
bool dir_is_empty(const char *path) {
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
fat_error_t remove_entry(const char *path) {
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
bool delete_tree(const char *path) {
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
void perform_delete(const char *path, bool is_dir) {
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
        fmt(msg, "Delete failed (FAT error %d)", (int)code);
        set_status(msg);
    }
    drive_refresh();
}

/* Zoom on the USB list with nothing marked. An empty folder is deleted at once.
 * A file, or a folder with contents, needs a second Zoom to confirm. */
void zoom_delete(void) {
    item_t *it;
    char name[16];
    char path[PATH_MAX + 16];
    char msg[40];
    bool is_dir;

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Select a file or folder");
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
    fmt(msg, "Delete %s?", name);
    set_status(msg);
    status_timed = false;      /* stays until Y= or Graph answers it */
}

/* Del on a calculator variable (root of the calculator list). Asks first, same
 * as a drive delete. The USB row and folders aren't variables, so they're refused. */
void calc_delete_prompt(void) {
    item_t *it;
    char msg[40];

    if (location != LOC_CALC || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_VAR) {
        set_status("Select a variable");
        return;
    }
    /* FileR can't delete its own program. Deleting FILERCFG is allowed: it resets the colors. */
    if (strcmp(it->name, "FILER") == 0) {
        set_status("no");
        return;
    }
    strncpy(pending_var, it->name, sizeof(pending_var) - 1);
    pending_var[sizeof(pending_var) - 1] = 0;
    pending_var_type = it->type;
    pending_calc = true;
    pending_delete = true;
    fmt(msg, "Delete %s?", pending_var);
    set_status(msg);
    status_timed = false;      /* stays until Y= or Graph answers it */
}

/* Deletes the calculator variable waiting in pending_var. The calculator refuses
 * some types (protected programs), so the result is checked. */
void perform_calc_delete(void) {
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
void confirm_delete(void) {
    pending_delete = false;
    if (pending_calc) {
        pending_calc = false;
        perform_calc_delete();
        return;
    }
    perform_delete(pending_path, pending_is_dir);
}

/* Graph while a delete is waiting: leave it alone. */
void cancel_delete(void) {
    pending_delete = false;
    pending_calc = false;
    set_status("Delete cancelled");
}

/* Shows a message on screen right away, before work that takes a moment. */
void show_now(const char *msg) {
    set_status(msg);
    draw();
}

/* Flushes and closes the drive so it can be unplugged safely. The device stays
 * connected, so replugging is what mounts it again. */
void unmount_usb(void) {
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
        fmt(msg, "Unmounted. dropped %lu, written %lu",
                (unsigned long)writes_blocked, (unsigned long)writes_done);
        set_status(msg);
    }
}

/* ---------- Rename ---------- */

/* The character a key types while renaming, or 0. Call before keys_process so
 * the alpha state is the one from before this key. Letters are uppercase. */
char rename_char(uint8_t key) {
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

/* The character a key types in the editor: 2nd symbols first, then letters and digits
 * (as in rename), then the symbols on the plain and alpha keys. Call before keys_process. */
char editor_char(uint8_t key) {
    char c;

    if (keys_2nd_active()) {
        switch (key) {
            case sk_DecPnt: return ';';
            case sk_LParen: return '{';
            case sk_RParen: return '}';
            case sk_Mul:    return '[';
            case sk_Sub:    return ']';
            case sk_Enter:  return '=';
            default: break;
        }
    }
    c = rename_char(key);
    if (c) return c;
    if (keys_alpha_active()) {
        switch (key) {
            case sk_0:     return ' ';
            case sk_DecPnt: return ':';
            case sk_Add:   return '"';
            case sk_Left:  return '<';
            case sk_Right: return '>';
            default:       return 0;
        }
    }
    switch (key) {
        case sk_Add:    return '+';
        case sk_Sub:    return '-';
        case sk_Chs:    return '-';
        case sk_Mul:    return '*';
        case sk_Div:    return '/';
        case sk_Power:  return '^';
        case sk_LParen: return '(';
        case sk_RParen: return ')';
        case sk_Comma:  return ',';
        default:        return 0;
    }
}

/* True if s fits FAT 8.3: 1 to 8 characters, then optionally a dot and 1 to 3. */
bool rename_name_ok(const char *s) {
    const char *dot = strchr(s, '.');
    size_t base = dot ? (size_t)(dot - s) : strlen(s);
    size_t ext;

    if (base < 1 || base > 8) return false;
    if (!dot) return true;
    ext = strlen(dot + 1);
    return ext >= 1 && ext <= 3 && strchr(dot + 1, '.') == NULL;
}

/* Graph on a USB file: start typing the new name. */
void start_rename(void) {
    item_t *it;
    char name[16];

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Select a file or folder");
        return;
    }
    strncpy(name, it->name, sizeof(name) - 1);   /* folder names carry a trailing "/" */
    name[sizeof(name) - 1] = 0;
    strip_trailing_slash(name);
    make_path(rename_src, drive_path, name);
    rename_buf[0] = 0;
    new_file_mode = false;
    rename_mode = true;
}

/* Enter while renaming: the entry is renamed in place. Nothing is copied, so
 * this works for folders and for big files alike. */
void finish_rename(void) {
    if (!rename_name_ok(rename_buf)) {
        set_status("1-8 letters, optional .ext");
        return;
    }
    rename_mode = false;
    finish_writes();                  /* the driver has nothing pending before the raw edit */
    if (lfn_rename(&msd, part_lba[cur_part], rename_src, rename_buf)) {
        if (strcmp(mark_path, rename_src) == 0) mark_set = false;   /* the marked entry is renamed */
        set_status("Renamed");
    } else {
        set_status("Failed, name taken?");
    }
    drive_refresh();
}

/* A calculator variable name: 1 to 8 characters, starting with a letter. */
bool calc_name_ok(const char *s) {
    size_t n = strlen(s);
    size_t i;

    if (n < 1 || n > 8) return false;
    if (!isalpha((unsigned char)s[0])) return false;
    for (i = 1; i < n; i++) {
        if (!isalnum((unsigned char)s[i])) return false;
    }
    return true;
}

/* Y= on a calculator variable's Info panel: type a new name. */
void start_calc_rename(void) {
    item_t *it;

    if (location != LOC_CALC || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_VAR) return;
    strncpy(calc_rename_old, it->name, sizeof(calc_rename_old) - 1);
    calc_rename_old[sizeof(calc_rename_old) - 1] = 0;
    calc_rename_type = it->type;
    calc_rename_hidden = it->hidden;
    rename_buf[0] = 0;
    new_file_mode = false;
    calc_rename = true;
    rename_mode = true;
}

/* Renames a calculator variable the way Cesium does: a new variable with the new name gets
 * a copy of the data, then the old one is deleted. Nothing is edited in place, and the app
 * keeps running. The archive status and the lock carry over; a hidden program stays hidden.
 * Returns 0 on success, 1 if the new name is taken, 2 for any other failure (the copy is
 * removed again so nothing is lost). */
static int copy_rename_var(const char *old_name, const char *new_name, uint8_t type, bool hidden) {
    uint8_t src;
    uint8_t dst;
    uint8_t create_type;
    uint16_t left;
    bool archived;
    bool locked;
    char enc[9];
    void *entry = NULL;
    void *data = NULL;

    src = ti_OpenVar(old_name, "r", type);
    if (!src) return 2;
    /* A hidden program's first name byte is XOR 0x40, so the new name is encoded the same
     * way before it is created. Every call below uses enc, not the typed name. */
    strncpy(enc, new_name, sizeof(enc) - 1);
    enc[sizeof(enc) - 1] = 0;
    if (hidden) enc[0] = (char)(enc[0] ^ 0x40);
    locked = type == OS_TYPE_PROT_PRGM;
    create_type = locked ? OS_TYPE_PRGM : type;       /* locked is set after the copy */

    /* "w" would overwrite a variable with this name, so look under both program types first. */
    dst = ti_OpenVar(enc, "r", create_type);
    if (!dst) dst = ti_OpenVar(enc, "r", type);
    if (dst) {
        ti_Close(dst);
        ti_Close(src);
        return 1;
    }
    archived = ti_IsArchived(src) != 0;
    left = ti_GetSize(src);

    dst = ti_OpenVar(enc, "w", create_type);
    if (!dst) {
        ti_Close(src);
        return 2;
    }
    while (left > 0) {
        uint16_t n = left < FAT_BLOCK_SIZE ? left : FAT_BLOCK_SIZE;
        if (ti_Read(copy_buf, 1, n, src) != n || ti_Write(copy_buf, 1, n, dst) != n) {
            ti_Close(src);
            ti_Close(dst);
            ti_DeleteVar(enc, create_type);
            return 2;
        }
        left -= n;
    }
    ti_Close(src);
    if (locked) {
        /* Lock the new entry by its type byte, as the Info panel does, while it is in RAM. */
        if (os_ChkFindSym(create_type, enc, &entry, &data) && entry) {
            uint8_t *tb = (uint8_t *)entry;
            *tb = (uint8_t)((*tb & 0xE0) | OS_TYPE_PROT_PRGM);
        }
    }
    if (archived) ti_SetArchiveStatus(true, dst);     /* the handle is still open */
    ti_Close(dst);
    if (!ti_DeleteVar(old_name, type)) {
        ti_DeleteVar(enc, create_type);               /* could not remove the old: undo */
        return 2;
    }
    return 0;
}

/* Runs copy_rename_var under the OS error handler, so an OS error comes back as a return
 * value instead of a reset. An OS error number is returned as is. */
static int safe_var_rename(const char *old_name, const char *new_name, uint8_t type, bool hidden) {
    int err;
    int r;

    err = os_PushErrorHandler();
    if (err) return err;
    r = copy_rename_var(old_name, new_name, type, hidden);
    os_PopErrorHandler();
    return r;
}

/* Enter while renaming a calculator variable: the name is changed in the VAT. Names
 * other than AppVars are uppercase, as on the calculator. */
void finish_calc_rename(void) {
    size_t i;
    int res;

    if (calc_rename_type != OS_TYPE_APPVAR) {
        for (i = 0; rename_buf[i]; i++) rename_buf[i] = (char)toupper((unsigned char)rename_buf[i]);
    }
    if (!calc_name_ok(rename_buf)) {
        set_status("1-8 characters, starts with a letter");
        return;                                  /* keep typing */
    }
    rename_mode = false;
    calc_rename = false;
    if (strcmp(rename_buf, calc_rename_old) == 0) {
        set_status("Rename cancelled");
        return;
    }
    res = safe_var_rename(calc_rename_old, rename_buf, calc_rename_type, calc_rename_hidden);
    if (res != 0) {
        set_status(res == 1 ? "Failed, name taken" : "Failed, TI error");
        return;
    }
    calc_refresh();
    for (i = 0; i < (size_t)item_count; i++) {
        if (strcmp(items[i].shown, rename_buf) == 0) { selected = (int)i; break; }
    }
    clamp_selection();
    set_status("Renamed");
}

/* Files > New file: type a name, then an empty .txt opens in the editor. */
void start_new_file(void) {
    if (location != LOC_DRIVE) {
        set_status("Open a folder first");
        return;
    }
    rename_buf[0] = 0;
    new_file_mode = true;
    rename_mode = true;
}

void finish_new_file(void) {
    char name[16];
    char path[160];
    bool created;

    if (!rename_name_ok(rename_buf)) {
        set_status("1-8 letters, optional .ext");
        return;
    }
    strncpy(name, rename_buf, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    if (!strchr(name, '.')) strncat(name, ".txt", sizeof(name) - strlen(name) - 1);
    if (!editor_can_open(name)) {
        set_status("New file must end in .txt");
        return;
    }
    rename_mode = false;
    new_file_mode = false;
    finish_writes();
    write_ok = true;                     /* writes only reach the drive while this is set */
    created = fat_Create(&fat, drive_path, name, 0) == FAT_SUCCESS;
    write_ok = false;
    finish_writes();                     /* flush the new entry to the drive */
    drive_refresh();
    if (!created) {
        set_status("Failed, name taken?");
        return;
    }
    make_path(path, drive_path, name);
    if (!editor_open(&fat, path, true)) set_status(editor_error());
}

/* Actions > Edit: opens the selected .txt in the editor. */
void start_edit(void) {
    item_t *it;
    char path[160];

    if (location != LOC_DRIVE || item_count == 0) return;
    it = &items[selected];
    if (it->kind != KIND_FILE || !editor_can_open(it->name)) {
        set_status("Select a .txt file");
        return;
    }
    make_path(path, drive_path, it->name);
    if (!editor_open(&fat, path, false)) set_status(editor_error());
    needs_redraw = true;
}

/* Keys while typing a new name. typed is the character this key gave, or 0. */
void rename_key(uint8_t key, char typed) {
    size_t n = strlen(rename_buf);

    if (typed) {
        if (n < (calc_rename ? 8 : 12)) {
            rename_buf[n] = typed;
            rename_buf[n + 1] = 0;
        }
    } else if (key == sk_Left || key == sk_Del) {     /* Del deletes the last character */
        if (n > 0) rename_buf[n - 1] = 0;
    } else if (key == sk_Enter) {
        if (new_file_mode) finish_new_file();
        else if (calc_rename) finish_calc_rename();
        else finish_rename();
    } else if (key == sk_Clear) {
        rename_mode = false;
        new_file_mode = false;
        calc_rename = false;
        set_status("Rename cancelled");
    }
    needs_redraw = true;
}

/* ---------- Text viewer glue ---------- */

/* Builds the drive path to the selected file and opens it in the viewer
 * that matches its extension. */
void open_file(const item_t *it) {
    char fpath[PATH_MAX + 16];

    if (viewer_can_open(it->name) || image_can_open(it->name) || gif_viewer_can_open(it->name)) {
        if (strcmp(drive_path, "/") == 0) {
            fmt(fpath, "/%s", it->name);
        } else {
            fmt(fpath, "%s/%s", drive_path, it->name);
        }
    }
    if (viewer_can_open(it->name)) {
        if (!viewer_open(&fat, fpath, it->name)) set_status("Could not open file");
    } else if (image_can_open(it->name)) {
        if (!image_open(&fat, fpath, it->name)) {
            set_status("Could not open image (BMP, 8 or 24-bit, max 640 wide)");
        }
    } else if (gif_viewer_can_open(it->name)) {
        show_now("Loading GIF...");            /* decoding takes a second or two */
        if (!gif_viewer_open(&fat, fpath)) set_status(gif_viewer_error());
    } else {
        set_status("Unsupported file type");
    }
}

/* Enter on the USB row: open the drive, or the partition list if there are several. */
void enter_drive(void) {
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

void activate_selected(void) {
    item_t *it;
    bool navigates;

    if (item_count == 0) return;
    it = &items[selected];

    /* Moving between lists starts fresh, so a search never carries over. */
    navigates = it->kind != KIND_FILE && it->kind != KIND_VAR && it->kind != KIND_APP;
    if (navigates) leave_search();

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

        case KIND_APP:
            set_status("Apps can't be opened");   /* read-only: nothing to open */
            break;
    }
    needs_redraw = true;
}

/* Adds one typed letter (from alpha or alpha lock) to the search. */
void type_letter(char c) {
    size_t n = strlen(search);
    if (n < SEARCH_MAX - 1) {
        search[n] = c;
        search[n + 1] = 0;
        reset_view_position();
        refresh_current();
    }
    needs_redraw = true;
}

