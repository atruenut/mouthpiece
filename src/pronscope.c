#include "pronscope.h"
#include "textutil.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SEP 0x1F
#define SNIP 48

static int ieq(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *b; a++, b++) if (fold_char(*a) != fold_char(*b)) return 0;
    return *a == *b;
}

/* Opening characters of sentence i, whitespace collapsed, case-folded, at most SNIP chars. */
static size_t snippet(const Document *d, int i, wchar_t *out)
{
    const Sentence *s = &d->sents[i];
    size_t n = 0;
    int sp = 0;
    for (size_t k = 0; k < s->len && n < SNIP; k++) {
        wchar_t c = d->text[s->start + k];
        if (c <= ' ') { sp = 1; continue; }
        if (is_zero_width(c)) continue;
        if (sp && n) out[n++] = ' ';
        sp = 0;
        if (n < SNIP) out[n++] = fold_char(c);
    }
    out[n] = 0;
    return n;
}

wchar_t *pronscope_chapter(const wchar_t *title)
{
    size_t l = wcslen(title);
    wchar_t *s = (wchar_t *)malloc((l + 9) * sizeof(wchar_t));
    if (!s) return NULL;
    wcscpy(s, L"chapter:");
    wcscpy(s + 8, title);
    return s;
}

static int occurrence_of(const Document *d, int sent, const wchar_t *snip)
{
    wchar_t b[SNIP + 2];
    int occ = 0;
    for (int i = 0; i < sent; i++) { snippet(d, i, b); if (wcscmp(b, snip) == 0) occ++; }
    return occ;
}

wchar_t *pronscope_range(const Document *d, int first, int last)
{
    if (first < 0 || last < first || last >= d->nsent) return NULL;
    wchar_t a[SNIP + 2], b[SNIP + 2];
    if (!snippet(d, first, a) || !snippet(d, last, b)) return NULL;
    int oa = occurrence_of(d, first, a), ob = occurrence_of(d, last, b);
    wchar_t *s = (wchar_t *)malloc((2 * SNIP + 40) * sizeof(wchar_t));
    if (!s) return NULL;
    swprintf(s, 2 * SNIP + 40, L"range:%ls%lc%d%lc%ls%lc%d", a, (wchar_t)SEP, oa, (wchar_t)SEP, b, (wchar_t)SEP, ob);
    return s;
}

static int find_snip(const Document *d, const wchar_t *snip, int occ)
{
    wchar_t b[SNIP + 2];
    int seen = 0;
    for (int i = 0; i < d->nsent; i++) {
        snippet(d, i, b);
        if (wcscmp(b, snip) == 0) { if (seen == occ) return i; seen++; }
    }
    return -1;
}

/* parse "A<SEP>n<SEP>B<SEP>m" */
static int parse_range(const wchar_t *s, wchar_t *a, int *oa, wchar_t *b, int *ob)
{
    const wchar_t *p1 = wcschr(s, (wchar_t)SEP);
    if (!p1) return 0;
    const wchar_t *p2 = wcschr(p1 + 1, (wchar_t)SEP);
    if (!p2) return 0;
    const wchar_t *p3 = wcschr(p2 + 1, (wchar_t)SEP);
    if (!p3) return 0;
    size_t la = (size_t)(p1 - s), lb = (size_t)(p3 - p2 - 1);
    if (la > SNIP || lb > SNIP) return 0;
    memcpy(a, s, la * sizeof(wchar_t)); a[la] = 0;
    memcpy(b, p2 + 1, lb * sizeof(wchar_t)); b[lb] = 0;
    *oa = (int)wcstol(p1 + 1, NULL, 10);
    *ob = (int)wcstol(p3 + 1, NULL, 10);
    return 1;
}

void pron_resolve_scopes(PronDict *d, const Document *doc, const ChapterList *chs)
{
    for (int k = 0; k < d->n; k++) {
        PronEntry *e = &d->e[k];
        e->s_first = 0; e->s_last = -1;
        if (!e->scope || !e->scope[0]) continue;
        if (!wcsncmp(e->scope, L"chapter:", 8)) {
            const wchar_t *title = e->scope + 8;
            for (int c = 0; chs && c < chs->n; c++)
                if (chs->c[c].title && ieq(chs->c[c].title, title)) {
                    e->s_first = chapter_first_sentence(chs, doc, c);
                    e->s_last = chapter_last_sentence(chs, doc, c);
                    break;
                }
        } else if (!wcsncmp(e->scope, L"range:", 6)) {
            wchar_t a[SNIP + 2], b[SNIP + 2];
            int oa, ob;
            if (parse_range(e->scope + 6, a, &oa, b, &ob)) {
                int f = find_snip(doc, a, oa), l = find_snip(doc, b, ob);
                if (f >= 0 && l >= f) { e->s_first = f; e->s_last = l; }
            }
        }
    }
}

void pronscope_describe(const PronEntry *e, wchar_t *out, size_t cap)
{
    if (!e->scope || !e->scope[0]) { _snwprintf(out, cap, L"everywhere"); }
    else if (!wcsncmp(e->scope, L"chapter:", 8)) _snwprintf(out, cap, L"chapter \"%ls\"%ls", e->scope + 8, e->s_first > e->s_last ? L" (not found)" : L"");
    else if (!wcsncmp(e->scope, L"range:", 6)) {
        wchar_t a[SNIP + 2], b[SNIP + 2];
        int oa, ob;
        if (parse_range(e->scope + 6, a, &oa, b, &ob))
            _snwprintf(out, cap, L"selection \"%ls...\"%ls", a, e->s_first > e->s_last ? L" (text changed - not applied)" : L"");
        else _snwprintf(out, cap, L"selection (unreadable)");
    } else _snwprintf(out, cap, L"unknown scope");
    out[cap - 1] = 0;
}
