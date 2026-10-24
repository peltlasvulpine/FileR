#ifndef VAR_INFO_H
#define VAR_INFO_H

#include <stdbool.h>
#include <stdint.h>

/* Info panel, drawn on the right of the list. Window closes it.
 * For a calculator variable, 2nd toggles the archive status.
 * For the USB drive row, the panel shows the drive; 2nd is handled by the caller (unmount). */

bool var_info_is_open(void);
bool var_info_is_usb(void);
void var_info_close(void);
void var_info_open(const char *name, uint8_t type);
void var_info_open_usb(int parts, bool mounted);
/* Handles one key while the panel is open. Returns true if the list needs a refresh. */
bool var_info_key(uint8_t key);
void var_info_draw(void);

#endif
