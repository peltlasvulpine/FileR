/* Text editor for .txt files on the USB drive.
 *
 * The whole file is held in RAM while it is edited, up to EDIT_MAX bytes.
 * Keys: letters and digits type, Enter starts a new line, Del deletes the
 * character before the cursor, the arrows move the cursor (Up/Down by line),
 * alpha or alpha lock + Up/Down move a page, Trace jumps to a line or
 * percentage (Zoom switches), Graph saves, and Clear leaves.
 * Clear with unsaved changes asks once: press Clear again to throw the changes away.
 *
 * Usage from main.c:
 *   editor_can_open(name)              - true for .txt names
 *   editor_open(&fat, path, is_new)    - edit a file (is_new: it is empty, just created)
 *   editor_is_open()                   - true while the editor is on screen
 *   editor_key(key, typed, page, second) - handle one key press; page is true for alpha + Up/Down,
 *                                        second is true for 2nd + Left/Right (line start/end)
 *                                        (see enum edit_result)
 *   editor_draw()                      - draw the editor screen
 *   editor_close()                     - leave the editor (e.g. drive unplugged)
 *   editor_error()                     - why the last editor_open failed
 */
#ifndef TEXT_EDITOR_H
#define TEXT_EDITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <fatdrvce.h>

enum edit_result {
    EDIT_NONE,      /* nothing to do outside the editor */
    EDIT_SAVED,     /* the file was written; the caller should flush the drive */
    EDIT_CLOSED     /* the editor closed */
};

bool editor_can_open(const char *name);
bool editor_open(fat_t *fat, const char *fpath, bool is_new);
bool editor_is_open(void);
bool editor_is_dirty(void);
/* Writes a copy of unsaved text as AUTOSAV1.TXT (or the next free number, up to 9).
 * The drive must allow writes (main.c sets write_ok). name_out gets the file name. */
bool editor_autosave(char *name_out, size_t size);
enum edit_result editor_key(uint8_t key, char typed, bool page, bool second);
void editor_draw(void);
/* Makes the next editor_draw() redraw the whole screen (for example after the color
 * menu opens or closes, since the screen is reused from frame to frame). */
void editor_force_full(void);
/* Call from the main loop. Clears a status message once its time is up; returns true
 * when the screen needs a redraw. */
bool editor_tick(void);
void editor_close(void);
/* A function the editor calls after drawing and before showing the screen (battery, modifier
 * icons, and the color menu). Set it once, from main.c. */
void editor_set_overlay(void (*fn)(void));
const char *editor_error(void);

#endif /* TEXT_EDITOR_H */
