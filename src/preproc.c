#include "preproc.h"
#include "textutil.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Line {
    size_t s, e;       /* content range [s,e) excluding the terminator           */
    size_t a, b;       /* trimmed range; a == b means blank                      */
    int    tl;         /* terminator length at e (0 for an unterminated last line) */
    int    del;        /* 0 keep, else the ChangeKind that removes the line      */
    int    tab;        /* part of a table that is read row by row                */
} Line;

enum { SEP_NEWLINE, SEP_SPACE, SEP_NOSPACE, SEP_DEHYPHEN, SEP_PARA };

typedef struct Ctx {
    const wchar_t *t;
    size_t len;
    PreprocOptions o;
    int log;
    PreprocResult *r;
} Ctx;

void preproc_default_options(PreprocOptions *o)
{
    o->enabled = 1;
    o->repeated_lines = o->page_numbers = o->join_lines = o->fix_hyphens = 1;
    o->normalize_ws = o->normalize_unicode = 1;
    o->remove_urls = o->remove_citations = 0;
    o->remove_footnotes = o->remove_tables = o->table_rows = 0;
}

void preproc_free(PreprocResult *r)
{
    free(r->text); free(r->map); free(r->changes);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------ output buffer */

static int grow_text(PreprocResult *r, size_t need)
{
    if (need + 1 <= (size_t)r->cap_text) return 1;
    size_t nc = (size_t)r->cap_text * 2;
    if (nc < need + 1) nc = need + 1;
    if (nc > 0x7FFFFFFF) return 0;
    wchar_t *p = (wchar_t *)realloc(r->text, nc * sizeof(wchar_t));
    if (!p) return 0;
    r->text = p; r->cap_text = (int)nc;
    return 1;
}

static int push_map(PreprocResult *r, size_t ns, size_t nl, size_t ss, size_t sl)
{
    if (r->nmap == r->cap_map) {
        int nc = r->cap_map ? r->cap_map * 2 : 256;
        MapEntry *p = (MapEntry *)realloc(r->map, (size_t)nc * sizeof *p);
        if (!p) return 0;
        r->map = p; r->cap_map = nc;
    }
    MapEntry *m = &r->map[r->nmap++];
    m->norm_start = ns; m->norm_len = nl; m->src_start = ss; m->src_len = sl;
    return 1;
}

/* Append n chars produced from source [src_pos, src_pos+src_len). */
static int emit(Ctx *c, size_t src_pos, size_t src_len, const wchar_t *s, size_t n)
{
    PreprocResult *r = c->r;
    if (!grow_text(r, r->len + n)) return 0;
    memcpy(r->text + r->len, s, n * sizeof(wchar_t));
    if (n == src_len && r->nmap) {                          /* try to extend a linear run */
        MapEntry *m = &r->map[r->nmap - 1];
        if (m->norm_len == m->src_len && m->src_start + m->src_len == src_pos &&
            m->norm_start + m->norm_len == r->len) {
            m->norm_len += n; m->src_len += n;
            r->len += n;
            return 1;
        }
    }
    if (!push_map(r, r->len, n, src_pos, src_len)) return 0;
    r->len += n;
    return 1;
}

static int emit1(Ctx *c, size_t src_pos, wchar_t ch) { return emit(c, src_pos, 1, &ch, 1); }

static wchar_t last_out(const Ctx *c) { return c->r->len ? c->r->text[c->r->len - 1] : 0; }

static void unemit_last(Ctx *c)
{
    PreprocResult *r = c->r;
    if (!r->len) return;
    MapEntry *m = &r->map[r->nmap - 1];
    if (m->norm_len > 1) {
        if (m->norm_len == m->src_len) m->src_len--;
        m->norm_len--;
    } else {
        r->nmap--;
    }
    r->len--;
}

static int log_change(Ctx *c, int kind, size_t start, size_t len)
{
    PreprocResult *r = c->r;
    if (!c->log) return 1;
    if (r->nchanges == r->cap_changes) {
        int nc = r->cap_changes ? r->cap_changes * 2 : 128;
        Change *p = (Change *)realloc(r->changes, (size_t)nc * sizeof *p);
        if (!p) return 0;
        r->changes = p; r->cap_changes = nc;
    }
    r->changes[r->nchanges].kind = kind;
    r->changes[r->nchanges].src_start = start;
    r->changes[r->nchanges].src_len = len;
    r->nchanges++;
    return 1;
}

/* ---------------------------------------------------------------- map lookup */

static int find_norm(const PreprocResult *r, size_t n)       /* last entry with norm_start <= n */
{
    int lo = 0, hi = r->nmap - 1, ans = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (r->map[mid].norm_start <= n) { ans = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return ans;
}

size_t preproc_norm_to_src(const PreprocResult *r, size_t n)
{
    if (r->nmap == 0) return 0;
    if (n >= r->len) return r->src_len;
    const MapEntry *m = &r->map[find_norm(r, n)];
    return m->norm_len == m->src_len ? m->src_start + (n - m->norm_start) : m->src_start;
}

size_t preproc_norm_end_to_src(const PreprocResult *r, size_t end)
{
    if (r->nmap == 0 || end == 0) return 0;
    if (end > r->len) return r->src_len;
    size_t n = end - 1;
    const MapEntry *m = &r->map[find_norm(r, n)];
    return m->norm_len == m->src_len ? m->src_start + (n - m->norm_start) + 1 : m->src_start + m->src_len;
}

size_t preproc_src_to_norm(const PreprocResult *r, size_t p)
{
    if (r->nmap == 0) return 0;
    if (p >= r->src_len) return r->len;
    int lo = 0, hi = r->nmap - 1, ans = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (r->map[mid].src_start <= p) { ans = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (ans < 0) return 0;
    const MapEntry *m = &r->map[ans];
    if (p < m->src_start + m->src_len)
        return m->norm_len == m->src_len ? m->norm_start + (p - m->src_start) : m->norm_start;
    return m->norm_start + m->norm_len;                       /* in removed text: next kept char */
}

/* -------------------------------------------------------------------- lines */

static int is_eol(wchar_t c) { return c == '\n' || c == '\r' || c == 0x0C || c == 0x2028 || c == 0x2029; }
static int blankish(wchar_t c) { return is_space(c) || is_zero_width(c); }

static Line *scan_lines(const wchar_t *t, size_t len, size_t *out_n)
{
    size_t n = 0;
    for (size_t i = 0; i < len;) {                             /* count first, allocate once */
        while (i < len && !is_eol(t[i])) i++;
        if (i < len) { i += (t[i] == '\r' && i + 1 < len && t[i + 1] == '\n') ? 2 : 1; }
        n++;
    }
    Line *L = (Line *)calloc(n ? n : 1, sizeof *L);
    if (!L) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < len;) {
        Line *ln = &L[k++];
        ln->s = i;
        while (i < len && !is_eol(t[i])) i++;
        ln->e = i;
        ln->tl = 0;
        if (i < len) { ln->tl = (t[i] == '\r' && i + 1 < len && t[i + 1] == '\n') ? 2 : 1; i += (size_t)ln->tl; }
        size_t a = ln->s, b = ln->e;
        while (a < b && blankish(t[a])) a++;
        while (b > a && blankish(t[b - 1])) b--;
        ln->a = a; ln->b = b;
    }
    *out_n = n;
    return L;
}

/* ----------------------------------------------- repeated header/footer lines */

static uint64_t line_signature(const wchar_t *t, const Line *ln, int *has_digit, int *first_word_blocked)
{
    size_t n = ln->b - ln->a;
    *has_digit = 0; *first_word_blocked = 0;
    if (n < 4 || n > 80) return 0;
    uint64_t h = 1469598103934665603ULL;
    int alpha = 0, pd = 0, ps = 0;
    char word[12]; size_t wl = 0; int in_first = 1;
    for (size_t i = ln->a; i < ln->b; i++) {
        wchar_t c = t[i];
        if (is_digit(c)) { *has_digit = 1; if (!pd) { h = (h ^ '#') * 1099511628211ULL; } pd = 1; ps = 0; in_first = 0; continue; }
        pd = 0;
        if (is_space(c)) { if (!ps) h = (h ^ ' ') * 1099511628211ULL; ps = 1; in_first = 0; continue; }
        ps = 0;
        if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
        if (is_alpha(c)) alpha++;
        if (in_first && wl < sizeof word - 1 && c < 0x80) word[wl++] = (char)c;
        h = (h ^ (uint64_t)c) * 1099511628211ULL;
    }
    word[wl] = 0;
    if (alpha < 3) return 0;
    /* "Chapter 3", "Part II", ... repeat regularly but are structure, not running heads. */
    static const char *const blocked[] = { "chapter", "part", "book", "section", "appendix", "act",
                                           "scene", "volume", "lesson", "canto", "article", "unit", NULL };
    for (const char *const *b = blocked; *b; b++) if (strcmp(*b, word) == 0) *first_word_blocked = 1;
    return h | 1;
}

typedef struct { uint64_t h; uint32_t line; } HL;
static int cmp_hl(const void *x, const void *y)
{
    const HL *a = (const HL *)x, *b = (const HL *)y;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    return a->line < b->line ? -1 : (a->line > b->line);
}
static int cmp_sz(const void *x, const void *y)
{
    size_t a = *(const size_t *)x, b = *(const size_t *)y;
    return a < b ? -1 : (a > b);
}

static int detect_repeated(Ctx *c, Line *L, size_t nl)
{
    uint64_t *sig = (uint64_t *)calloc(nl ? nl : 1, sizeof *sig);
    if (!sig) return 0;
    size_t ncand = 0;
    for (size_t i = 0; i < nl; i++) {
        int hd, blocked;
        uint64_t h = line_signature(c->t, &L[i], &hd, &blocked);
        if (h && !(hd && blocked)) { sig[i] = h; ncand++; }
    }
    if (ncand >= 3) {
        size_t tsz = 16;
        while (tsz < ncand * 2) tsz <<= 1;
        struct { uint64_t h; uint32_t cnt; } *tab = calloc(tsz, sizeof *tab);
        HL *hl = NULL;
        if (!tab) { free(sig); return 0; }
        for (size_t i = 0; i < nl; i++) {
            if (!sig[i]) continue;
            size_t p = (size_t)(sig[i] * 0x9E3779B97F4A7C15ULL >> 20) & (tsz - 1);
            while (tab[p].h && tab[p].h != sig[i]) p = (p + 1) & (tsz - 1);
            tab[p].h = sig[i]; tab[p].cnt++;
        }
        size_t nrep = 0;
        for (size_t i = 0; i < nl; i++) {
            if (!sig[i]) continue;
            size_t p = (size_t)(sig[i] * 0x9E3779B97F4A7C15ULL >> 20) & (tsz - 1);
            while (tab[p].h != sig[i]) p = (p + 1) & (tsz - 1);
            if (tab[p].cnt >= 3) nrep++;
        }
        if (nrep) {
            hl = (HL *)malloc(nrep * sizeof *hl);
            size_t *gaps = (size_t *)malloc(nrep * sizeof *gaps);
            if (!hl || !gaps) { free(hl); free(gaps); free(tab); free(sig); return 0; }
            size_t k = 0;
            for (size_t i = 0; i < nl; i++) {
                if (!sig[i]) continue;
                size_t p = (size_t)(sig[i] * 0x9E3779B97F4A7C15ULL >> 20) & (tsz - 1);
                while (tab[p].h != sig[i]) p = (p + 1) & (tsz - 1);
                if (tab[p].cnt >= 3) { hl[k].h = sig[i]; hl[k].line = (uint32_t)i; k++; }
            }
            qsort(hl, nrep, sizeof *hl, cmp_hl);
            for (size_t g = 0; g < nrep;) {
                size_t ge = g;
                while (ge < nrep && hl[ge].h == hl[g].h) ge++;
                size_t ng = ge - g - 1;                           /* number of gaps */
                for (size_t j = 0; j < ng; j++) gaps[j] = (size_t)(hl[g + j + 1].line - hl[g + j].line);
                /* median of the gaps (sorting in place is fine; hl keeps the line numbers) */
                qsort(gaps, ng, sizeof *gaps, cmp_sz);
                size_t med = gaps[ng / 2];
                size_t good = 0;
                for (size_t j = 0; j < ng; j++) if (2 * gaps[j] >= med && gaps[j] <= 2 * med) good++;
                if (ng >= 2 && med >= 8 && good * 10 >= ng * 7)
                    for (size_t j = g; j < ge; j++) L[hl[j].line].del = CH_HEADER;
                g = ge;
            }
            free(gaps);
            free(hl);
        }
        free(tab);
    }
    free(sig);
    return 1;
}

/* ------------------------------------------------------------ page numbers */

/* Canonical roman numeral for v (1..3999) into buf (upper case). */
static size_t to_roman(long v, char *buf)
{
    static const struct { int v; const char *s; } T[] = {
        {1000,"M"},{900,"CM"},{500,"D"},{400,"CD"},{100,"C"},{90,"XC"},{50,"L"},{40,"XL"},
        {10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"} };
    size_t n = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        while (v >= T[i].v) { for (const char *p = T[i].s; *p; p++) buf[n++] = *p; v -= T[i].v; }
    buf[n] = 0;
    return n;
}

/* Strict roman numeral: one case only, canonical form (so "IIII", "VX", "mix"-style
 * non-numerals are rejected by round-tripping). Returns 1 and the value, or 0. */
static int parse_roman(const wchar_t *s, size_t n, long *value)
{
    if (n == 0 || n > 12) return 0;
    int lower = (s[0] >= 'a' && s[0] <= 'z');
    long v = 0, prev = 0;
    for (size_t k = n; k-- > 0;) {                            /* right to left */
        wchar_t c = s[k];
        if (lower) { if (c < 'a' || c > 'z') return 0; c = (wchar_t)(c - 32); }
        else if (c < 'A' || c > 'Z') return 0;
        long d;
        switch (c) {
        case 'I': d = 1; break;   case 'V': d = 5; break;    case 'X': d = 10; break;
        case 'L': d = 50; break;  case 'C': d = 100; break;  case 'D': d = 500; break;
        case 'M': d = 1000; break; default: return 0;
        }
        v += (d < prev) ? -d : d;
        if (d >= prev) prev = d;
    }
    if (v < 1 || v > 3999) return 0;
    char canon[32];
    size_t cn = to_roman(v, canon);
    if (cn != n) return 0;
    for (size_t k = 0; k < n; k++) {
        wchar_t c = s[k];
        if (lower) c = (wchar_t)(c - 32);
        if (c != (wchar_t)canon[k]) return 0;
    }
    *value = v;
    return 1;
}

/* Returns 0 = not a page number, 1 = bare (needs sequence evidence), 2 = decorated
 * ("Page 4", "4 of 90", "- 4 -", "Page iv"). *type: 0 arabic, 1 roman. */
static int parse_pagenum(const wchar_t *s, size_t n, long *value, int *type)
{
    size_t i = 0, e = n;
    int lead_dash = 0, trail_dash = 0, word = 0, of = 0;
    *type = 0;
    while (i < e && (s[i] == '-' || s[i] == 0x2013 || s[i] == 0x2014 || s[i] == 0x00B7 || s[i] == 0x2022 || s[i] == ' ')) { if (s[i] != ' ') lead_dash = 1; i++; }
    while (e > i && (s[e - 1] == '-' || s[e - 1] == 0x2013 || s[e - 1] == 0x2014 || s[e - 1] == 0x00B7 || s[e - 1] == 0x2022 || s[e - 1] == ' ')) { if (s[e - 1] != ' ') trail_dash = 1; e--; }
    if (e - i >= 5 && (s[i] | 32) == 'p' && (s[i + 1] | 32) == 'a' && (s[i + 2] | 32) == 'g' && (s[i + 3] | 32) == 'e') {
        i += 4; word = 1;
        while (i < e && s[i] == ' ') i++;
    }
    size_t d0 = i;
    long v = 0;
    while (i < e && is_digit(s[i]) && i - d0 < 4) { v = v * 10 + (s[i] - '0'); i++; }
    if (i == d0) {                                            /* no digits: roman numeral alone? */
        long rv;
        if (!parse_roman(s + d0, e - d0, &rv)) return 0;
        *value = rv; *type = 1;
        if (word || (lead_dash && trail_dash)) return 2;
        return (lead_dash || trail_dash) ? 0 : 1;
    }
    if (i < e) {                                              /* optional "of M" or "/ M" */
        size_t j = i;
        while (j < e && s[j] == ' ') j++;
        if (j + 1 < e && (s[j] | 32) == 'o' && (s[j + 1] | 32) == 'f') j += 2;
        else if (j < e && s[j] == '/') j++;
        else return 0;
        while (j < e && s[j] == ' ') j++;
        size_t m0 = j;
        while (j < e && is_digit(s[j]) && j - m0 < 4) j++;
        if (j == m0 || j != e) return 0;
        of = 1;
    }
    *value = v;
    if (word || of || (lead_dash && trail_dash)) return 2;
    if (lead_dash || trail_dash) return 0;
    return 1;
}

static void detect_pagenums(Ctx *c, Line *L, size_t nl)
{
    typedef struct { size_t line; long v; int type; } Cand;
    size_t cap = 64, n = 0;
    Cand *cand = (Cand *)malloc(cap * sizeof *cand);
    if (!cand) return;
    for (size_t i = 0; i < nl; i++) {
        if (L[i].del || L[i].a == L[i].b) continue;
        long v = 0;
        int type = 0;
        int k = parse_pagenum(c->t + L[i].a, L[i].b - L[i].a, &v, &type);
        if (k == 2) { L[i].del = CH_PAGENUM; continue; }
        if (k == 1) {
            if (n == cap) {
                Cand *p = (Cand *)realloc(cand, cap * 2 * sizeof *cand);
                if (!p) break;
                cand = p; cap *= 2;
            }
            cand[n].line = i; cand[n].v = v; cand[n].type = type; n++;
        }
    }
    /* Evidence comes only from a neighbour of the SAME kind (arabic with arabic, roman with roman). */
    long *prev_same = (long *)malloc((n ? n : 1) * sizeof *prev_same);
    long *next_same = (long *)malloc((n ? n : 1) * sizeof *next_same);
    if (prev_same && next_same) {
        long last[2] = { -1, -1 };
        for (size_t j = 0; j < n; j++) { prev_same[j] = last[cand[j].type]; last[cand[j].type] = (long)j; }
        last[0] = last[1] = -1;
        for (size_t j = n; j-- > 0;) { next_same[j] = last[cand[j].type]; last[cand[j].type] = (long)j; }
        for (size_t j = 0; j < n; j++) {
            int ok = 0;
            if (prev_same[j] >= 0) {
                const Cand *q = &cand[prev_same[j]];
                if (cand[j].v - q->v >= 1 && cand[j].v - q->v <= 3 && cand[j].line - q->line >= 6) ok = 1;
            }
            if (next_same[j] >= 0) {
                const Cand *q = &cand[next_same[j]];
                if (q->v - cand[j].v >= 1 && q->v - cand[j].v <= 3 && q->line - cand[j].line >= 6) ok = 1;
            }
            if (ok) L[cand[j].line].del = CH_PAGENUM;
        }
    }
    free(prev_same); free(next_same);
    free(cand);
}

/* ------------------------------------------------------ join / separator rules */

static int is_terminal(wchar_t c)
{
    return c == '.' || c == '!' || c == '?' || c == ':' || c == ';' || c == '"' || c == '\'' ||
           c == ')' || c == ']' || c == 0x201D || c == 0x2019 || c == 0x2026 || c == 0xBB;
}
static int is_hyphen_char(wchar_t c) { return c == '-' || c == 0x2010 || c == 0x2011 || c == 0xAD; }

static int looks_like_list_item(const wchar_t *t, const Line *N)
{
    size_t i = N->a, e = N->b;
    wchar_t c = t[i];
    if ((c == 0x2022 || c == 0x00B7 || c == 0x25AA || c == 0x2023 || c == 0x25E6 || c == 0x25CF ||
         c == '*' || c == '-' || c == 0x2013 || c == 0x2014 || c == '+') && i + 1 < e && t[i + 1] == ' ')
        return 1;
    size_t j = i;
    while (j < e && is_digit(t[j]) && j - i < 3) j++;
    if (j > i && j + 1 < e && (t[j] == '.' || t[j] == ')') && t[j + 1] == ' ') return 1;
    if (is_alpha(c) && i + 2 < e && t[i + 1] == ')' && t[i + 2] == ' ') return 1;
    if (c == '(' && i + 3 < e && (is_digit(t[i + 1]) || is_alpha(t[i + 1])) && t[i + 2] == ')' && t[i + 3] == ' ') return 1;
    return 0;
}

static int all_caps_heading(const wchar_t *t, const Line *N, size_t W)
{
    size_t n = N->b - N->a;
    if (n * 2 >= W) return 0;
    int letters = 0;
    for (size_t i = N->a; i < N->b; i++) {
        if (is_lower(t[i])) return 0;
        if (is_upper(t[i])) letters++;
    }
    return letters >= 3;
}

static int hyphen_keep_fragment(const wchar_t *t, size_t hy)      /* t[hy] is the hyphen */
{
    size_t e = hy, s = hy;
    while (s > 0 && is_alpha(t[s - 1])) s--;
    size_t n = e - s;
    if (n <= 1) return 1;                                         /* e-mail, x-ray */
    static const char *const keep[] = { "well", "self", "ex", "all", "half", "non", "quasi", "vice", NULL };
    char buf[8];
    if (n >= sizeof buf) return 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t ch = t[s + i];
        if (ch >= 'A' && ch <= 'Z') ch = (wchar_t)(ch + 32);
        if (ch >= 0x80) return 0;
        buf[i] = (char)ch;
    }
    buf[n] = 0;
    for (const char *const *k = keep; *k; k++) if (strcmp(*k, buf) == 0) return 1;
    return 0;
}

static int decide(const Ctx *c, int hard_wrapped, size_t W, const Line *L, const Line *N)
{
    const wchar_t *t = c->t;
    size_t ln = L->b - L->a;
    wchar_t last = t[L->b - 1], first = t[N->a];

    if (c->o.fix_hyphens && is_hyphen_char(last) && ln >= 2 &&
        (last == 0xAD || is_alpha(t[L->b - 2]))) {
        if (is_lower(first) || last == 0xAD)
            return (last != 0xAD && hyphen_keep_fragment(t, L->b - 1)) ? SEP_NOSPACE : SEP_DEHYPHEN;
        if (is_upper(first)) return SEP_NOSPACE;                  /* Anglo-\nSaxon */
    }
    if (!c->o.join_lines || !hard_wrapped) return SEP_NEWLINE;

    if (looks_like_list_item(t, N)) return SEP_PARA;
    if (N->a - N->s >= 2 && (N->a - N->s >= 4 || t[N->s] == '\t' || t[N->s + 1] == ' ')) return SEP_PARA; /* indented */
    if (is_terminal(last) && ln * 4 < W * 3) return SEP_PARA;     /* short line that ends a sentence */
    if (!is_terminal(last) && ln * 2 < W && (is_upper(first) || is_digit(first))) return SEP_PARA;  /* heading-like */
    if (all_caps_heading(t, N, W)) return SEP_PARA;
    return SEP_SPACE;
}

/* Is the text hard-wrapped (lines of near-constant width)? Sets *W to the wrap width. */
static int detect_wrap(const Ctx *c, const Line *L, size_t nl, size_t *W)
{
    size_t hist[257];
    memset(hist, 0, sizeof hist);
    size_t total = 0;
    for (size_t i = 0; i < nl; i++) {
        if (L[i].del || L[i].a == L[i].b) continue;
        size_t n = L[i].b - L[i].a;
        hist[n > 256 ? 256 : n]++;
        total++;
    }
    (void)c;
    if (total < 6) return 0;
    size_t acc = 0, p95 = 256, need = (total * 95 + 99) / 100;
    for (size_t k = 0; k <= 256; k++) { acc += hist[k]; if (acc >= need) { p95 = k; break; } }
    if (p95 < 30) return 0;
    size_t lo = (p95 * 85) / 100, band = 0;
    for (size_t k = lo; k <= p95; k++) band += hist[k];
    *W = p95;
    return band * 100 >= total * 35;
}

/* -------------------------------------------------------- content emission */

static int starts_with_ci(const wchar_t *t, size_t i, size_t end, const char *p)
{
    for (; *p; p++, i++) {
        if (i >= end) return 0;
        wchar_t c = t[i];
        if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
        if (c != (wchar_t)*p) return 0;
    }
    return 1;
}

static int url_end(const wchar_t *t, size_t i, size_t end, size_t *out)
{
    if (!(starts_with_ci(t, i, end, "http://") || starts_with_ci(t, i, end, "https://") ||
          starts_with_ci(t, i, end, "ftp://") || starts_with_ci(t, i, end, "www."))) return 0;
    size_t j = i;
    while (j < end && !is_space(t[j])) j++;
    while (j > i + 4 && (t[j - 1] == '.' || t[j - 1] == ',' || t[j - 1] == ';' || t[j - 1] == ':' ||
                         t[j - 1] == '!' || t[j - 1] == '?' || t[j - 1] == ')' || t[j - 1] == ']' ||
                         t[j - 1] == '"' || t[j - 1] == '\'' || t[j - 1] == 0x201D || t[j - 1] == 0x2019)) j--;
    if (j <= i + 4) return 0;
    *out = j;
    return 1;
}

static int citation_end(const wchar_t *t, size_t i, size_t end, size_t *out)
{
    if (t[i] != '[') return 0;
    size_t j = i + 1;
    int digits = 0;
    while (j < end && j - i < 24 && (is_digit(t[j]) || t[j] == ',' || t[j] == ';' || t[j] == '-' || t[j] == 0x2013 || t[j] == ' ')) {
        if (is_digit(t[j])) digits++;
        j++;
    }
    if (j >= end || t[j] != ']' || !digits || !is_digit(t[i + 1])) return 0;
    *out = j + 1;
    return 1;
}

static int is_after_delete_punct(wchar_t c) { return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')'; }

/* ------------------------------------------------------------ footnotes */

static int is_super_digit(wchar_t c)
{
    return c == 0xB2 || c == 0xB3 || c == 0xB9 || c == 0x2070 || (c >= 0x2074 && c <= 0x2079);
}

/* "[^12]" at t[i]; sets *out past it. */
static int fn_marker_end(const wchar_t *t, size_t i, size_t end, size_t *out)
{
    if (t[i] == '[' && i + 3 < end && t[i + 1] == '^') {
        size_t j = i + 2;
        while (j < end && j - i < 12 && is_alnum_c(t[j])) j++;
        if (j > i + 2 && j < end && t[j] == ']') { *out = j + 1; return 1; }
        return 0;
    }
    if (is_super_digit(t[i])) {
        size_t j = i;
        while (j < end && is_super_digit(t[j])) j++;
        *out = j;
        return 1;
    }
    return 0;
}

static void detect_footnotes(Ctx *c, Line *L, size_t nl)
{
    (void)c;
    for (size_t i = 0; i < nl; i++) {
        if (L[i].del || L[i].a == L[i].b) continue;
        const wchar_t *t = c->t;
        size_t a = L[i].a, b = L[i].b, j;
        int note = 0;
        if (t[a] == '[' && a + 3 < b && t[a + 1] == '^') {                  /* [^1]: text */
            j = a + 2;
            while (j < b && is_alnum_c(t[j])) j++;
            note = (j > a + 2 && j + 1 < b && t[j] == ']' && t[j + 1] == ':');
        } else if (is_super_digit(t[a])) {                                    /* superscript number, then text */
            j = a;
            while (j < b && is_super_digit(t[j])) j++;
            note = j < b && (is_space(t[j]) || is_alpha(t[j])) && b - j > 3;
        } else if ((t[a] == 0x2020 || t[a] == 0x2021) && a + 1 < b && is_space(t[a + 1])) {   /* dagger notes */
            note = 1;
        }
        if (note) L[i].del = CH_FOOTNOTE;
    }
}

/* ------------------------------------------------------------------ tables */

/* Number of cells in a line: separators are tab, '|', or a run of 3+ spaces between text. */
static int count_cells(const wchar_t *t, size_t a, size_t b)
{
    int cells = 0, in_cell = 0;
    size_t i = a;
    while (i < b) {
        wchar_t ch = t[i];
        if (ch == '\t' || ch == '|') { in_cell = 0; i++; continue; }
        if (ch == ' ') {
            size_t j = i;
            while (j < b && t[j] == ' ') j++;
            if (j - i >= 3 && j < b) { in_cell = 0; i = j; continue; }
            i = j;
            continue;
        }
        if (!in_cell) { cells++; in_cell = 1; }
        i++;
    }
    return cells;
}

static int is_rule_row(const wchar_t *t, size_t a, size_t b)      /* |---|---|  or  ---+---  or ==== */
{
    int dash = 0, sep = 0;
    for (size_t i = a; i < b; i++) {
        wchar_t ch = t[i];
        if (ch == '-' || ch == '=' || ch == 0x2500 || ch == 0x2550) dash++;
        else if (ch == '|' || ch == '+' || ch == ':' || ch == ' ' || ch == '\t' || ch == 0x2502 || ch == 0x253C) { if (ch == '|' || ch == '+') sep++; }
        else return 0;
    }
    return dash >= 3 && sep >= 1;
}

static void detect_tables(Ctx *c, Line *L, size_t nl)
{
    const wchar_t *t = c->t;
    size_t i = 0;
    while (i < nl) {
        if (L[i].del || L[i].a == L[i].b) { i++; continue; }
        size_t j = i;
        int rows = 0, same = 0, prev_cells = -1;
        while (j < nl && !L[j].del && L[j].a != L[j].b) {
            if (is_rule_row(t, L[j].a, L[j].b)) { j++; continue; }
            int cells = count_cells(t, L[j].a, L[j].b);
            if (cells < 2) break;
            rows++;
            if (cells == prev_cells) same++;
            prev_cells = cells;
            j++;
        }
        if (rows >= 2 && same >= 1) {
            for (size_t k = i; k < j; k++) {
                if (is_rule_row(t, L[k].a, L[k].b) || c->o.remove_tables) L[k].del = CH_TABLE;
                else L[k].tab = 1;
            }
            i = j;
        } else {
            i = j > i ? j : i + 1;
        }
    }
}

/* Emit a table row as "cell, cell, cell." */
static int emit_table_row(Ctx *c, const Line *L)
{
    const wchar_t *t = c->t;
    size_t a = L->a, b = L->b, i = a;
    while (i < b && (t[i] == '|' || t[i] == ' ' || t[i] == '\t')) i++;
    while (b > i && (t[b - 1] == '|' || t[b - 1] == ' ' || t[b - 1] == '\t')) b--;
    int pending = 0;
    while (i < b) {
        wchar_t ch = t[i];
        if (ch == '\t' || ch == '|' || ch == ' ') {
            size_t j = i;
            int sepf = 0;
            while (j < b && (t[j] == ' ' || t[j] == '\t' || t[j] == '|')) { if (t[j] != ' ' || (j - i >= 2)) sepf = 1; j++; }
            if (j - i >= 3 || sepf) {
                if (j < b) pending = 1;
                if (!log_change(c, CH_JOIN, i, j - i)) return 0;
                i = j;
                continue;
            }
            if (!emit1(c, i, ' ')) return 0;
            i++;
            continue;
        }
        if (pending) {
            wchar_t lo = last_out(c);
            if (lo != ',' && lo != '.' && lo != ';' && lo != ':') { if (!emit(c, i, 0, L", ", 2)) return 0; }
            else if (!emit(c, i, 0, L" ", 1)) return 0;
            pending = 0;
        }
        if (!emit1(c, i, ch)) return 0;
        i++;
    }
    wchar_t lo = last_out(c);
    if (lo != '.' && lo != '!' && lo != '?' && lo != ':' && lo != ';') { if (!emit(c, L->b, 0, L".", 1)) return 0; }
    return 1;
}

static int emit_content(Ctx *c, const Line *L, int drop_last)
{
    const wchar_t *t = c->t;
    const PreprocOptions *o = &c->o;
    PreprocResult *r = c->r;
    if (L->tab && o->table_rows && !drop_last) return emit_table_row(c, L);
    size_t start = o->normalize_ws ? L->a : L->s;
    size_t end = o->normalize_ws ? L->b : L->e;
    if (drop_last) end = L->b - 1;
    size_t i = start;
    while (i < end) {
        wchar_t ch = t[i];
        size_t j;
        if ((o->remove_urls && (i == start || is_space(t[i - 1]) || t[i - 1] == '(' || t[i - 1] == '[' || t[i - 1] == '<') &&
             url_end(t, i, end, &j)) ||
            (o->remove_citations && citation_end(t, i, end, &j))) {
            int is_url = (ch != '[');
            if (!log_change(c, is_url ? CH_URL : CH_CITATION, i, j - i)) return 0;
            if (is_url) r->n_urls++; else r->n_citations++;
            if (last_out(c) == ' ' && (j >= end || is_after_delete_punct(t[j]))) unemit_last(c);
            i = j;
            continue;
        }
        if (o->remove_footnotes && fn_marker_end(t, i, end, &j) && i > start) {
            if (!log_change(c, CH_FOOTNOTE, i, j - i)) return 0;
            r->n_footnotes++;
            i = j;
            continue;
        }
        if (o->normalize_unicode) {
            if (is_zero_width(ch) || ch == 0xAD || ch == 0xFFFD || (ch < 0x20 && ch != '\t') || ch == 0x7F) {
                r->n_unicode++; log_change(c, CH_UNICODE, i, 1); i++; continue;
            }
            if (ch >= 0xFB00 && ch <= 0xFB06) {
                static const wchar_t *const lig[] = { L"ff", L"fi", L"fl", L"ffi", L"ffl", L"st", L"st" };
                const wchar_t *x = lig[ch - 0xFB00];
                if (!emit(c, i, 1, x, wcslen(x))) return 0;
                r->n_unicode++; log_change(c, CH_UNICODE, i, 1); i++;
                continue;
            }
            if (ch == 0x2010 || ch == 0x2011) { ch = '-'; r->n_unicode++; log_change(c, CH_UNICODE, i, 1); }
            else if (ch != ' ' && is_space(ch)) { ch = ' '; r->n_unicode++; log_change(c, CH_UNICODE, i, 1); }
            else if (ch == '\t') ch = ' ';
        }
        if (ch == ' ' || ch == '\t') {
            if (o->normalize_ws) {
                wchar_t lo = last_out(c);
                if (r->len == 0 || lo == ' ' || lo == '\n') { i++; continue; }
            }
        }
        if (!emit1(c, i, ch)) return 0;
        i++;
    }
    return 1;
}

static int emit_separator(Ctx *c, const Line *prev, int kind, int blanks)
{
    PreprocResult *r = c->r;
    if (c->o.normalize_ws && last_out(c) == ' ') unemit_last(c);
    size_t sp = prev->e, sl = prev->tl ? (size_t)prev->tl : 1;
    switch (kind) {
    case SEP_NEWLINE: return emit(c, sp, sl, L"\n", 1);
    case SEP_SPACE:   r->n_joined++; log_change(c, CH_JOIN, sp, sl); return emit(c, sp, sl, L" ", 1);
    case SEP_NOSPACE: r->n_joined++; log_change(c, CH_JOIN, sp, sl); return 1;
    case SEP_DEHYPHEN: r->n_joined++; return 1;
    default: {
        int n = c->o.normalize_ws ? 2 : blanks + 1;
        for (int k = 0; k < n; k++) if (!emit(c, sp, sl, L"\n", 1)) return 0;
        return 1;
    }
    }
}

/* --------------------------------------------------------------------- run */

static int cmp_change(const void *x, const void *y)
{
    const Change *a = (const Change *)x, *b = (const Change *)y;
    return a->src_start < b->src_start ? -1 : (a->src_start > b->src_start);
}

int preproc_run(const wchar_t *t, size_t len, const PreprocOptions *oin, int log_changes, PreprocResult *r)
{
    Ctx c;
    memset(r, 0, sizeof *r);
    r->src_len = len;
    c.t = t; c.len = len; c.log = log_changes; c.r = r;
    c.o = *oin;
    if (!c.o.enabled) {
        int en = 0; memset(&c.o, 0, sizeof c.o); c.o.enabled = en;
    }
    size_t nl = 0;
    Line *L = scan_lines(t, len, &nl);
    size_t *K = NULL; unsigned char *sep = NULL; int *blanks = NULL;
    if (!L) goto oom;
    if (!grow_text(r, len + len / 8 + 64)) goto oom;
    r->text[0] = 0;

    if (c.o.repeated_lines && !detect_repeated(&c, L, nl)) goto oom;
    if (c.o.page_numbers) detect_pagenums(&c, L, nl);
    if (c.o.remove_footnotes) detect_footnotes(&c, L, nl);
    if (c.o.remove_tables || c.o.table_rows) detect_tables(&c, L, nl);

    /* content lines (kept, non-blank) and what sits between consecutive ones */
    K = (size_t *)malloc((nl ? nl : 1) * sizeof *K);
    sep = (unsigned char *)malloc(nl ? nl : 1);
    blanks = (int *)malloc((nl ? nl : 1) * sizeof *blanks);
    if (!K || !sep || !blanks) goto oom;
    size_t nk = 0;
    for (size_t i = 0; i < nl; i++) if (!L[i].del && L[i].a != L[i].b) K[nk++] = i;

    size_t W = 0;
    int hard = (c.o.join_lines && detect_wrap(&c, L, nl, &W)) ? 1 : 0;
    r->hard_wrapped = hard;
    for (size_t k = 0; k + 1 < nk; k++) {
        int nb = 0;
        for (size_t i = K[k] + 1; i < K[k + 1]; i++) if (!L[i].del) nb++;     /* kept blank lines */
        blanks[k] = nb;
        sep[k] = (unsigned char)(nb > 0 ? SEP_PARA : ((L[K[k]].tab || L[K[k + 1]].tab) && c.o.table_rows ? SEP_NEWLINE : decide(&c, hard, W, &L[K[k]], &L[K[k + 1]])));
    }

    size_t k = 0;
    for (size_t i = 0; i < nl; i++) {
        if (L[i].del) {
            if (!log_change(&c, L[i].del, L[i].a, L[i].b - L[i].a)) goto oom;
            if (L[i].del == CH_HEADER) r->n_headers++;
            else if (L[i].del == CH_PAGENUM) r->n_pagenums++;
            else if (L[i].del == CH_FOOTNOTE) r->n_footnotes++;
            else r->n_tables++;
            continue;
        }
        if (L[i].a == L[i].b) continue;
        if (k > 0 && !emit_separator(&c, &L[K[k - 1]], sep[k - 1], blanks[k - 1])) goto oom;
        int drop = (k + 1 < nk && (sep[k] == SEP_DEHYPHEN || (sep[k] == SEP_NOSPACE && t[L[i].b - 1] == 0xAD)));
        if (drop) {
            size_t hy = L[i].b - 1, ws = hy, we = L[K[k + 1]].a;
            while (ws > L[i].a && is_alpha(t[ws - 1])) ws--;
            while (we < L[K[k + 1]].b && is_alpha(t[we])) we++;
            r->n_hyphens++;
            if (c.log && !log_change(&c, CH_HYPHEN, ws, we - ws)) goto oom;
        }
        if (!emit_content(&c, &L[i], drop)) goto oom;
        k++;
    }
    r->text[r->len] = 0;
    if (r->nchanges > 1) qsort(r->changes, (size_t)r->nchanges, sizeof *r->changes, cmp_change);
    free(L); free(K); free(sep); free(blanks);
    return 1;
oom:
    free(L); free(K); free(sep); free(blanks);
    preproc_free(r);
    return 0;
}
