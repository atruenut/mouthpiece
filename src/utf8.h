/* utf8.h - UTF-8 <-> wchar_t (UTF-16 or UTF-32 depending on platform). Pure C. */
#ifndef UTF8_H
#define UTF8_H
#include <stddef.h>
#include <wchar.h>
/* Both return the output size; call with out == NULL to measure. Invalid input
 * becomes U+FFFD. utf8_from_wide: out size in bytes (no terminator written). */
size_t utf8_from_wide(const wchar_t *w, size_t n, char *out);
size_t wide_from_utf8(const char *s, size_t n, wchar_t *out);
#endif
