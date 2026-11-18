#ifndef SAFE_ARC_H
#define SAFE_ARC_H

#include <stdbool.h>
#include <stdint.h>

/* Moves a variable into the archive without letting the OS garbage-collect prompt show on
 * FileR's own screen. If the archive has room, it is archived directly. If not, FileR
 * clears the screen, archives it from a clean state, and relaunches itself, so the
 * function does not return in that case. Returns the archive call's result otherwise.
 * Ported from CEAShell: see safe_arc.c. */
bool safe_set_archived(const char *name, uint8_t type, bool to_arc);

/* An operation that touches calculator memory, for safe_run_or_relaunch. */
typedef int (*safe_op_fn)(void *ctx);

/* True if a new variable of size bytes can be made without the garbage-collect prompt
 * (free RAM, and archive room too when archived is true). Always true inside an op that
 * safe_run_or_relaunch is running. */
bool safe_var_fits(uint32_t size, bool archived);

/* Runs op(ctx) from a clean screen and relaunches FileR, so a garbage-collect prompt does
 * not show on FileR's own screen. Does not return when the relaunch works. Otherwise it
 * runs op here and returns its result. Use it only when safe_var_fits said no. */
int safe_run_or_relaunch(safe_op_fn op, void *ctx);

#endif
