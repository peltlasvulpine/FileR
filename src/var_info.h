#ifndef VAR_INFO_H
#define VAR_INFO_H

#include <stdbool.h>
#include <stdint.h>

/* Info panel for a calculator variable, drawn on the right of the list.
 * Enter closes it; 2nd toggles the archive status. */

bool var_info_is_open(void);
void var_info_open(const char *name, uint8_t type);
/* Handles one key while the panel is open. Returns true if the list needs a refresh. */
bool var_info_key(uint8_t key);
void var_info_draw(void);

#endif
