/* Image viewer for .bmp files on the USB drive.
 *
 * Supports uncompressed BMP files, 8-bit (palette) or 24-bit, with a
 * 40-byte header and at most 640 pixels wide. Images bigger than the screen
 * are shrunk by picking every Nth pixel. Colors are reduced to 256 colors
 * (3 bits red, 3 green, 2 blue) and the calculator palette is replaced while
 * the image is on screen. main.c restores it when the image closes.
 *
 * Usage from main.c:
 *   image_can_open(name)            - true for .bmp names
 *   image_open(&fat, path, name)    - draw the image once
 *   image_is_open()                 - true while the image is on screen
 *   image_key(key)                  - returns true when the image closed
 *   image_close()                   - leave the image (e.g. drive unplugged)
 */
#ifndef IMAGE_VIEWER_H
#define IMAGE_VIEWER_H

#include <stdbool.h>
#include <stdint.h>
#include <fatdrvce.h>

bool image_can_open(const char *name);
bool image_open(fat_t *fat, const char *fpath, const char *name);
bool image_is_open(void);
bool image_key(uint8_t key);
void image_close(void);

#endif /* IMAGE_VIEWER_H */
