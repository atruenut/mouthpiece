#include "pron.h"
#include "textutil.h"
#include "utf8.h"
#include "rx.h"
#include <stdlib.h>
#include <string.h>

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = wcslen(s);
    wchar_t *p = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (p) memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

void pron_init(PronDict *d) { memset(d, 0, sizeof *d); }

void pron_clear(PronDict *d)
{
    for (int i = 0; i < d->n; i++) { free(d->e[i].word); free(d->e[i].repl); free(d->e[i].scope); rx_free((Rx *)d->e[i].rx); }
    d->n = 0;
    d->nrx = 0;
    d->index_valid = 0;
}

void pron_free(PronDict *d)
{
    pron_clear(d);
    free(d->e); free(d->order); free(d->rxlist);
    pron_init(d);
}

static int bad_text(const wchar_t *s)
{
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') return 1;
    return 0;
}

static int same_scope(const wchar_t *a, const wchar_t *b)
{
    if (!a) a = L"";
    if (!b) b = L"";
    return wcscmp(a, b) == 0;
}

static int same_word(const wchar_t *a, const wchar_t *b, int match_case)
{
    size_t n = wcslen(a);
    if (n != wcslen(b)) return 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t x = a[i], y = b[i];
        if (!match_case) { x = fold_char(x); y = fold_char(y); }
        if (x != y) return 0;
    }
    return 1;
}

int pron_find_ex(const PronDict *d, const wchar_t *word, int regex, const wchar_t *scope)
{
    for (int i = 0; i < d->n; i++)
        if (d->e[i].regex == (regex != 0) && same_scope(d->e[i].scope, scope) &&
            same_word(d->e[i].word, word, d->e[i].match_case)) return i;
    return -1;
}

int pron_find(const PronDict *d, const wchar_t *word) { return pron_find_ex(d, word, 0, NULL); }

static int dup_other(const PronDict *d, int self, const wchar_t *word, int match_case, int regex, const wchar_t *scope)
{
    for (int i = 0; i < d->n; i++)
        if (i != self && d->e[i].regex == (regex != 0) && same_scope(d->e[i].scope, scope) &&
            same_word(d->e[i].word, word, match_case && d->e[i].match_case)) return 1;
    return 0;
}

int pron_regex_check(const wchar_t *pattern, int match_case, wchar_t *err, size_t errcap)
{
    Rx *r = rx_compile(pattern, !match_case, err, errcap);
    if (!r) return 0;
    rx_free(r);
    return 1;
}

int pron_add_ex(PronDict *d, const wchar_t *word, const wchar_t *repl, int flags, const wchar_t *scope)
{
    if (!word || !*word || !repl || bad_text(word) || bad_text(repl) || (scope && bad_text(scope))) return -2;
    int regex = (flags & PRON_REGEX) != 0;
    if (pron_find_ex(d, word, regex, scope) >= 0) return -1;
    if (regex) {
        wchar_t err[64];
        if (!pron_regex_check(word, (flags & PRON_CASE) != 0, err, 64)) return -2;
    }
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 32;
        PronEntry *p = (PronEntry *)realloc(d->e, (size_t)nc * sizeof *p);
        if (!p) return -2;
        d->e = p; d->cap = nc;
    }
    PronEntry *e = &d->e[d->n];
    memset(e, 0, sizeof *e);
    e->word = wdup(word); e->repl = wdup(repl);
    e->scope = (scope && *scope) ? wdup(scope) : NULL;
    if (!e->word || !e->repl || (scope && *scope && !e->scope)) { free(e->word); free(e->repl); free(e->scope); return -2; }
    e->match_case = (flags & PRON_CASE) != 0; e->partial = (flags & PRON_PARTIAL) != 0; e->regex = regex;
    e->s_first = 0; e->s_last = -1;
    d->index_valid = 0;
    return d->n++;
}

int pron_add(PronDict *d, const wchar_t *word, const wchar_t *repl, int match_case, int partial)
{
    return pron_add_ex(d, word, repl, (match_case ? PRON_CASE : 0) | (partial ? PRON_PARTIAL : 0), NULL);
}

int pron_set_ex(PronDict *d, int i, const wchar_t *word, const wchar_t *repl, int flags, const wchar_t *scope)
{
    if (i < 0 || i >= d->n || !word || !*word || !repl || bad_text(word) || bad_text(repl) || (scope && bad_text(scope))) return 0;
    int regex = (flags & PRON_REGEX) != 0;
    if (dup_other(d, i, word, (flags & PRON_CASE) != 0, regex, scope)) return 0;
    if (regex) {
        wchar_t err[64];
        if (!pron_regex_check(word, (flags & PRON_CASE) != 0, err, 64)) return 0;
    }
    wchar_t *w = wdup(word), *r = wdup(repl), *sc = (scope && *scope) ? wdup(scope) : NULL;
    if (!w || !r || (scope && *scope && !sc)) { free(w); free(r); free(sc); return 0; }
    free(d->e[i].word); free(d->e[i].repl); free(d->e[i].scope);
    d->e[i].word = w; d->e[i].repl = r; d->e[i].scope = sc;
    d->e[i].match_case = (flags & PRON_CASE) != 0; d->e[i].partial = (flags & PRON_PARTIAL) != 0; d->e[i].regex = regex;
    d->e[i].s_first = 0; d->e[i].s_last = -1;
    d->index_valid = 0;
    return 1;
}

int pron_set(PronDict *d, int i, const wchar_t *word, const wchar_t *repl, int match_case, int partial)
{
    int flags = (match_case ? PRON_CASE : 0) | (partial ? PRON_PARTIAL : 0);
    if (i >= 0 && i < d->n) {
        if (d->e[i].regex) flags |= PRON_REGEX;
        return pron_set_ex(d, i, word, repl, flags, d->e[i].scope);
    }
    return 0;
}

void pron_remove(PronDict *d, int i)
{
    if (i < 0 || i >= d->n) return;
    free(d->e[i].word); free(d->e[i].repl); free(d->e[i].scope); rx_free((Rx *)d->e[i].rx);
    memmove(&d->e[i], &d->e[i + 1], (size_t)(d->n - i - 1) * sizeof *d->e);
    d->n--;
    d->index_valid = 0;
}

/* ---------------------------------------------------------------- matching */

static int bucket_of(wchar_t c)
{
    c = fold_char(c);
    return c < 128 ? (int)c : 128;
}

static const PronDict *g_sort_dict;
static int cmp_entries(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    const PronEntry *ea = &g_sort_dict->e[a], *eb = &g_sort_dict->e[b];
    int ba = bucket_of(ea->word[0]), bb = bucket_of(eb->word[0]);
    if (ba != bb) return ba - bb;
    size_t la = wcslen(ea->word), lb = wcslen(eb->word);
    if (la != lb) return la > lb ? -1 : 1;                    /* longest first */
    int sa = ea->scope != NULL, sb = eb->scope != NULL;
    if (sa != sb) return sa ? -1 : 1;                         /* scoped entries win ties */
    return a - b;                                              /* then insertion order (stable) */
}

void pron_prepare(PronDict *d)
{
    free(d->order); free(d->rxlist);
    d->order = (int *)malloc((size_t)(d->n ? d->n : 1) * sizeof(int));
    d->rxlist = (int *)malloc((size_t)(d->n ? d->n : 1) * sizeof(int));
    d->nrx = 0;
    if (!d->order || !d->rxlist) { free(d->order); free(d->rxlist); d->order = d->rxlist = NULL; d->index_valid = 0; return; }
    int np = 0;
    for (int i = 0; i < d->n; i++) {
        PronEntry *e = &d->e[i];
        rx_free((Rx *)e->rx);
        e->rx = NULL;
        if (e->regex) {
            wchar_t err[64];
            e->rx = rx_compile(e->word, !e->match_case, err, 64);
            if (e->rx) d->rxlist[d->nrx++] = i;
        } else d->order[np++] = i;
    }
    g_sort_dict = d;
    if (np > 1) qsort(d->order, (size_t)np, sizeof(int), cmp_entries);
    int k = 0;
    for (int b = 0; b <= 128; b++) {
        d->bucket[b] = k;
        while (k < np && bucket_of(d->e[d->order[k]].word[0]) == b) k++;
    }
    d->bucket[129] = np;
    d->index_valid = 1;
}

static int try_entry(const PronEntry *e, const wchar_t *t, size_t n, size_t i)
{
    size_t len = wcslen(e->word);
    if (len > n - i) return 0;
    for (size_t k = 0; k < len; k++) {
        wchar_t x = t[i + k], y = e->word[k];
        if (!e->match_case) { x = fold_char(x); y = fold_char(y); }
        if (x != y) return 0;
    }
    if (!e->partial) {
        if (is_alnum_c(e->word[0]) && i > 0 && is_alnum_c(t[i - 1])) return 0;
        if (is_alnum_c(e->word[len - 1]) && i + len < n && is_alnum_c(t[i + len])) return 0;
    }
    return 1;
}

static int entry_active(const PronEntry *e, int sent)
{
    if (!e->scope || !e->scope[0]) return 1;
    return sent >= 0 && sent >= e->s_first && sent <= e->s_last;
}

int pron_match_ex(const PronDict *d, const wchar_t *t, size_t n, size_t i, int sent, size_t *matched)
{
    if (i >= n || d->n == 0) return -1;
    if (d->index_valid) {
        int b = bucket_of(t[i]);
        for (int k = d->bucket[b]; k < d->bucket[b + 1]; k++) {
            const PronEntry *e = &d->e[d->order[k]];
            if (entry_active(e, sent) && try_entry(e, t, n, i)) { *matched = wcslen(e->word); return d->order[k]; }
        }
        for (int k = 0; k < d->nrx; k++) {
            const PronEntry *e = &d->e[d->rxlist[k]];
            if (!entry_active(e, sent) || !e->rx) continue;
            size_t caps[20];
            int ng = rx_groups((const Rx *)e->rx);
            if (ng > 9) continue;
            if (rx_match_at((const Rx *)e->rx, t, n, i, caps) && caps[1] > i) {
                *matched = caps[1] - i;
                return d->rxlist[k];
            }
        }
        return -1;
    }
    int best = -1;                                             /* slow path (index not built): plain entries only */
    size_t bl = 0;
    for (int k = 0; k < d->n; k++)
        if (!d->e[k].regex && entry_active(&d->e[k], sent) && try_entry(&d->e[k], t, n, i)) {
            size_t l = wcslen(d->e[k].word);
            if (l > bl) { bl = l; best = k; }
        }
    if (best >= 0) *matched = bl;
    return best;
}

int pron_match(const PronDict *d, const wchar_t *t, size_t n, size_t i, size_t *matched)
{
    return pron_match_ex(d, t, n, i, -1, matched);
}

size_t pron_replacement(const PronDict *d, int ei, const wchar_t *t, size_t n, size_t i, wchar_t *out)
{
    const PronEntry *e = &d->e[ei];
    if (!e->regex || !e->rx) {
        size_t l = wcslen(e->repl);
        if (out) memcpy(out, e->repl, (l + 1) * sizeof(wchar_t));
        return l;
    }
    size_t caps[20];
    if (!rx_match_at((const Rx *)e->rx, t, n, i, caps)) { if (out) out[0] = 0; return 0; }
    return rx_expand(e->repl, t, caps, rx_groups((const Rx *)e->rx), out);
}

uint64_t pron_signature(const PronDict *d)
{
    uint64_t h = 1469598103934665603ULL;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 1099511628211ULL)
    for (int i = 0; i < d->n; i++) {
        MIX(d->e[i].match_case); MIX(d->e[i].partial); MIX(d->e[i].regex);
        if (d->e[i].scope) for (const wchar_t *p = d->e[i].scope; *p; p++) MIX(*p);
        MIX(0x1D);
        for (const wchar_t *p = d->e[i].word; *p; p++) MIX(*p);
        MIX(0x1F);
        for (const wchar_t *p = d->e[i].repl; *p; p++) MIX(*p);
        MIX(0x1E);
    }
#undef MIX
    return h;
}

/* ----------------------------------------------------------- serialization */

char *pron_to_utf8(const PronDict *d, size_t *len)
{
    static const char head[] = "# Mouthpiece pronunciation dictionary\n# flags<TAB>word<TAB>replacement   (flags: c = match case, p = match inside words, r = regular expression, - = none; optional 4th column = scope)\n";
    size_t cap = sizeof head;
    for (int i = 0; i < d->n; i++)
        cap += 6 + 4 * (wcslen(d->e[i].word) + wcslen(d->e[i].repl) + (d->e[i].scope ? wcslen(d->e[i].scope) : 0)) + 4;
    char *out = (char *)malloc(cap + 1);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, head, sizeof head - 1); o = sizeof head - 1;
    for (int i = 0; i < d->n; i++) {
        const PronEntry *e = &d->e[i];
        if (e->match_case) out[o++] = 'c';
        if (e->partial) out[o++] = 'p';
        if (e->regex) out[o++] = 'r';
        if (!e->match_case && !e->partial && !e->regex) out[o++] = '-';
        out[o++] = '\t';
        o += utf8_from_wide(e->word, wcslen(e->word), out + o);
        out[o++] = '\t';
        o += utf8_from_wide(e->repl, wcslen(e->repl), out + o);
        if (e->scope) { out[o++] = '\t'; o += utf8_from_wide(e->scope, wcslen(e->scope), out + o); }
        out[o++] = '\n';
    }
    out[o] = 0;
    *len = o;
    return out;
}

int pron_from_utf8(PronDict *d, const char *s, size_t len, int *bad)
{
    int nbad = 0, loaded = 0;
    pron_clear(d);
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) { s += 3; len -= 3; }
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && s[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        if (le > ls && s[le - 1] == '\r') le--;
        if (le == ls || s[ls] == '#') continue;
        size_t t1 = ls;
        while (t1 < le && s[t1] != '\t') t1++;
        size_t t2 = t1 + 1;
        while (t2 < le && s[t2] != '\t') t2++;
        if (t1 >= le || t2 >= le) { nbad++; continue; }
        size_t t3 = t2 + 1;                                      /* optional scope column */
        while (t3 < le && s[t3] != '\t') t3++;
        size_t rend = t3 < le ? t3 : le;
        int flags = 0;
        for (size_t k = ls; k < t1; k++) { if (s[k] == 'c') flags |= PRON_CASE; else if (s[k] == 'p') flags |= PRON_PARTIAL; else if (s[k] == 'r') flags |= PRON_REGEX; }
        size_t wn = wide_from_utf8(s + t1 + 1, t2 - t1 - 1, NULL), rn = wide_from_utf8(s + t2 + 1, rend - t2 - 1, NULL);
        wchar_t *w = (wchar_t *)malloc((wn + 1) * sizeof(wchar_t)), *r = (wchar_t *)malloc((rn + 1) * sizeof(wchar_t));
        if (!w || !r) { free(w); free(r); nbad++; continue; }
        wide_from_utf8(s + t1 + 1, t2 - t1 - 1, w); w[wn] = 0;
        wide_from_utf8(s + t2 + 1, rend - t2 - 1, r); r[rn] = 0;
        wchar_t *sc = NULL;
        if (t3 < le) {
            size_t sn = wide_from_utf8(s + t3 + 1, le - t3 - 1, NULL);
            sc = (wchar_t *)malloc((sn + 1) * sizeof(wchar_t));
            if (sc) { wide_from_utf8(s + t3 + 1, le - t3 - 1, sc); sc[sn] = 0; }
        }
        if (pron_add_ex(d, w, r, flags, sc) >= 0) loaded++; else nbad++;
        free(w); free(r); free(sc);
    }
    pron_prepare(d);
    if (bad) *bad = nbad;
    return loaded;
}
