#include "chapters.h"
#include "textutil.h"
#include "plat.h"   /* _snwprintf shim for non-Windows hosts */
#include <stdlib.h>
#include <string.h>

static wchar_t *wdup(const wchar_t *s)
{
    size_t n = wcslen(s);
    wchar_t *p = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (p) memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}
void chapters_free(ChapterList *l)
{
    for (int i = 0; i < l->n; i++) free(l->c[i].title);
    free(l->c);
    memset(l, 0, sizeof *l);
}

void overrides_free(Overrides *o)
{
    for (int i = 0; i < o->n; i++) { free(o->o[i].key); free(o->o[i].title); }
    free(o->o);
    memset(o, 0, sizeof *o);
}

/* ------------------------------------------------------------- paragraph text */

/* Collapsed, trimmed text of paragraph p into out (cap chars, NUL-terminated). Returns length. */
static size_t para_text(const Document *d, int p, wchar_t *out, size_t cap)
{
    const Paragraph *pg = &d->paras[p];
    size_t k = 0;
    int sp = 0;
    for (size_t i = 0; i < pg->len && k + 1 < cap; i++) {
        wchar_t c = d->text[pg->start + i];
        if (is_space(c)) { sp = 1; continue; }
        if (sp && k) out[k++] = ' ';
        sp = 0;
        if (k + 1 < cap) out[k++] = c;
    }
    out[k] = 0;
    return k;
}

#define KEYCAP 160

static int occurrence_of(const Document *d, int para, const wchar_t *key)
{
    int occ = 0;
    wchar_t t[KEYCAP];
    for (int p = 0; p < para; p++) { para_text(d, p, t, KEYCAP); if (wcscmp(t, key) == 0) occ++; }
    return occ;
}

static int find_para(const Document *d, const wchar_t *key, int occurrence)
{
    wchar_t t[KEYCAP];
    int seen = 0;
    for (int p = 0; p < d->npara; p++) {
        para_text(d, p, t, KEYCAP);
        if (wcscmp(t, key) == 0 && seen++ == occurrence) return p;
    }
    return -1;
}

/* ------------------------------------------------------------------ headings */

static int tok_lower(const wchar_t *s, size_t n, char *out, size_t cap)
{
    if (n == 0 || n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
        if (c >= 0x80 || c == 0) return 0;
        out[i] = (char)c;
    }
    out[n] = 0;
    return 1;
}

static int is_roman_word(const wchar_t *s, size_t n)
{
    if (n == 0 || n > 8) return 0;
    int lower = s[0] >= 'a' && s[0] <= 'z';
    for (size_t i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (lower ? (c >= 'A' && c <= 'Z') : (c >= 'a' && c <= 'z')) return 0;
        if (c >= 'a' && c <= 'z') c = (wchar_t)(c - 32);
        if (!(c == 'I' || c == 'V' || c == 'X' || c == 'L' || c == 'C' || c == 'D' || c == 'M')) return 0;
    }
    /* reject non-canonical forms such as IIII / VX via a value round trip */
    static const struct { int v; const char *s; } T[] = { {1000,"M"},{900,"CM"},{500,"D"},{400,"CD"},{100,"C"},{90,"XC"},{50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"} };
    long v = 0, prev = 0;
    for (size_t k = n; k-- > 0;) {
        wchar_t c = s[k];
        if (c >= 'a') c = (wchar_t)(c - 32);
        long dv = c == 'I' ? 1 : c == 'V' ? 5 : c == 'X' ? 10 : c == 'L' ? 50 : c == 'C' ? 100 : c == 'D' ? 500 : 1000;
        v += dv < prev ? -dv : dv;
        if (dv >= prev) prev = dv;
    }
    if (v < 1 || v > 3999) return 0;
    char canon[32];
    size_t cn = 0;
    long x = v;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) while (x >= T[i].v) { for (const char *q = T[i].s; *q; q++) canon[cn++] = *q; x -= T[i].v; }
    if (cn != n) return 0;
    for (size_t i = 0; i < n; i++) { wchar_t c = s[i]; if (c >= 'a') c = (wchar_t)(c - 32); if (c != (wchar_t)canon[i]) return 0; }
    return 1;
}

static int is_number_word(const wchar_t *s, size_t n)
{
    static const char *const W[] = { "one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve",
        "thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen","twenty","thirty","forty","fifty",
        "first","second","third","fourth","fifth","sixth","seventh","eighth","ninth","tenth","eleventh","twelfth",
        "twentieth","last","final", NULL };
    char b[16];
    if (!tok_lower(s, n, b, sizeof b)) return 0;
    for (const char *const *w = W; *w; w++) if (strcmp(*w, b) == 0) return 1;
    return 0;
}

static int is_number_token(const wchar_t *s, size_t n)
{
    if (n == 0) return 0;
    int digits = 1;
    for (size_t i = 0; i < n; i++) if (!is_digit(s[i])) digits = 0;
    if (digits) return n <= 4;
    return is_roman_word(s, n) || is_number_word(s, n);
}

static int count_words(const wchar_t *t)
{
    int w = 0, in = 0;
    for (; *t; t++) { if (is_space(*t)) in = 0; else if (!in) { in = 1; w++; } }
    return w;
}

static int ends_sentence(const wchar_t *t)
{
    size_t n = wcslen(t);
    return n && (t[n - 1] == '.' || t[n - 1] == '!' || t[n - 1] == '?' || t[n - 1] == ',' || t[n - 1] == ';');
}

/* 0 = not a heading, 1 = chapter-like, 2 = part/book, 3 = special (prologue...), 4 = bare roman numeral */
static int classify(const wchar_t *t, int *needs_title)
{
    size_t n = wcslen(t);
    *needs_title = 0;
    if (n == 0 || n > 90 || count_words(t) > 12) return 0;

    size_t a = 0;
    while (a < n && !is_space(t[a]) && t[a] != ':' && t[a] != '.' && t[a] != '-') a++;
    char kw[16];
    int have_kw = tok_lower(t, a, kw, sizeof kw);

    if (have_kw && (!strcmp(kw, "chapter") || !strcmp(kw, "part") || !strcmp(kw, "book"))) {
        size_t b = a;
        while (b < n && is_space(t[b])) b++;
        size_t e = b;
        while (e < n && !is_space(t[e]) && t[e] != ':' && t[e] != '.' && t[e] != '-' && t[e] != 0x2013 && t[e] != 0x2014 && t[e] != ')') e++;
        if (!is_number_token(t + b, e - b)) return 0;
        size_t r = e;                                           /* remainder = optional subtitle */
        while (r < n && (is_space(t[r]) || t[r] == ':' || t[r] == '.' || t[r] == '-' || t[r] == 0x2013 || t[r] == 0x2014 || t[r] == ')')) r++;
        if (r < n) {
            if (count_words(t + r) > 9 || ends_sentence(t + r)) return 0;
        } else {
            *needs_title = 1;
        }
        return strcmp(kw, "chapter") == 0 ? 1 : 2;
    }
    if (have_kw) {
        static const char *const SPECIAL[] = { "prologue", "epilogue", "preface", "foreword", "introduction", "afterword",
            "acknowledgments", "acknowledgements", "conclusion", "interlude", NULL };
        for (const char *const *s = SPECIAL; *s; s++)
            if (!strcmp(*s, kw) && (a == n || ((t[a] == ':' || t[a] == '.') && a + 1 >= n))) return 3;
        if (!strcmp(kw, "appendix")) {
            size_t b = a;
            while (b < n && is_space(t[b])) b++;
            if (b == n) return 3;
            size_t e = b;
            while (e < n && !is_space(t[e]) && t[e] != ':') e++;
            if (e - b <= 3 && count_words(t) <= 8 && !ends_sentence(t)) return 3;
        }
    }
    /* bare roman numeral, optionally with a trailing period: "XIV."  "II" */
    size_t m = n;
    if (m > 1 && t[m - 1] == '.') m--;
    if (is_roman_word(t, m)) return 4;
    return 0;
}

static int push_chapter(ChapterList *l, int para, int first_sent, const wchar_t *title, int level, int manual)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        Chapter *p = (Chapter *)realloc(l->c, (size_t)nc * sizeof *p);
        if (!p) return 0;
        l->c = p; l->cap = nc;
    }
    Chapter *c = &l->c[l->n];
    c->para = para; c->first_sent = first_sent; c->level = level; c->manual = manual;
    c->title = wdup(title);
    if (!c->title) return 0;
    l->n++;
    return 1;
}

static int cmp_chapter(const void *x, const void *y)
{
    const Chapter *a = (const Chapter *)x, *b = (const Chapter *)y;
    return a->para - b->para;
}

int chapters_detect(ChapterList *out, const Document *d, const Overrides *ov)
{
    memset(out, 0, sizeof *out);
    wchar_t t[KEYCAP], nx[KEYCAP], title[2 * KEYCAP + 8];
    int part_para = -1, part_sent = 0;
    wchar_t part_title[KEYCAP];
    part_title[0] = 0;

    for (int p = 0; p < d->npara; p++) {
        if (d->paras[p].sentence_count == 0) continue;
        para_text(d, p, t, KEYCAP);
        int needs = 0;
        int k = classify(t, &needs);
        if (!k) continue;
        wcscpy(title, t);
        if (needs && p + 1 < d->npara) {                         /* "CHAPTER 1" + next short paragraph as its title */
            para_text(d, p + 1, nx, KEYCAP);
            int nneeds = 0;
            if (nx[0] && wcslen(nx) <= 60 && count_words(nx) <= 8 && !ends_sentence(nx) && !classify(nx, &nneeds) &&
                (is_upper(nx[0]) || is_digit(nx[0]))) {
                _snwprintf(title, sizeof title / sizeof(wchar_t), L"%ls - %ls", t, nx);
                title[sizeof title / sizeof(wchar_t) - 1] = 0;
            }
        }
        if (k == 2) {                                            /* PART / BOOK: wait to see whether a chapter follows */
            part_para = p; part_sent = d->paras[p].first_sentence;
            wcsncpy(part_title, title, KEYCAP - 1); part_title[KEYCAP - 1] = 0;
            continue;
        }
        int cp = p, cs = d->paras[p].first_sentence;
        if (part_para >= 0 && p - part_para <= 2) { cp = part_para; cs = part_sent; }
        else if (part_para >= 0) {                               /* a part with no chapter right after it stands alone */
            if (!push_chapter(out, part_para, part_sent, part_title, 1, 0)) { chapters_free(out); return 0; }
        }
        part_para = -1;
        if (!push_chapter(out, cp, cs, title, k == 1 ? 0 : 0, 0)) { chapters_free(out); return 0; }
    }
    if (part_para >= 0 && !push_chapter(out, part_para, part_sent, part_title, 1, 0)) { chapters_free(out); return 0; }

    if (ov) {
        for (int i = 0; i < ov->n; i++) {
            const ChapterOverride *o = &ov->o[i];
            int p = find_para(d, o->key, o->occurrence);
            if (p < 0 || d->paras[p].sentence_count == 0) continue;
            int idx = -1;
            for (int c = 0; c < out->n; c++) if (out->c[c].para == p) idx = c;
            if (o->kind == CO_REMOVE) {
                if (idx >= 0) {
                    free(out->c[idx].title);
                    memmove(&out->c[idx], &out->c[idx + 1], (size_t)(out->n - idx - 1) * sizeof(Chapter));
                    out->n--;
                }
            } else if (o->kind == CO_ADD) {
                if (idx < 0) {
                    para_text(d, p, t, KEYCAP);
                    if (!push_chapter(out, p, d->paras[p].first_sentence, o->title && o->title[0] ? o->title : t, 0, 1)) { chapters_free(out); return 0; }
                } else if (o->title && o->title[0]) {
                    wchar_t *nt = wdup(o->title);
                    if (nt) { free(out->c[idx].title); out->c[idx].title = nt; }
                }
            } else if (o->kind == CO_RENAME && idx >= 0 && o->title && o->title[0]) {
                wchar_t *nt = wdup(o->title);
                if (nt) { free(out->c[idx].title); out->c[idx].title = nt; }
            }
        }
        if (out->n > 1) qsort(out->c, (size_t)out->n, sizeof(Chapter), cmp_chapter);
    }
    return 1;
}

int chapter_of_sentence(const ChapterList *l, int sent)
{
    int lo = 0, hi = l->n - 1, ans = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (l->c[mid].first_sent <= sent) { ans = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return ans;
}

int chapter_first_sentence(const ChapterList *l, const Document *d, int ch)
{
    if (ch < 0) return d->nsent ? 0 : -1;
    return ch < l->n ? l->c[ch].first_sent : -1;
}

int chapter_last_sentence(const ChapterList *l, const Document *d, int ch)
{
    if (ch + 1 < l->n) return l->c[ch + 1].first_sent - 1;
    return d->nsent - 1;
}

/* ---------------------------------------------------------------- overrides */

static int ov_push(Overrides *o, int kind, const wchar_t *key, int occ, const wchar_t *title)
{
    for (int i = 0; i < o->n; i++)                                /* one record per heading: replace it */
        if (o->o[i].occurrence == occ && wcscmp(o->o[i].key, key) == 0) {
            if (o->o[i].kind == CO_ADD && kind == CO_REMOVE) {   /* forgetting a manual heading */
                free(o->o[i].key); free(o->o[i].title);
                memmove(&o->o[i], &o->o[i + 1], (size_t)(o->n - i - 1) * sizeof(ChapterOverride));
                o->n--;
                return 1;
            }
            wchar_t *nt = title ? wdup(title) : NULL;
            free(o->o[i].title);
            o->o[i].title = nt;
            o->o[i].kind = (o->o[i].kind == CO_ADD && kind == CO_RENAME) ? CO_ADD : kind;
            return 1;
        }
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 16;
        ChapterOverride *p = (ChapterOverride *)realloc(o->o, (size_t)nc * sizeof *p);
        if (!p) return 0;
        o->o = p; o->cap = nc;
    }
    ChapterOverride *c = &o->o[o->n];
    c->kind = kind; c->occurrence = occ;
    c->key = wdup(key);
    c->title = title ? wdup(title) : NULL;
    if (!c->key) return 0;
    o->n++;
    return 1;
}

int overrides_add_heading(Overrides *o, const Document *d, int para, const wchar_t *title)
{
    if (para < 0 || para >= d->npara) return 0;
    wchar_t key[KEYCAP];
    para_text(d, para, key, KEYCAP);
    return ov_push(o, CO_ADD, key, occurrence_of(d, para, key), title);
}

int overrides_remove_heading(Overrides *o, const Document *d, int para)
{
    if (para < 0 || para >= d->npara) return 0;
    wchar_t key[KEYCAP];
    para_text(d, para, key, KEYCAP);
    return ov_push(o, CO_REMOVE, key, occurrence_of(d, para, key), NULL);
}

int overrides_rename(Overrides *o, const Document *d, int para, const wchar_t *title)
{
    if (para < 0 || para >= d->npara || !title || !*title) return 0;
    wchar_t key[KEYCAP];
    para_text(d, para, key, KEYCAP);
    return ov_push(o, CO_RENAME, key, occurrence_of(d, para, key), title);
}

void overrides_to_ini(const Overrides *o, Ini *d)
{
    ini_remove_section(d, L"ChapterFixes");
    ini_set_int(d, L"ChapterFixes", L"Count", o->n);
    for (int i = 0; i < o->n; i++) {
        wchar_t k[32];
        swprintf(k, 32, L"Kind%d", i + 1);   ini_set_int(d, L"ChapterFixes", k, o->o[i].kind);
        swprintf(k, 32, L"Text%d", i + 1);   ini_set(d, L"ChapterFixes", k, o->o[i].key);
        swprintf(k, 32, L"Occ%d", i + 1);    ini_set_int(d, L"ChapterFixes", k, o->o[i].occurrence);
        swprintf(k, 32, L"Title%d", i + 1);  ini_set(d, L"ChapterFixes", k, o->o[i].title ? o->o[i].title : L"");
    }
}

void overrides_from_ini(Overrides *o, const Ini *d)
{
    overrides_free(o);
    long n = ini_get_int(d, L"ChapterFixes", L"Count", 0);
    for (long i = 1; i <= n && i < 10000; i++) {
        wchar_t k[32];
        swprintf(k, 32, L"Kind%ld", i);  long kind = ini_get_int(d, L"ChapterFixes", k, 0);
        swprintf(k, 32, L"Text%ld", i);  const wchar_t *text = ini_get(d, L"ChapterFixes", k);
        swprintf(k, 32, L"Occ%ld", i);   long occ = ini_get_int(d, L"ChapterFixes", k, 0);
        swprintf(k, 32, L"Title%ld", i); const wchar_t *title = ini_get(d, L"ChapterFixes", k);
        if (kind < CO_ADD || kind > CO_RENAME || !text || !*text) continue;
        ov_push(o, (int)kind, text, (int)occ, title && *title ? title : NULL);
    }
}

/* ---------------------------------------------------------------- file names */

void chapter_filename(wchar_t *out, size_t cap, int ordinal, const wchar_t *title)
{
    wchar_t t[96];
    size_t k = 0;
    for (const wchar_t *p = title ? title : L""; *p && k < 80; p++) {
        wchar_t c = *p;
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
        if (c == ' ' && k && t[k - 1] == ' ') continue;
        t[k++] = c;
    }
    while (k && (t[k - 1] == ' ' || t[k - 1] == '.' || t[k - 1] == '_')) k--;
    t[k] = 0;
    size_t s = 0;
    while (t[s] == ' ' || t[s] == '.') s++;
    static const char *const RES[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3", NULL };
    int reserved = 0;
    for (const char *const *r = RES; *r; r++) {
        size_t rl = strlen(*r);
        if (k - s == rl) {
            size_t i = 0;
            for (; i < rl; i++) { wchar_t c = t[s + i]; if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32); if (c != (wchar_t)(*r)[i]) break; }
            if (i == rl) reserved = 1;
        }
    }
    if (k - s == 0) swprintf(out, cap, L"%02d - Chapter %d.wav", ordinal, ordinal);
    else swprintf(out, cap, L"%02d - %ls%ls.wav", ordinal, reserved ? L"_" : L"", t + s);
    out[cap - 1] = 0;
}

/* ---------------------------------------------------------------- bookmarks */

#define SNIP 48
int bm_resolve(const Bookmark *b, const Document *d, const ChapterList *chs);

static void snippet_of(const Document *d, int sent, wchar_t *out)
{
    wchar_t *t = doc_speech_text(d, sent);
    size_t n = t ? wcslen(t) : 0;
    if (n > SNIP - 1) n = SNIP - 1;
    if (t) memcpy(out, t, n * sizeof(wchar_t));
    out[n] = 0;
    free(t);
}

static int snippet_occurrence(const Document *d, int sent, const wchar_t *snip)
{
    int occ = 0;
    wchar_t s[SNIP];
    for (int i = 0; i < sent; i++) { snippet_of(d, i, s); if (wcscmp(s, snip) == 0) occ++; }
    return occ;
}

void bm_destroy(Bookmark *b) { free(b->label); free(b->snippet); free(b->note); memset(b, 0, sizeof *b); }

static const wchar_t *const kind_names[MK_COUNT] = { L"Bookmark", L"Revisit", L"Important", L"Unclear", L"Question", L"Doubt" };
static const wchar_t *const kind_glyphs[MK_COUNT] = { L"", L"R", L"!", L"~", L"?", L"x" };
const wchar_t *mark_kind_name(int k) { return (k >= 0 && k < MK_COUNT) ? kind_names[k] : L"?"; }
const wchar_t *mark_kind_glyph(int k) { return (k >= 0 && k < MK_COUNT) ? kind_glyphs[k] : L"?"; }

int bm_set_note(Bookmark *b, const wchar_t *note)
{
    free(b->note);
    b->note = NULL;
    if (!note || !*note) return 1;
    wchar_t *n = wdup(note);
    if (!n) return 0;
    for (wchar_t *p = n; *p; p++) if (*p == L'\r' || *p == L'\n' || *p == L'\t') *p = L' ';
    b->note = n;
    return 1;
}

void bm_clear(BookmarkList *l)
{
    for (int i = 0; i < l->n; i++) bm_destroy(&l->b[i]);
    l->n = 0;
}

void bm_free(BookmarkList *l) { bm_clear(l); free(l->b); memset(l, 0, sizeof *l); }

int bm_make(Bookmark *b, const Document *d, const ChapterList *chs, int sent, const wchar_t *label)
{
    memset(b, 0, sizeof *b);
    if (sent < 0 || sent >= d->nsent) return 0;
    int para = d->sents[sent].para;
    int ch = chs ? chapter_of_sentence(chs, sent) : -1;
    int ch_para = ch >= 0 ? chs->c[ch].para : 0;
    b->chapter = ch;
    b->para_in_ch = para - ch_para;
    b->sent_in_para = sent - d->paras[para].first_sentence;
    wchar_t snip[SNIP];
    snippet_of(d, sent, snip);
    b->snippet = wdup(snip);
    b->occurrence = snippet_occurrence(d, sent, snip);
    wchar_t auto_label[160];
    if (label && *label) wcsncpy(auto_label, label, 159);
    else swprintf(auto_label, 160, L"Ch %d, para %d, sentence %d: %.40ls", ch + 1, b->para_in_ch + 1, b->sent_in_para + 1, snip);
    auto_label[159] = 0;
    b->label = wdup(auto_label);
    if (!b->snippet || !b->label) { bm_destroy(b); return 0; }
    return 1;
}

int bm_add(BookmarkList *l, const Document *d, const ChapterList *chs, int sent, const wchar_t *label)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        Bookmark *p = (Bookmark *)realloc(l->b, (size_t)nc * sizeof *p);
        if (!p) return -1;
        l->b = p; l->cap = nc;
    }
    if (!bm_make(&l->b[l->n], d, chs, sent, label)) return -1;
    return l->n++;
}

int bm_find_kind_at(const BookmarkList *l, const Document *d, const ChapterList *chs, int sent, int kind)
{
    for (int i = 0; i < l->n; i++) if (l->b[i].kind == kind && bm_resolve(&l->b[i], d, chs) == sent) return i;
    return -1;
}

int bm_add_kind(BookmarkList *l, const Document *d, const ChapterList *chs, int sent, int kind, const wchar_t *note)
{
    if (kind < 0 || kind >= MK_COUNT) return -1;
    if (bm_find_kind_at(l, d, chs, sent, kind) >= 0) return -2;
    int i = bm_add(l, d, chs, sent, NULL);
    if (i < 0) return -1;
    l->b[i].kind = kind;
    if (!bm_set_note(&l->b[i], note)) { bm_remove(l, i); return -1; }
    return i;
}

void bm_remove(BookmarkList *l, int i)
{
    if (i < 0 || i >= l->n) return;
    bm_destroy(&l->b[i]);
    memmove(&l->b[i], &l->b[i + 1], (size_t)(l->n - i - 1) * sizeof(Bookmark));
    l->n--;
}

int bm_resolve(const Bookmark *b, const Document *d, const ChapterList *chs)
{
    wchar_t s[SNIP];
    /* 1. structural position */
    int ch_para = 0, ch_end_para = d->npara, ok = 1;
    if (b->chapter >= 0) {
        if (!chs || b->chapter >= chs->n) ok = 0;
        else {
            ch_para = chs->c[b->chapter].para;
            if (b->chapter + 1 < chs->n) ch_end_para = chs->c[b->chapter + 1].para;
        }
    } else if (chs && chs->n) ch_end_para = chs->c[0].para;
    if (ok) {
        int p = ch_para + b->para_in_ch;
        if (p >= 0 && p < ch_end_para && p < d->npara && b->sent_in_para < d->paras[p].sentence_count) {
            int sent = d->paras[p].first_sentence + b->sent_in_para;
            snippet_of(d, sent, s);
            if (wcscmp(s, b->snippet) == 0) return sent;
        }
    }
    /* 2. the sentence moved: find it by its opening words (the occurrence-th match, else the nearest) */
    int seen = 0, first = -1, best = -1;
    for (int i = 0; i < d->nsent; i++) {
        snippet_of(d, i, s);
        if (wcscmp(s, b->snippet) != 0) continue;
        if (first < 0) first = i;
        if (seen++ == b->occurrence) return i;
        best = i;
    }
    return seen ? (best >= 0 ? best : first) : -1;
}

int bm_find_at(const BookmarkList *l, const Document *d, const ChapterList *chs, int sent)
{
    for (int i = 0; i < l->n; i++) if (bm_resolve(&l->b[i], d, chs) == sent) return i;
    return -1;
}

int bm_next(const BookmarkList *l, const Document *d, const ChapterList *chs, int from, int dir)
{
    return bm_next_mask(l, d, chs, from, dir, ~0u);
}

int bm_next_mask(const BookmarkList *l, const Document *d, const ChapterList *chs, int from, int dir, unsigned mask)
{
    int best = -1, best_s = 0, wrap = -1, wrap_s = 0;
    for (int i = 0; i < l->n; i++) {
        if (!(mask & (1u << l->b[i].kind))) continue;
        int s = bm_resolve(&l->b[i], d, chs);
        if (s < 0) continue;
        if (dir > 0) {
            if (s > from && (best < 0 || s < best_s)) { best = i; best_s = s; }
            if (wrap < 0 || s < wrap_s) { wrap = i; wrap_s = s; }
        } else {
            if (s < from && (best < 0 || s > best_s)) { best = i; best_s = s; }
            if (wrap < 0 || s > wrap_s) { wrap = i; wrap_s = s; }
        }
    }
    return best >= 0 ? best : wrap;
}

void bm_anchor_to_ini(const Bookmark *b, Ini *d, const wchar_t *sec)
{
    ini_remove_section(d, sec);
    ini_set_int(d, sec, L"Chapter", b->chapter);
    ini_set_int(d, sec, L"Paragraph", b->para_in_ch);
    ini_set_int(d, sec, L"Sentence", b->sent_in_para);
    ini_set_int(d, sec, L"Occurrence", b->occurrence);
    ini_set(d, sec, L"Snippet", b->snippet ? b->snippet : L"");
    ini_set(d, sec, L"Label", b->label ? b->label : L"");
    ini_set_int(d, sec, L"Kind", b->kind);
    ini_set(d, sec, L"Note", b->note ? b->note : L"");
}

int bm_anchor_from_ini(Bookmark *b, const Ini *d, const wchar_t *sec)
{
    memset(b, 0, sizeof *b);
    const wchar_t *snip = ini_get(d, sec, L"Snippet");
    if (!snip || !*snip) return 0;
    b->chapter = (int)ini_get_int(d, sec, L"Chapter", -1);
    b->para_in_ch = (int)ini_get_int(d, sec, L"Paragraph", 0);
    b->sent_in_para = (int)ini_get_int(d, sec, L"Sentence", 0);
    b->occurrence = (int)ini_get_int(d, sec, L"Occurrence", 0);
    b->snippet = wdup(snip);
    const wchar_t *lab = ini_get(d, sec, L"Label");
    b->label = wdup(lab ? lab : L"");
    b->kind = (int)ini_get_int(d, sec, L"Kind", 0);
    if (b->kind < 0 || b->kind >= MK_COUNT) b->kind = 0;
    const wchar_t *nt = ini_get(d, sec, L"Note");
    if (nt && *nt) b->note = wdup(nt);
    if (!b->snippet || !b->label) { bm_destroy(b); return 0; }
    return 1;
}

void bm_to_ini(const BookmarkList *l, Ini *d)
{
    for (int i = 1; i < 100000; i++) {                         /* drop old numbered sections */
        wchar_t sec[32];
        swprintf(sec, 32, L"Bookmark%d", i);
        if (!ini_get(d, sec, L"Snippet")) break;
        ini_remove_section(d, sec);
    }
    ini_remove_section(d, L"Bookmarks");
    ini_set_int(d, L"Bookmarks", L"Count", l->n);
    for (int i = 0; i < l->n; i++) {
        wchar_t sec[32];
        swprintf(sec, 32, L"Bookmark%d", i + 1);
        bm_anchor_to_ini(&l->b[i], d, sec);
    }
}

void bm_from_ini(BookmarkList *l, const Ini *d)
{
    bm_clear(l);
    long n = ini_get_int(d, L"Bookmarks", L"Count", 0);
    for (long i = 1; i <= n && i < 100000; i++) {
        wchar_t sec[32];
        swprintf(sec, 32, L"Bookmark%ld", i);
        Bookmark b;
        if (!bm_anchor_from_ini(&b, d, sec)) continue;
        if (l->n == l->cap) {
            int nc = l->cap ? l->cap * 2 : 16;
            Bookmark *p = (Bookmark *)realloc(l->b, (size_t)nc * sizeof *p);
            if (!p) { bm_destroy(&b); break; }
            l->b = p; l->cap = nc;
        }
        l->b[l->n++] = b;
    }
}
