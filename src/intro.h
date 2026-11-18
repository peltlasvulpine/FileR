#ifndef INTRO_H
#define INTRO_H

/* Plays FileR's startup intro (about 4 to 5 seconds). The title flies in from the right,
 * the subtitle rises from the bottom, slides back out, and the title shrinks into the top
 * bar. A key press after the launching key is let go ends it early. Call once, after the
 * palette is set and before the main loop. */
void intro_run(void);

#endif
