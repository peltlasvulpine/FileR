#include "filer.h"

/* ---------- Input ---------- */

void open_menu(enum menu_kind kind) {
    menu_kind = kind;
    menu_sel = 0;
    menu_open = true;
    needs_redraw = true;
}

void close_menu(void) {
    menu_open = false;
    needs_redraw = true;
}

/* Runs the item the menu has selected, then closes the menu. */
/* Sets or clears the hidden attribute of the selected USB file or folder. The
 * attribute byte is read first, so only the hidden bit changes. */
void toggle_usb_hidden(void) {
    item_t *it;
    char name[16];
    char path[PATH_MAX + 16];
    char msg[40];
    bool want_hidden;

    if (location != LOC_DRIVE || item_count == 0 || !mounted) return;
    it = &items[selected];
    if (it->kind != KIND_FILE && it->kind != KIND_DIR) {
        set_status("Select a file or folder");
        return;
    }
    strncpy(name, it->name, sizeof(name) - 1);   /* folder names carry a trailing "/" */
    name[sizeof(name) - 1] = 0;
    strip_trailing_slash(name);
    if (strcmp(name, "..") == 0) {
        set_status("Select a file or folder");
        return;
    }
    make_path(path, drive_path, name);

    want_hidden = !it->hidden;
    /* Raw edit of the short entry, like rename: the driver has nothing pending first. The
     * FAT driver's attribute calls are not used, since it can't find long-named entries. */
    finish_writes();
    if (lfn_set_attrib(&msd, part_lba[cur_part], path,
                       want_hidden ? FAT_HIDDEN : 0, want_hidden ? 0 : FAT_HIDDEN)) {
        fmt(msg, "%s %s", want_hidden ? "Hidden:" : "Shown:", name);
        set_status(msg);
    } else {
        set_status("Could not change the entry");
    }
    drive_refresh();
}

void menu_run(void) {
    enum menu_kind kind = menu_kind;
    int sel = menu_sel;

    close_menu();
    if (kind == MENU_FILES) {
        if (sel == 0) new_folder();
        else if (sel == 1) zoom_delete();
        else start_new_file();
    } else {
        if (sel == 0) {
            start_rename();
        } else if (sel == 1) {
            if (mark_set) drop_marked();
            else set_status("Mark a file first");
        } else if (sel == 2) {
            if (mark_set) copy_marked();
            else set_status("Mark a file first");
        } else if (sel == 3) {
            start_edit();
        } else if (sel == 4) {
            toggle_usb_hidden();
        } else {
            send_to_calc();
        }
    }
}

/* Keys while a menu is open: Up and Down move, Enter runs, Clear or Del closes. */
void menu_key(uint8_t key) {
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
void go_up_level(void) {
    leave_search();            /* moving up a level turns search off */
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
void clear_status_message(void) {
    status[0] = 0;
    status_timed = false;
    status_changed = true;
    pending_delete = false;   /* a pending delete is dropped with its message */
}

/* Alpha + Up/Down: moves one screen of rows (no wrap). */
void page_selection(int step) {
    clear_status_message();
    if (item_count == 0) return;
    selected += step * VISIBLE_ROWS;
    if (selected < 0) selected = 0;
    if (selected > item_count - 1) selected = item_count - 1;
    clamp_selection();
    needs_redraw = true;
}

/* Up at the top wraps to the bottom; Down at the bottom wraps to the top. */
void move_selection(int delta) {
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

/* ---------- Main ---------- */

/* Editor auto-repeat: a held arrow or Del repeats after REPEAT_DELAY_MS, then every REPEAT_MS. */

bool key_repeats(uint8_t key) {
    return key == sk_Up || key == sk_Down || key == sk_Left || key == sk_Right || key == sk_Del;
}

/* Bisect checkpoints: each shows its number and waits for a key. Kept at 0 for normal use. */
#define BISECT 0
#if BISECT
#define STAGE(n) do { gfx_FillScreen(0); gfx_SetTextFGColor(255); gfx_PrintStringXY("stage " #n, 10, 10); gfx_SwapDraw(); while (!os_GetCSC()) {} } while (0)
#else
#define STAGE(n)
#endif

/* TEMPORARY: 1 makes FileR return at once, before any of its own code runs. */
#define EXIT_FIRST 0

int main(void) {
#if EXIT_FIRST
    return 0;
#endif
    uint8_t key;
    bool quit = false;
    uint8_t last_key = 0;
    bool pressed;
    bool typing_now = false;   /* search was being typed when this key went down */
    bool page_key = false;     /* alpha + Up/Down, on the frame the key went down */
    bool repeat_press = false; /* this press is an auto-repeat of a held editor key */
    bool editor_menu_was = false;   /* the color menu was open when the editor last drew */
    bool second_key = false;        /* 2nd was on for this press (read before keys_process) */
    bool viewer_menu_was = false;   /* the color menu was open when the viewer last drew */
    uint32_t repeat_at = 0;    /* clock() value when a held editor key next repeats */
    char letter;
    char typed = 0;            /* character typed by this key while renaming */
    bool last_ready = false;
    bool second_held = false;  /* 2nd is on (tapped or held), read before this key changes it */

    gfx_Begin();
    {
        const char *bad = mem_init();   /* the big buffers come from the heap */
        if (bad) {                      /* stop cleanly, and say which part did not fit */
            char msg[48];
            gfx_FillScreen(0);
            gfx_SetTextFGColor(255);
            gfx_SetTextBGColor(0);
            gfx_SetTextTransparentColor(0);
            fmt(msg, "Not enough memory: %s", bad);
            gfx_PrintStringXY("FileR can't start.", 10, 40);
            gfx_PrintStringXY(msg, 10, 60);
            gfx_PrintStringXY("Press any key to exit.", 10, 80);
            gfx_SwapDraw();
            while (kb_AnyKey()) {}      /* let go of the key that started FileR */
            while (!os_GetCSC()) {}     /* then wait for a new press */
            gfx_End();
            return 0;
        }
    }
    STAGE(1);
    theme_snapshot();          /* before any palette change */
    theme_find_defaults();
    status_at = (uint32_t)clock();
    status_timed = true;
    if (cache_startup()) set_status("Last run ended early; cache cleared");
    {
        int i;
        for (i = 0; i < 16; i++) ti_pal[i] = gfx_palette[basic_palette[i]];  /* before FileR overwrites it */
    }
    STAGE(2);
    theme_load();
    STAGE(3);
    viewer_set_overlay(draw_viewer_overlay);
    editor_set_overlay(draw_viewer_overlay);
    restore_palette();
    gfx_SetDrawBuffer();
    STAGE(4);

    battery_poll();
    STAGE(5);
    usb_Init(usb_handler, NULL, NULL, USB_DEFAULT_INIT_FLAGS);
    STAGE(6);
    calc_refresh();
    STAGE(7);

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
        if (!msd_ready && editor_is_open()) {
            editor_close();
            needs_redraw = true;
        }
        if (!msd_ready && gif_viewer_is_open()) {
            gif_viewer_close();
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

        /* While a GIF plays, step it in a tight loop until a key is pressed. The rest
         * of this loop takes too long per pass to keep the frames on time. */
        if (gif_viewer_is_open()) {
            while (gif_viewer_is_open() && !kb_AnyKey()) gif_viewer_tick();
        }
        key = os_GetCSC();
        /* 2nd is sticky after a tap, so either the modifier or the key being held counts. */
        second_held = keys_2nd_active() || kb_IsDown(kb_Key2nd);
        /* 2nd + Mode exits. Mode alone opens the color menu, so this check must come first. */
        if (key == sk_Mode && second_held && !rename_mode && !search_mode) break;
        /* A key counts once per press. Holding it down doesn't run the modifiers again. */
        pressed = key != 0 && key != last_key;
        /* The editor repeats arrows and Del while they are held. The other screens take one press per key. */
        repeat_press = false;
        if (editor_is_open() && key != 0 && key == last_key && key_repeats(key)) {
            if ((uint32_t)clock() >= repeat_at) {
                pressed = true;
                repeat_press = true;
                repeat_at = (uint32_t)clock() + (uint32_t)REPEAT_MS * CLOCKS_PER_SEC / 1000UL;
            }
        } else if (pressed) {
            repeat_at = (uint32_t)clock() + (uint32_t)REPEAT_DELAY_MS * CLOCKS_PER_SEC / 1000UL;
        }
        last_key = key;
        letter = 0;
        typed = 0;
        page_key = false;
        if (pressed) {
            typing_now = search_mode;                       /* only y= feeds the search */
            letter = search_mode ? keys_char(key) : 0;
            typed = rename_mode ? rename_char(key) : (editor_is_open() ? editor_char(key) : 0);   /* before the modifiers change */
            page_key = keys_alpha_active() && (key == sk_Up || key == sk_Down);   /* before the modifiers change */
            /* With the info panel open, 2nd is its archive toggle, not the shift modifier. */
            second_key = keys_2nd_active();          /* the editor's 2nd + Left/Right, read before the modifiers change */
            if (!repeat_press && !(key == sk_2nd && var_info_is_open())) keys_process(key);   /* a repeat is not a new modifier press */
            needs_redraw = true;         /* indicators may have changed */
        }

        if (editor_is_open()) {
            /* Mode opens the color menu here too; while it is open it takes the keys. */
            if (settings_open) {
                if (pressed) settings_key(key);
            } else if (pressed && key == sk_Mode && !second_held) {
                settings_begin();
            } else if (pressed) {
                write_ok = key == sk_Graph;      /* only Graph (save) may write to the drive */
                if (editor_key(key, typed, page_key, second_key) == EDIT_SAVED) {
                    write_ok = false;
                    finish_writes();             /* flush the saved file */
                }
                write_ok = false;
            }
            needs_redraw = true;
        } else if (gif_viewer_is_open()) {
            if (gif_viewer_key(key)) {
                restore_palette();
                needs_redraw = true;
            }
        } else if (image_is_open()) {
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
            } else if (viewer_key(key, page_key)) {
                needs_redraw = true;
            }
        } else if (var_info_is_usb() && pressed && key == sk_2nd) {
            var_info_close();          /* 2nd on the USB panel unmounts the drive */
            unmount_usb();
            needs_redraw = true;
        } else if (var_info_is_open()) {
            /* Keep the selection on the same variable across the refresh, since a hide or
             * lock can move it in the sorted list. */
            char keep[16];
            strncpy(keep, selected < item_count ? items[selected].shown : "", sizeof(keep) - 1);
            keep[sizeof(keep) - 1] = 0;
            if (key == sk_Yequ && pressed && !var_info_is_usb()) {
                /* Y= on a variable's Info panel: apply the pending choices, then rename the
                 * variable (re-finding the selection first, since a hide can move it). */
                if (var_info_apply()) {
                    int k;
                    calc_refresh();
                    for (k = 0; k < item_count; k++) {
                        if (strcmp(items[k].shown, keep) == 0) { selected = k; break; }
                    }
                    clamp_selection();
                }
                start_calc_rename();
                needs_redraw = true;
            } else if (var_info_key(key)) {
                int k;
                calc_refresh();
                for (k = 0; k < item_count; k++) {
                    if (strcmp(items[k].shown, keep) == 0) { selected = k; break; }
                }
                clamp_selection();
            }
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
                    if (export_picking() && location == LOC_PART) {   /* on partition select, Clear cancels the export */
                        export_cancel();
                    } else if (typing_now) {
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
                    } else if (xfer_pending()) {
                        xfer_confirm();
                    } else {
                        search_mode = !search_mode;
                        set_status(search_mode ? "Search on" : "Search off");
                    }
                    break;
                case sk_Window:
                    if (location == LOC_CALC) {
                        if (item_count > 0 && items[selected].kind == KIND_VAR) {
                            var_info_open(items[selected].name, items[selected].type,
                                          items[selected].shown, items[selected].hidden);
                        } else if (item_count > 0 && items[selected].kind == KIND_USB) {
                            var_info_open_usb(part_count, msd_ready);
                        } else {
                            set_status("Select a calculator variable");
                        }
                    } else if (location == LOC_DRIVE) {
                        open_menu(MENU_FILES);
                    }
                    break;
                case sk_Zoom:
                    if (location == LOC_DRIVE) {
                        open_menu(MENU_ACTIONS);
                    } else {
                        set_status("Unassigned");
                    }
                    break;
                case sk_Vars:
                    if (location == LOC_DRIVE) start_edit();   /* Vars opens the selected .txt in the editor */
                    else set_status("Select a text file");
                    break;
                case sk_Trace:
                    if (export_picking()) export_here();
                    else if (location == LOC_DRIVE) mark_selected();
                    else if (location == LOC_CALC) export_begin();
                    else set_status("Unassigned");
                    break;
                case sk_Graph:
                    if (pending_delete) {
                        cancel_delete();
                    } else if (xfer_pending()) {
                        xfer_cancel();
                    } else if (export_picking()) {
                        export_cancel();
                    } else if (location == LOC_DRIVE || location == LOC_CALC) {
                        filter_open = true;
                        filter_sel = 0;
                        filter_drop = false;
                        needs_redraw = true;
                    } else {
                        set_status("Unassigned");
                    }
                    break;
                case sk_Prgm:
                    if (location == LOC_DRIVE) send_to_calc();          /* USB file -> calc */
                    else set_status("Select a USB file");
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
        if (editor_is_open() && editor_tick()) needs_redraw = true;   /* editor message timed out */
        gif_viewer_tick();                 /* animated GIFs step to their next frame */
        if (needs_redraw) {
            /* Only the color menu changed on the main screen: copy the frame on screen
             * and draw the box over it, instead of redrawing the whole list. */
            bool fast = settings_fast && settings_open && !editor_is_open() && !viewer_is_open()
                        && !image_is_open() && !gif_viewer_is_open();
            settings_fast = false;
            if (fast) {
                gfx_BlitScreen();
                draw_settings();
                gfx_SwapDraw();
            } else if (editor_is_open()) {
                if (settings_open != editor_menu_was) {   /* the color menu came or went: redraw all */
                    editor_force_full();
                    editor_menu_was = settings_open;
                }
                editor_draw();
            }
            else if (viewer_is_open()) {
                if (settings_open != viewer_menu_was) {   /* the color menu came or went: redraw all */
                    viewer_force_full();
                    viewer_menu_was = settings_open;
                }
                viewer_draw();
            }
            else if (!image_is_open() && !gif_viewer_is_open()) draw();   /* pictures are drawn on open */
            needs_redraw = false;
        }
    }

    /* Every way out of the loop (Del, or Clear at the calculator root) lands here.
     * Close any open viewer first, since it holds a file on the drive, then the
     * drive itself. Writes are allowed for the final close so the drive is saved
     * cleanly. */
    show_now("Exiting...");
    if (viewer_is_open()) viewer_close();
    if (editor_is_open()) {
        if (editor_is_dirty()) {            /* leaving with unsaved text: keep a copy */
            char copy[16];
            write_ok = true;
            editor_autosave(copy, sizeof(copy));
            write_ok = false;
            finish_writes();
        }
        editor_close();
    }
    if (gif_viewer_is_open()) gif_viewer_close();
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
    cache_shutdown();          /* deletes FRCACHE and FRSWAP: a clean exit */
    gfx_End();
    return 0;
}

