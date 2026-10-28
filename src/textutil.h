/* textutil.h - portable, locale-independent character classes shared by the
 * pure-C core (doc.c, preproc.c). iswlower() & co. differ between C runtimes
 * and locales; segmentation must behave identically everywhere. */
#ifndef TEXTUTIL_H
#define TEXTUTIL_H
#include <wchar.h>

/* Covers ASCII, Latin-1/Extended-A, Greek,
 * Cyrillic. Anything else is "not cased". */
static inline int is_space(wchar_t c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000;
}
static inline int is_digit(wchar_t c) { return c >= '0' && c <= '9'; }
static inline int is_lower(wchar_t c)
{
    if (c >= 'a' && c <= 'z') return 1;
    if (c < 0x80) return 0;
    if (c >= 0xDF && c <= 0xFF) return c != 0xF7;
    if (c >= 0x100 && c <= 0x17F) return (c & 1) ? 1 : 0;
    if (c >= 0x3B1 && c <= 0x3C9) return 1;
    if (c >= 0x430 && c <= 0x45F) return 1;
    return 0;
}
static inline int is_upper(wchar_t c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c < 0x80) return 0;
    if (c >= 0xC0 && c <= 0xDE) return c != 0xD7;
    if (c >= 0x100 && c <= 0x17F) return (c & 1) ? 0 : 1;
    if (c >= 0x391 && c <= 0x3A9) return 1;
    if (c >= 0x400 && c <= 0x42F) return 1;
    return 0;
}
static inline int is_alpha(wchar_t c) { return is_lower(c) || is_upper(c); }
static inline int is_term(wchar_t c) { return c == '.' || c == '!' || c == '?' || c == 0x2026; }
static inline int is_closer(wchar_t c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' ||
           c == 0x201D || c == 0x2019 || c == 0xBB || c == 0x203A;
}
static inline int is_opener(wchar_t c)
{
    return c == '"' || c == '\'' || c == '(' || c == '[' || c == '{' ||
           c == 0x201C || c == 0x2018 || c == 0xAB || c == 0x2039;
}


/* Simple case folding for the scripts above (enough for dictionary matching). */
static inline wchar_t fold_char(wchar_t c)
{
    if (c >= 'A' && c <= 'Z') return (wchar_t)(c + 32);
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (wchar_t)(c + 32);
    if (c >= 0x100 && c <= 0x17F) return (wchar_t)(c | 1);
    if (c >= 0x391 && c <= 0x3A9) return (wchar_t)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (wchar_t)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (wchar_t)(c + 80);
    return c;
}
static inline int is_alnum_c(wchar_t c) { return is_alpha(c) || (c >= '0' && c <= '9') || c >= 0x4E00; }

/* Zero-width characters that carry no content (soft hyphen is handled separately). */
static inline int is_zero_width(wchar_t c)
{
    return c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0xFEFF;
}
#endif
