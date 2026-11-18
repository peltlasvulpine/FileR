#ifndef FILER_H
#define FILER_H

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
#include "fmt.h"
#include "icon/gfx.h"   /* convimg output: all icon sprites + palette */
#include "text_viewer.h"    /* text viewer for .txt files */
#include "image_viewer.h"   /* image viewer for .bmp files */
#include "gif_viewer.h"     /* GIF viewer for .gif files */
#include "text_editor.h"    /* text editor for .txt files */
#include "var_info.h"     /* info panel for calculator variables */
#include "cache.h"
#include "mem.h"           /* startup allocation of the big buffers (heap, not static RAM) */
#include "keys.h"         /* 2nd and alpha modifiers */
#include "lfn_fix.h"      /* removes long-name leftovers after a delete */
#include <time.h>          /* clock(): status message timeouts */
#include <stdlib.h>        /* malloc/free: folder listings during a folder copy */

#define SCREEN_W      320
#define SCREEN_H      240
#define TITLE_H       20
#define FILTER_Y      24
#define LIST_Y        38
#define ROW_H         18          /* tall enough for the 18px icons */
#define VISIBLE_ROWS  10
#define STATUS_H      14
#define MAX_ITEMS     400         /* how many rows the list can hold (about 16 KB of RAM) */
#define MAX_PARTS     4
#define ADD_LONG_MAX 256         /* long-name buffer for copies, allocated at startup */
#define SEARCH_MAX    16          /* search text length, including room for NUL */
#define PATH_MAX      128
#define ICON_X        4           /* left icon column */
#define ICON_W        16          /* icon box: centers each sprite inside */
#define ICON_H        18
#define TEXT_X        24          /* names start after the icon column */
#define ARCHIVE_X     (SCREEN_W - 100)
#define LOCK_X        (SCREEN_W - 80)
#define LABEL_X       (SCREEN_W - 60)
#define PRGM_ICON_X   (ARCHIVE_X - ICON_W - 2)   /* program's own icon, left of archive */
#define PRGM_TRANSPARENT 0xDF   /* palette index treated as see-through in program icons */
#define C_BLACK  0x00
#define C_WHITE  0xFF
#define C_BLUE   0x32          /* title bar */
#define C_SELECT 0x7B          /* selected row */
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
#define T_HIDDEN 0x4A   /* name of a hidden program in the variable panel */
#define THEME_ROLES   11
#define THEME_PRESETS 256   /* every entry of the calculator's palette, 0-255 */
#define ROLE_HIDDEN   10    /* index of the Hidden role, the last one */
#define THEME_HIDDEN_DEFAULT 148   /* its starting preset: palette index 0x94 */
#define SETTINGS_VISIBLE 8                  /* role rows that fit in the color box */
#define STATUS_MS 2500             /* how long a status message stays, in milliseconds */
#define EXT_COUNT 6
#define VAT_PROG_PTR (*(uint8_t *volatile *)0xD0259D)
#define VAT_PTEMP    (*(uint8_t *volatile *)0xD0259A)
#define ICON_PIXELS 256
#define ICON_SCAN 32   /* how far into the program to look for the icon header */
#define BASIC_SCAN   128
#define BASIC_PIXELS 256
#define TI_PAL_BASE 0x80            /* slots 0x80..0x8F hold the 16 TI-BASIC colors */
#define THEME_VAR   "FILERCFG"
#define THEME_MAGIC 0x47   /* 'G': palette indices. Older 'F' saves are ignored. */
#define REPEAT_DELAY_MS 400
#define REPEAT_MS       90


/* ---------- Layout ---------- */


/* Right-side markers. They sit next to the type label and are never
 * drawn on top of the left icon. */

/* Palette indices. The icon palette is now convimg's built-in "xlibc" palette (xlibc.c),
 * not the old custom mypalette, so the exact slot range the icons occupy is whatever
 * convimg assigned them this build; check icon/xlibc.h if that range matters again.
 * 0x00 stays black and 0xFF stays white; 0x10 is the default blue. */
/* ---------- Types ---------- */

enum location { LOC_CALC, LOC_PART, LOC_DRIVE };
enum kind { KIND_USB, KIND_PARENT, KIND_PART, KIND_DIR, KIND_FILE, KIND_VAR, KIND_APP };
enum storage_filter { STORE_ALL, STORE_ARCHIVE, STORE_RAM, STORE_COUNT };
enum type_filter { TYPE_ALL, TYPE_APPVAR, TYPE_PRGM, TYPE_VAR, TYPE_APP, TYPE_COUNT };

/* Which icon goes on top of the base file icon. */
enum overlay { OV_NONE, OV_APPVAR, OV_PRGM, OV_APP, OV_VAR, OV_TEXT, OV_HTML, OV_IMAGE, OV_GIF };

typedef struct {
    char name[16];      /* the real name, used for every file operation */
    char shown[16];     /* calculator variables: the name as drawn and searched */
    bool hidden;        /* calculator programs hidden by the XOR 0x64 trick */
    uint8_t kind;
    uint8_t type;       /* OS type for calculator variables; partition index for KIND_PART */
    uint8_t overlay;    /* enum overlay: icon drawn on top of the file icon */
    bool archived;      /* calculator variables only */
    uint8_t contents;   /* folders only: 0 empty, 1 holds a file, 2 holds folders only */
    uint32_t app_addr;  /* apps only: flash address of the app header (its icon is read from there) */
} item_t;



/* ---- shared types ---- */
typedef struct {
    char name[13];
    bool is_dir;
} kid_t;
typedef struct {
    const char *label;
    const char *const *opts;
    uint8_t count;
    uint8_t *val;
} filter_row_t;
enum menu_kind { MENU_NONE, MENU_FILES, MENU_ACTIONS };

/* ---- shared functions ---- */
bool contains_ci(const char *hay, const char *needle);
bool copy_folder(const char *src_path, const char *dst_dir, const char *name);
bool delete_tree(const char *path);
bool dir_is_empty(const char *path);
bool ext_equals(const char *dot, const char *ext);
bool inside_folder(const char *dst, const char *src);
bool is_archived(const char *name, uint8_t type);
bool is_prgm_type(uint8_t type);
bool key_repeats(uint8_t key);
bool letter_ok(uint8_t c);
bool move_and_flush(const char *src, const char *dst_dir, const char *name);
bool move_file(const char *src_path, const char *dst_dir, const char *name, bool remove_src);
bool name_char_ok(uint8_t c);
bool open_partition(int i);
bool path_exists(const char *path);
bool read_basic_icon(const item_t *it);
bool read_prgm_icon(const item_t *it);
bool rename_name_ok(const char *s);
bool row_is_marked(const item_t *it);
bool selected_drive_file_size(uint32_t *size);
bool selected_size(uint32_t *size);
bool still_on_drive(const char *path);
bool storage_matches(bool archived);
bool theme_save(void);
bool type_matches(uint8_t type);
bool usb_matches(enum kind k, const char *name);
char editor_char(uint8_t key);
char rename_char(uint8_t key);
const char * item_right_label(const item_t *it);
const char * menu_item(int i);
const char * overlay_label(uint8_t ov);
fat_error_t remove_entry(const char *path);
int find_icon_header(const uint8_t *buf, int n);
int list_dir(const char *path, kid_t **out);
int main(void);
int menu_count(void);
int name_cmp(const char *a, const char *b);
int settings_digit(uint8_t key);
int sort_rank(uint8_t c);
uint24_t read_cb(void *usr, uint32_t lba, uint24_t count, void *buf);
uint24_t write_cb(void *usr, uint32_t lba, uint24_t count, const void *buf);
uint8_t basic_color(uint8_t ch);
uint8_t filter_rows_build(filter_row_t *rows);
uint8_t folder_contents(const char *path);
uint8_t overlay_for_filename(const char *name);
uint8_t overlay_for_os_type(uint8_t type);
usb_error_t usb_handler(usb_event_t event, void *event_data, void *callback_data);
void activate_selected(void);
void apply_search(void);
void battery_poll(void);
void calc_delete_prompt(void);
void calc_refresh(void);
void cancel_delete(void);
void clamp_selection(void);
void clear_search(void);
void leave_search(void);
void clear_status_message(void);
void close_menu(void);
void confirm_delete(void);
void copy_marked(void);
void draw(void);
void draw_body(void);
void draw_battery(void);
void draw_file_with_overlay(uint8_t overlay, int y);
void draw_filter(void);
void draw_in_box(gfx_rletsprite_t *s, int w, int h, int y);
void draw_indicators(void);
void draw_item_icon(const item_t *it, int y);
void draw_key_legend(int y);
void draw_menu(void);
void draw_row(int y, const item_t *it, const char *right, bool sel);
void draw_settings(void);
void draw_viewer_overlay(void);
void drive_go_down(const char *name);
void drive_go_up(void);
void drive_refresh(void);
void drop_marked(void);
void enter_drive(void);
void fat_close_volume(void);
void filter_key(uint8_t key);
void filters_changed(void);
void finish_new_file(void);
void finish_rename(void);
void finish_writes(void);
void flush_add(void);
void flush_lfn(void);
void format_size(uint32_t n, char *out);
void go_up_level(void);
void make_path(char *out, const char *folder, const char *name);
void mark_selected(void);
void menu_key(uint8_t key);
void menu_run(void);
void mount_device(usb_device_t dev);
void move_folder(const char *src_path, const char *dst_dir);
void move_selection(int delta);
void new_folder(void);
void open_file(const item_t *it);
void open_menu(enum menu_kind kind);
void page_selection(int step);
void part_refresh(void);
void perform_calc_delete(void);
void perform_delete(const char *path, bool is_dir);
void read_part_labels(void);
void refresh_current(void);
void rename_key(uint8_t key, char typed);
void reset_usb_filters(void);
void reset_view_position(void);
void set_status(const char *s);
void settings_begin(void);
void settings_close(bool save);
void settings_follow(void);
void settings_key(uint8_t key);
void shorten_path(char *out, size_t outsz, const char *full);
void show_now(const char *msg);
void shown_name(const char *raw, uint8_t type, char *out, size_t sz, bool *hidden);
void sort_items(bool desc, bool use_shown);
void start_edit(void);
void start_new_file(void);
void start_rename(void);
void start_calc_rename(void);
void finish_calc_rename(void);
bool calc_name_ok(const char *s);
void strip_trailing_slash(char *s);
void text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void text_clip(int x, int y, const char *s, uint8_t fg, uint8_t bg, int maxw);
void theme_find_defaults(void);
void theme_load(void);
void theme_apply(void);
void theme_snapshot(void);
void toggle_usb_hidden(void);
void send_to_calc(void);
void export_begin(void);
void icon_cache_clear(void);
void export_here(void);
void export_cancel(void);
bool export_picking(void);
bool xfer_pending(void);
void xfer_confirm(void);
void xfer_cancel(void);
void type_letter(char c);
void unmount_device(void);
void unmount_usb(void);
void usb_fail(const char *msg);
void zoom_delete(void);

/* ---- shared variables ---- */
extern bool add_pending;
extern bool batt_charging;
extern bool copy_cancelled;
extern bool fat_open;
extern bool filter_drop;
extern bool filter_open;
extern bool icon_transparent;
extern bool lfn_pending;
extern bool mark_is_dir;
extern bool mark_set;
extern bool menu_open;
extern bool mounted;
extern bool msd_open;
extern bool msd_ready;
extern bool needs_redraw;
extern bool new_file_mode;
extern bool pending_calc;
extern bool pending_delete;
extern bool pending_is_dir;
extern bool rename_mode;
extern bool calc_rename;
extern char calc_rename_old[9];
extern uint8_t calc_rename_type;
extern bool calc_rename_hidden;
extern bool search_mode;
extern bool settings_fast;
extern bool settings_open;
extern bool settings_typing;
extern bool status_changed;
extern bool status_timed;
extern bool type_forced;
extern bool write_ok;
extern char *add_dir;
extern char *add_long;
extern char add_short[13];
extern char drive_path[PATH_MAX];
extern char hide_label[8];
extern char mark_name[16];
extern char *mark_path;
extern char part_label[MAX_PARTS][12];
extern char *pending_path;
extern char pending_var[9];
extern char rename_buf[13];
extern char *rename_src;
extern char search[SEARCH_MAX];
extern char status[40];
extern char usb_label[16];
extern const char * actions_items[4];
extern const char * files_items[3];
extern const uint8_t THEME_DEFAULT_RGB[ROLE_HIDDEN][3];
extern const uint8_t THEME_ROLE_SLOT[THEME_ROLES];
extern const uint8_t basic_palette[16];
extern enum location location;
extern enum menu_kind menu_kind;
extern fat_t fat;
extern int cur_part;
extern int item_count;
extern int menu_sel;
extern int part_count;
extern int selected;
extern int top;
extern item_t *items;
extern lfn_list_t lfn_list;
extern msd_t msd;
extern uint16_t *palette256;
extern uint16_t ti_pal[16];
extern uint32_t part_lba[MAX_PARTS];
extern uint32_t status_at;
extern uint32_t writes_blocked;
extern uint32_t writes_done;
extern uint8_t basic_head[BASIC_SCAN];
extern uint8_t basic_pix[BASIC_PIXELS];
extern uint8_t batt_level;
extern uint8_t *copy_buf;
extern uint8_t filter_drop_sel;
extern uint8_t filter_sel;
extern uint8_t hidden_filter;
extern uint8_t icon_sprite[2 + ICON_PIXELS];
extern uint8_t pending_var_type;
extern uint8_t settings_num;
extern uint8_t settings_sel;
extern uint8_t settings_top;
extern uint8_t sort_filter;
extern uint8_t store_filter;
extern uint8_t theme_default[THEME_ROLES];
extern uint8_t theme_idx[THEME_ROLES];
extern uint8_t theme_saved[THEME_ROLES];
extern uint8_t type_before;
extern uint8_t type_filter;
extern uint8_t usb_ext;
extern uint8_t usb_hidden;
extern uint8_t usb_kind;
extern uint8_t usb_sort;

#endif
