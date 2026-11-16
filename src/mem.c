#include <stdlib.h>
#include "mem.h"

/* Calls each module's allocator. All buffers are allocated once, here, before the
 * app starts its screens. Returns NULL on success, or the name of the module whose
 * buffers did not fit in the heap. */
const char *mem_init(void) {
    if (!state_alloc()) return "state";
    if (!palette_alloc()) return "palette";
    if (!draw_alloc()) return "draw";
    if (!lfn_alloc()) return "lfn";
    if (!usb_alloc()) return "usb";
    if (!text_editor_alloc()) return "text editor";
    if (!text_viewer_alloc()) return "text viewer";
    if (!image_viewer_alloc()) return "image viewer";
    if (!gif_viewer_alloc()) return "GIF viewer";   /* also calls gif_alloc */
    return NULL;
}
