#include "find.h"
#include "rx.h"
#include "textutil.h"
#include <stdlib.h>
#include <string.h>

static int boundary_ok(const wchar_t *t, size_t n, size_t s, size_t l)
{
    if (l == 0) return 1;
    if (is_alnum_c(t[s]) && s > 0 && is_alnum_c(t[s - 1])) return 0;
    if (is_alnum_c(t[s + l - 1]) && s + l < n && is_alnum_c(t[s + l])) return 0;
    return 1;
}

static int plain_at(const wchar_t *t, size_t n, size_t i, const wchar_t *nd, size_t nl, const FindOpts *o)
{
    if (nl > n - i) return 0;
    for (size_t k = 0; k < nl; k++) {
        wchar_t x = t[i + k], y = nd[k];
        if (!o->match_case) { x = fold_char(x); y = fold_char(y); }
        if (x != y) return 0;
    }
    return !o->whole_word || boundary_ok(t, n, i, nl);
}

static Rx *compile(const wchar_t *needle, const FindOpts *o, wchar_t *err, size_t errcap)
{
    size_t l = wcslen(needle);
    wchar_t *pat = (wchar_t *)malloc((l + 16) * sizeof(wchar_t));
    if (!pat) return NULL;
    if (o->whole_word) { wcscpy(pat, L"\\b(?:"); wcscat(pat, needle); wcscat(pat, L")\\b"); }
    else wcscpy(pat, needle);
    Rx *r = rx_compile(pat, !o->match_case, err, errcap);
    free(pat);
    return r;
}

int find_next(const wchar_t *text, size_t n, const wchar_t *needle, const FindOpts *o, size_t from, int dir,
              size_t *start, size_t *len, wchar_t *err, size_t errcap)
{
    size_t nl = wcslen(needle);
    if (nl == 0) return 0;
    if (from > n) from = n;
    if (o->regex) {
        Rx *r = compile(needle, o, err, errcap);
        if (!r) return err && err[0] ? -1 : 0;
        size_t caps[20];
        int ok = 0;
        if (dir >= 0) {
            for (size_t i = from; i <= n && !ok; i++)
                if (rx_match_at(r, text, n, i, caps) && caps[1] > caps[0]) ok = 1;
        } else {
            for (size_t i = from; i > 0 && !ok; i--)
                if (rx_match_at(r, text, n, i - 1, caps) && caps[1] > caps[0]) ok = 1;
        }
        if (ok) { *start = caps[0]; *len = caps[1] - caps[0]; }
        rx_free(r);
        return ok;
    }
    if (dir >= 0) {
        for (size_t i = from; i + nl <= n; i++)
            if (plain_at(text, n, i, needle, nl, o)) { *start = i; *len = nl; return 1; }
    } else {
        for (size_t i = from; i > 0; i--)
            if (i - 1 + nl <= n && plain_at(text, n, i - 1, needle, nl, o)) { *start = i - 1; *len = nl; return 1; }
    }
    return 0;
}

wchar_t *find_replacement_for(const wchar_t *text, size_t n, const wchar_t *needle, const wchar_t *repl, const FindOpts *o,
                              size_t start, size_t len)
{
    (void)len;
    if (o->regex) {
        wchar_t err[64];
        Rx *r = compile(needle, o, err, 64);
        if (r) {
            size_t caps[20];
            if (rx_groups(r) <= 9 && rx_match_at(r, text, n, start, caps)) {
                size_t l = rx_expand(repl, text, caps, rx_groups(r), NULL);
                wchar_t *out = (wchar_t *)malloc((l + 1) * sizeof(wchar_t));
                if (out) rx_expand(repl, text, caps, rx_groups(r), out);
                rx_free(r);
                return out;
            }
            rx_free(r);
        }
    }
    size_t l = wcslen(repl);
    wchar_t *out = (wchar_t *)malloc((l + 1) * sizeof(wchar_t));
    if (out) memcpy(out, repl, (l + 1) * sizeof(wchar_t));
    return out;
}

wchar_t *find_replace_all(const wchar_t *text, size_t n, const wchar_t *needle, const wchar_t *repl, const FindOpts *o,
                          size_t *count, wchar_t *err, size_t errcap)
{
    *count = 0;
    size_t cap = n + 64, len = 0, pos = 0;
    wchar_t *out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) return NULL;
    Rx *r = NULL;
    if (o->regex) {
        r = compile(needle, o, err, errcap);
        if (!r) { free(out); return NULL; }
        if (rx_groups(r) > 9) { rx_free(r); free(out); if (err && errcap) wcsncpy(err, L"At most 9 groups are supported.", errcap); return NULL; }
    }
    size_t nl = wcslen(needle), rl = wcslen(repl);
    if (nl == 0) { free(out); rx_free(r); out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t)); if (out) { memcpy(out, text, n * sizeof(wchar_t)); out[n] = 0; } return out; }
    while (pos <= n) {
        size_t caps[20], ms = 0, ml = 0;
        int hit = 0;
        if (r) {
            for (size_t i = pos; i <= n; i++)
                if (rx_match_at(r, text, n, i, caps) && caps[1] > caps[0]) { ms = caps[0]; ml = caps[1] - caps[0]; hit = 1; break; }
        } else {
            for (size_t i = pos; i + nl <= n; i++)
                if (plain_at(text, n, i, needle, nl, o)) { ms = i; ml = nl; hit = 1; break; }
        }
        size_t copy_to = hit ? ms : n;
        size_t need = len + (copy_to - pos) + (hit ? (r ? rx_expand(repl, text, caps, rx_groups(r), NULL) : rl) : 0) + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            wchar_t *np = (wchar_t *)realloc(out, cap * sizeof(wchar_t));
            if (!np) { free(out); rx_free(r); return NULL; }
            out = np;
        }
        memcpy(out + len, text + pos, (copy_to - pos) * sizeof(wchar_t));
        len += copy_to - pos;
        if (!hit) break;
        if (r) len += rx_expand(repl, text, caps, rx_groups(r), out + len);
        else { memcpy(out + len, repl, rl * sizeof(wchar_t)); len += rl; }
        (*count)++;
        pos = ms + ml;
    }
    out[len] = 0;
    rx_free(r);
    return out;
}
