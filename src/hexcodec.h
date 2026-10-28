/* hexcodec.h - UTF-16 <-> hex text, used by the voice-host protocol so every message is one ASCII line. */
#ifndef HEXCODEC_H
#define HEXCODEC_H
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* malloc'd, NUL-terminated hex of the UTF-16 code units (4 hex digits each). */
static __attribute__((unused)) char *hex_encode(const wchar_t *w)
{
    size_t n = wcslen(w);
    char *o = (char *)malloc(n * 4 + 1);
    if (!o) return NULL;
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        unsigned v = (unsigned)(w[i] & 0xFFFF);
        o[i * 4] = H[(v >> 12) & 15]; o[i * 4 + 1] = H[(v >> 8) & 15];
        o[i * 4 + 2] = H[(v >> 4) & 15]; o[i * 4 + 3] = H[v & 15];
    }
    o[n * 4] = 0;
    return o;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes `len` hex chars (multiple of 4) into a malloc'd wide string; NULL on bad input. */
static __attribute__((unused)) wchar_t *hex_decode(const char *s, size_t len)
{
    if (len % 4) return NULL;
    wchar_t *o = (wchar_t *)malloc((len / 4 + 1) * sizeof(wchar_t));
    if (!o) return NULL;
    for (size_t i = 0; i < len / 4; i++) {
        int a = hex_digit(s[i * 4]), b = hex_digit(s[i * 4 + 1]), c = hex_digit(s[i * 4 + 2]), d = hex_digit(s[i * 4 + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) { free(o); return NULL; }
        o[i] = (wchar_t)((a << 12) | (b << 8) | (c << 4) | d);
    }
    o[len / 4] = 0;
    return o;
}
#endif
