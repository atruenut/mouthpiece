#include "utf8.h"

size_t utf8_from_wide(const wchar_t *w, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned long c = (unsigned long)w[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
            (unsigned long)w[i + 1] >= 0xDC00 && (unsigned long)w[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((unsigned long)w[i + 1] - 0xDC00);
            i++;
        } else if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) { if (out) out[o] = (char)c; o += 1; }
        else if (c < 0x800) { if (out) { out[o] = (char)(0xC0 | (c >> 6)); out[o + 1] = (char)(0x80 | (c & 0x3F)); } o += 2; }
        else if (c < 0x10000) { if (out) { out[o] = (char)(0xE0 | (c >> 12)); out[o + 1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o + 2] = (char)(0x80 | (c & 0x3F)); } o += 3; }
        else { if (out) { out[o] = (char)(0xF0 | (c >> 18)); out[o + 1] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o + 2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o + 3] = (char)(0x80 | (c & 0x3F)); } o += 4; }
    }
    return o;
}

size_t wide_from_utf8(const char *s, size_t n, wchar_t *out)
{
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned char b = (unsigned char)s[i];
        unsigned long c;
        size_t len;
        if (b < 0x80) { c = b; len = 1; }
        else if (b >= 0xC2 && b <= 0xDF) { c = b & 0x1F; len = 2; }
        else if (b >= 0xE0 && b <= 0xEF) { c = b & 0x0F; len = 3; }
        else if (b >= 0xF0 && b <= 0xF4) { c = b & 0x07; len = 4; }
        else { c = 0xFFFD; len = 1; }
        if (len > 1) {
            if (i + len > n) { c = 0xFFFD; len = 1; }
            else {
                for (size_t k = 1; k < len; k++) {
                    unsigned char cb = (unsigned char)s[i + k];
                    if ((cb & 0xC0) != 0x80) { c = 0xFFFD; len = 1; break; }
                    c = (c << 6) | (cb & 0x3F);
                }
                if (len == 3 && (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF))) c = 0xFFFD;
                if (len == 4 && (c < 0x10000 || c > 0x10FFFF)) c = 0xFFFD;
            }
        }
        i += len;
        if (sizeof(wchar_t) == 2 && c >= 0x10000) {
            if (out) { out[o] = (wchar_t)(0xD800 + ((c - 0x10000) >> 10)); out[o + 1] = (wchar_t)(0xDC00 + ((c - 0x10000) & 0x3FF)); }
            o += 2;
        } else {
            if (out) out[o] = (wchar_t)c;
            o += 1;
        }
    }
    return o;
}
