/* GIF viewer for .gif files on the USB drive.
 *
 * Every frame is decoded once when the GIF opens and kept in RAM, run-length
 * encoded (a run of pixels with the same color is stored as a count and the
 * color). Playback only expands and draws the frames. If the frames don't fit
 * in memory, a few decoded frames are kept ready in a small ring instead.
 * Nothing is written to the drive, and everything is freed on close.
 *
 * Usage from main.c:
 *   gif_viewer_can_open(name)       - true for .gif names
 *   gif_viewer_open(&fat, path)     - load and start the animation
 *   gif_viewer_is_open()            - true while the GIF is on screen
 *   gif_viewer_key(key)             - returns true when the GIF closed
 *   gif_viewer_tick()               - call often: moves to the next frame when due
 *   gif_viewer_error()              - why the last open failed
 *   gif_viewer_close()              - leave the GIF (e.g. drive unplugged)
 */
#ifndef GIF_VIEWER_H
#define GIF_VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include <fatdrvce.h>

bool gif_viewer_can_open(const char *name);
bool gif_viewer_open(fat_t *fat, const char *fpath);
bool gif_viewer_is_open(void);
bool gif_viewer_key(uint8_t key);
void gif_viewer_tick(void);
const char *gif_viewer_error(void);
void gif_viewer_close(void);

#endif /* GIF_VIEWER_H */
