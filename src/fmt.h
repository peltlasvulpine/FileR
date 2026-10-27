/* A small replacement for sprintf. It only handles the formats FileR uses:
 * %s, %d, %u, %lu (with an optional width such as %3d), %02X and %%.
 * The C library's printf is large on the calculator, so FileR avoids it. */
#ifndef FMT_H
#define FMT_H

/* Writes the formatted text to out (no size check: callers pass big enough buffers). */
void fmt(char *out, const char *format, ...);

#endif /* FMT_H */
