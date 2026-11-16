#ifndef VAR_INFO_H
#define VAR_INFO_H

#include <stdbool.h>
#include <stdint.h>

/* Info panel, drawn on the right of the list. Window (or Clear) applies the choices and
 * closes it. Y= renames the variable (the caller applies the choices first).
 * For a calculator variable: Zoom toggles archive/RAM. For a TI-BASIC program: Trace
 * toggles lock/unlock and Graph toggles hidden/visible. A compiled program is always
 * locked, so Trace does nothing for it. Changes are pending until the panel closes.
 * For the USB drive row, the panel shows the drive; 2nd is handled by the caller (unmount). */

bool var_info_is_open(void);
bool var_info_is_usb(void);
/* True when the panel is open on a program (Trace and Graph do something). */
bool var_info_is_prgm(void);
/* Applies the pending choices and closes the panel. Returns true if the list changed. */
bool var_info_apply(void);
void var_info_close(void);
/* name is the real name (file operations use it); shown is the name as the list draws it. */
void var_info_open(const char *name, uint8_t type, const char *shown, bool hidden);
void var_info_open_usb(int parts, bool mounted);
/* Handles one key while the panel is open. Returns true if the list needs a refresh. */
bool var_info_key(uint8_t key);
/* Fills the five bottom-bar labels (Y=, Window, Zoom, Trace, Graph) for the open panel. */
void var_info_labels(const char **lab);
void var_info_draw(void);

#endif
