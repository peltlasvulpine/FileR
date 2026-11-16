/* FRCACHE: FileR's big working tables, kept in an unarchived AppVar in the calculator's
 * user RAM instead of in FileR's own static memory. FRSWAP is a tiny archived marker
 * that exists while FileR runs, so a run that ended without cleaning up can be detected. */
#ifndef FILER_CACHE_H
#define FILER_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/* Call at startup. Clears any old cache and marks this run as in progress.
 * Returns true if the previous run never reached cache_shutdown(). */
bool cache_startup(void);

/* Call at exit. Deletes the cache and the marker. */
void cache_shutdown(void);

/* Creates the cache with the given size, zero-filled. False if it can't be created. */
bool cache_open(uint16_t bytes);

/* Closes and deletes the cache. Safe to call when it isn't open. */
void cache_close(void);

/* Reads or writes n bytes at offset off. False on failure. */
bool cache_read(uint32_t off, void *buf, uint16_t n);
bool cache_write(uint32_t off, const void *buf, uint16_t n);

#endif /* FILER_CACHE_H */
