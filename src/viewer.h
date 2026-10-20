/* Text viewer for .txt files on the USB drive.
 *
 * Usage from main.c:
 *   viewer_can_open(name)            - true for .txt names
 *   viewer_open(&fat, path, name)    - load a file and show it
 *   viewer_is_open()                 - true while the viewer is on screen
 *   viewer_key(key)                  - handle a key; returns true if redraw needed
 *   viewer_draw()                    - draw the viewer screen
 *   viewer_close()                   - leave the viewer (e.g. drive unplugged)
 */
#ifndef VIEWER_H
#define VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include <fatdrvce.h>

bool viewer_can_open(const char *name);
bool viewer_open(fat_t *fat, const char *fpath, const char *name);
bool viewer_is_open(void);
bool viewer_key(uint8_t key);
void viewer_draw(void);
void viewer_close(void);

#endif /* VIEWER_H */
