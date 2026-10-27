#include <stdarg.h>
#include <stdbool.h>
#include "fmt.h"

/* Appends the digits of v in base 10 or 16, padded to width with pad. */
static char *put_num(char *o, unsigned long v, bool neg, unsigned width, char pad, unsigned base, bool upper) {
    char t[16];
    unsigned n = 0;

    do {
        unsigned d = (unsigned)(v % base);
        t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    if (neg) t[n++] = '-';
    while (n < width) t[n++] = pad;
    while (n) *o++ = t[--n];
    return o;
}

void fmt(char *out, const char *format, ...) {
    va_list ap;
    char *o = out;

    va_start(ap, format);
    while (*format) {
        unsigned width = 0;
        char pad = ' ';
        bool is_long = false;

        if (*format != '%') {
            *o++ = *format++;
            continue;
        }
        format++;
        if (*format == '%') { *o++ = '%'; format++; continue; }
        if (*format == '0') { pad = '0'; format++; }
        while (*format >= '0' && *format <= '9') width = width * 10 + (unsigned)(*format++ - '0');
        if (*format == 'l') { is_long = true; format++; }
        switch (*format++) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                while (*s) *o++ = *s++;
                break;
            }
            case 'd': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                bool neg = v < 0;
                o = put_num(o, neg ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v, neg, width, pad, 10, false);
                break;
            }
            case 'u':
                o = put_num(o, is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int),
                            false, width, pad, 10, false);
                break;
            case 'X':
                o = put_num(o, (unsigned long)va_arg(ap, unsigned int), false, width, pad, 16, true);
                break;
            default:
                break;
        }
    }
    va_end(ap);
    *o = 0;
}
