#include <tice.h>
#include <fileioc.h>
#include <stdio.h>
#include <string.h>
#include "cache.h"

#define CACHE_NAME "FRCACHE"    /* 7 characters: TI variable names are 8 at most */
#define SWAP_NAME  "FRSWAP"

static uint8_t cache_slot = 0;
static bool cache_is_open = false;

/* Creates the archived marker. Archived variables can't be written, so this runs
 * only after the old marker has been deleted. */
static void swap_mark(void) {
    uint8_t h = ti_OpenVar(SWAP_NAME, "w", OS_TYPE_APPVAR);

    if (!h) return;
    ti_Write("R", 1, 1, h);
    ti_SetArchiveStatus(true, h);
    ti_Close(h);
}

bool cache_startup(void) {
    uint8_t h = ti_OpenVar(SWAP_NAME, "r", OS_TYPE_APPVAR);
    bool crashed = h != 0;

    if (h) ti_Close(h);
    ti_DeleteVar(CACHE_NAME, OS_TYPE_APPVAR);   /* a cache never survives a run */
    ti_DeleteVar(SWAP_NAME, OS_TYPE_APPVAR);
    swap_mark();
    return crashed;
}

void cache_shutdown(void) {
    cache_close();
    ti_DeleteVar(SWAP_NAME, OS_TYPE_APPVAR);
}

bool cache_open(uint16_t bytes) {
    uint8_t zero[64];
    uint16_t done = 0;

    if (cache_is_open) return true;
    cache_slot = ti_OpenVar(CACHE_NAME, "w+", OS_TYPE_APPVAR);
    if (!cache_slot) return false;
    cache_is_open = true;

    /* Fill the whole size up front, so later seeks never go past the end. */
    memset(zero, 0, sizeof(zero));
    while (done < bytes) {
        uint16_t n = bytes - done;
        if (n > sizeof(zero)) n = sizeof(zero);
        if (ti_Write(zero, 1, n, cache_slot) != n) {
            cache_close();
            return false;
        }
        done += n;
    }
    return true;
}

void cache_close(void) {
    if (cache_is_open) ti_Close(cache_slot);
    cache_is_open = false;
    ti_DeleteVar(CACHE_NAME, OS_TYPE_APPVAR);
}

bool cache_read(uint32_t off, void *buf, uint16_t n) {
    if (!cache_is_open) return false;
    if (ti_Seek((int)off, SEEK_SET, cache_slot)) return false;
    return ti_Read(buf, 1, n, cache_slot) == n;
}

bool cache_write(uint32_t off, const void *buf, uint16_t n) {
    if (!cache_is_open) return false;
    if (ti_Seek((int)off, SEEK_SET, cache_slot)) return false;
    return ti_Write(buf, 1, n, cache_slot) == n;
}
