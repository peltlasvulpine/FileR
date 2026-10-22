#ifndef KEYS_H
#define KEYS_H

#include <stdbool.h>
#include <stdint.h>

/* Global 2nd (shift) and alpha modifiers, usable from any module.
 *  - 2nd toggles shift; any key except alpha clears it.
 *  - alpha toggles alpha; any key except 2nd clears it.
 *  - 2nd then alpha sets alpha lock; alpha stays on until alpha is pressed again. */

/* Call once per key press, after reading the key and before acting on it. */
void keys_process(uint8_t key);
/* Letter for this key while alpha or alpha lock is on, otherwise 0. Call before keys_process. */
char keys_letter(uint8_t key);
/* Letter for this key with no modifiers on (used by search mode). */
char keys_char(uint8_t key);
bool keys_alpha_active(void);
/* Draws the active modifier icons just left of the battery at battery_x, row y. */
void keys_draw(int battery_x, int y);

#endif
