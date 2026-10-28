/* GIF decoder. Decodes one frame at a time onto a canvas of palette indexes,
 * so the picture can be animated. Only the first GIF in a file is read.
 * Frames bigger than the canvas limits, and files over the size limit, are refused.
 *
 *   gif_load(data, len)   - parse the file (data stays owned by the caller)
 *   gif_next()            - decode the next frame; wraps to the first frame at the end
 *   gif_canvas()          - the canvas: gif_width() * gif_height() palette indexes
 *   gif_palette_rgb()     - 3 bytes (R, G, B) per index for the current frame
 *   gif_palette_count()   - how many palette entries the current frame uses
 *   gif_delay_cs()        - how long the last frame stays up, in hundredths of a second
 *   gif_frame_count()     - frames in the file
 *   gif_unload()          - forget the file
 */
#ifndef GIF_H
#define GIF_H

#include <stdbool.h>
#include <stdint.h>

#define GIF_MAX_W 160
#define GIF_MAX_H 160

bool gif_load(const uint8_t *data, uint32_t len);
bool gif_next(void);
void gif_unload(void);
uint16_t gif_width(void);
uint16_t gif_height(void);
const uint8_t *gif_canvas(void);
const uint8_t *gif_palette_rgb(void);
uint16_t gif_palette_count(void);
uint32_t gif_delay_cs(void);
uint32_t gif_frame_count(void);
const char *gif_error(void);      /* why the last load or frame failed */

#endif /* GIF_H */
