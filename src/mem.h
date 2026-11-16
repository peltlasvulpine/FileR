/* Startup allocation. FileR keeps its large buffers on the heap, not in static
 * RAM. A program with a big static RAM section fails to start on the calculator,
 * so each module allocates its own buffers here, once, from mem_init. */
#ifndef MEM_H
#define MEM_H

#include <stdbool.h>

/* Allocates every buffer. Returns NULL on success, or the name of the module whose
 * buffers did not fit in the heap; the caller must then stop instead of running. */
const char *mem_init(void);

/* One allocator per module, each defined in that module's .c file. */
bool state_alloc(void);
bool text_editor_alloc(void);
bool text_viewer_alloc(void);
bool image_viewer_alloc(void);
bool gif_viewer_alloc(void);
bool gif_alloc(void);
bool lfn_alloc(void);
bool usb_alloc(void);
bool draw_alloc(void);
bool palette_alloc(void);

#endif
