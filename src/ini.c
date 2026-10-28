#include "ini.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = wcslen(s);
    wchar_t *p = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (p) memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

void ini_init(Ini *d) { memset(d, 0, sizeof *d); }

void ini_free(Ini *d)
{
    for (int i = 0; i < d->n; i++) { free(d->kv[i].section); free(d->kv[i].key); free(d->kv[i].value); }
    free(d->kv);
    ini_init(d);
}

static int find(const Ini *d, const wchar_t *sec, const wchar_t *key)
{
    for (int i = 0; i < d->n; i++)
        if (wcscmp(d->kv[i].section, sec) == 0 && wcscmp(d->kv[i].key, key) == 0) return i;
    return -1;
}

int ini_set(Ini *d, const wchar_t *sec, const wchar_t *key, const wchar_t *value)
{
    if (!sec || !*sec || !key || !*key || !value) return 0;
    for (const wchar_t *p = key; *p; p++) if (*p == '=' || *p == '\n' || *p == '\r') return 0;
    for (const wchar_t *p = sec; *p; p++) if (*p == ']' || *p == '\n' || *p == '\r') return 0;
    wchar_t *v = wdup(value);
    if (!v) return 0;
    int i = find(d, sec, key);
    if (i >= 0) { free(d->kv[i].value); d->kv[i].value = v; return 1; }
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 32;
        IniKV *p = (IniKV *)realloc(d->kv, (size_t)nc * sizeof *p);
        if (!p) { free(v); return 0; }
        d->kv = p; d->cap = nc;
    }
    d->kv[d->n].section = wdup(sec);
    d->kv[d->n].key = wdup(key);
    d->kv[d->n].value = v;
    if (!d->kv[d->n].section || !d->kv[d->n].key) { free(d->kv[d->n].section); free(d->kv[d->n].key); free(v); return 0; }
    d->n++;
    return 1;
}

int ini_set_int(Ini *d, const wchar_t *sec, const wchar_t *key, long v)
{
    wchar_t b[32];
    swprintf(b, 32, L"%ld", v);
    return ini_set(d, sec, key, b);
}

const wchar_t *ini_get(const Ini *d, const wchar_t *sec, const wchar_t *key)
{
    int i = find(d, sec, key);
    return i < 0 ? NULL : d->kv[i].value;
}

long ini_get_int(const Ini *d, const wchar_t *sec, const wchar_t *key, long def)
{
    const wchar_t *v = ini_get(d, sec, key);
    if (!v || !*v) return def;
    wchar_t *end;
    long r = wcstol(v, &end, 10);
    return end == v ? def : r;
}

void ini_remove_section(Ini *d, const wchar_t *sec)
{
    int w = 0;
    for (int r = 0; r < d->n; r++) {
        if (wcscmp(d->kv[r].section, sec) == 0) { free(d->kv[r].section); free(d->kv[r].key); free(d->kv[r].value); }
        else d->kv[w++] = d->kv[r];
    }
    d->n = w;
}

/* ------------------------------------------------------------ serialization */

typedef struct Buf { char *p; size_t n, cap; int ok; } Buf;
static void bput(Buf *b, const char *s, size_t n)
{
    if (!b->ok) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *p = (char *)realloc(b->p, nc);
        if (!p) { b->ok = 0; return; }
        b->p = p; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void bwide(Buf *b, const wchar_t *w, size_t n, int escape)
{
    wchar_t chunk[256];
    size_t k = 0;
    for (size_t i = 0; i <= n; i++) {
        int flush = (i == n) || k + 2 >= 256;
        if (i < n && !flush) {
            wchar_t c = w[i];
            if (escape && c == '\\') { chunk[k++] = '\\'; chunk[k++] = '\\'; }
            else if (escape && c == '\n') { chunk[k++] = '\\'; chunk[k++] = 'n'; }
            else if (escape && c == '\r') { chunk[k++] = '\\'; chunk[k++] = 'r'; }
            else chunk[k++] = c;
            continue;
        }
        if (k) {
            char u[256 * 4];
            size_t bn = utf8_from_wide(chunk, k, u);
            bput(b, u, bn);
            k = 0;
        }
        if (i < n) {                                              /* the char that did not fit */
            wchar_t c = w[i];
            if (escape && c == '\\') { chunk[k++] = '\\'; chunk[k++] = '\\'; }
            else if (escape && c == '\n') { chunk[k++] = '\\'; chunk[k++] = 'n'; }
            else if (escape && c == '\r') { chunk[k++] = '\\'; chunk[k++] = 'r'; }
            else chunk[k++] = c;
        }
    }
}

char *ini_to_utf8(const Ini *d, size_t *len)
{
    Buf b = { NULL, 0, 0, 1 };
    bput(&b, "# Mouthpiece settings/project file (UTF-8)\n", 42);
    for (int i = 0; i < d->n; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) if (wcscmp(d->kv[j].section, d->kv[i].section) == 0) { first = 0; break; }
        if (!first) continue;
        bput(&b, "\n[", 2);
        bwide(&b, d->kv[i].section, wcslen(d->kv[i].section), 0);
        bput(&b, "]\n", 2);
        for (int j = i; j < d->n; j++) {
            if (wcscmp(d->kv[j].section, d->kv[i].section) != 0) continue;
            bwide(&b, d->kv[j].key, wcslen(d->kv[j].key), 0);
            bput(&b, "=", 1);
            bwide(&b, d->kv[j].value, wcslen(d->kv[j].value), 1);
            bput(&b, "\n", 1);
        }
    }
    if (!b.ok) { free(b.p); return NULL; }
    if (!b.p) { b.p = (char *)malloc(1); }
    b.p[b.n] = 0;
    *len = b.n;
    return b.p;
}

int ini_from_utf8(Ini *d, const char *s, size_t len)
{
    ini_free(d);
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) { s += 3; len -= 3; }
    wchar_t sec[256];
    sec[0] = 0;
    int count = 0;
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && s[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        if (le > ls && s[le - 1] == '\r') le--;
        if (le == ls || s[ls] == '#' || s[ls] == ';') continue;
        size_t wn = wide_from_utf8(s + ls, le - ls, NULL);
        wchar_t *w = (wchar_t *)malloc((wn + 1) * sizeof(wchar_t));
        if (!w) continue;
        wide_from_utf8(s + ls, le - ls, w);
        w[wn] = 0;
        if (w[0] == '[') {
            wchar_t *e = wcschr(w, L']');
            if (e && (size_t)(e - w - 1) < 255) { memcpy(sec, w + 1, (size_t)(e - w - 1) * sizeof(wchar_t)); sec[e - w - 1] = 0; }
        } else if (sec[0]) {
            wchar_t *eq = wcschr(w, L'=');
            if (eq && eq != w) {
                *eq = 0;
                wchar_t *v = eq + 1, *o = v;                      /* unescape in place */
                for (const wchar_t *p = v; *p; p++) {
                    if (*p == '\\' && p[1]) {
                        p++;
                        *o++ = *p == 'n' ? L'\n' : *p == 'r' ? L'\r' : *p;
                    } else *o++ = *p;
                }
                *o = 0;
                if (ini_set(d, sec, w, v)) count++;
            }
        }
        free(w);
    }
    return count;
}
