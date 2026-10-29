/* Text viewer for .txt files on the USB drive.
 *
 * Usage from main.c:
 *   viewer_can_open(name)            - true for .txt, .html and .htm names
 *   viewer_open(&fat, path, name)    - load a file and show it
 *   viewer_is_open()                 - true while the viewer is on screen
 *   viewer_key(key)                  - handle a key; returns true if redraw needed
 *   viewer_draw()                    - draw the viewer screen
 *   viewer_close()                   - leave the viewer (e.g. drive unplugged)
 */
#ifndef TEXT_VIEWER_H
#define TEXT_VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include <fatdrvce.h>

bool viewer_can_open(const char *name);
bool viewer_open(fat_t *fat, const char *fpath, const char *name);
bool viewer_is_open(void);
bool viewer_key(uint8_t key);
void viewer_draw(void);
/* Makes the next viewer_draw() redraw the whole screen (for example after the color
 * menu opens or closes, since the screen is reused from frame to frame). */
void viewer_force_full(void);
void viewer_close(void);
/* A function the viewer calls after drawing its screen and before it shows it,
 * so main.c can add the battery and modifier icons. */
void viewer_set_overlay(void (*fn)(void));

#endif /* TEXT_VIEWER_H */
