#include "navmath.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

void poshist_clear(PosHist *h) { memset(h, 0, sizeof *h); }

static void push(int *arr, int *n, int v)
{
    if (*n > 0 && arr[*n - 1] == v) return;
    if (*n == POSHIST_MAX) { memmove(arr, arr + 1, (POSHIST_MAX - 1) * sizeof(int)); (*n)--; }
    arr[(*n)++] = v;
}

void poshist_jump(PosHist *h, int from)
{
    if (from < 0) return;
    push(h->back, &h->nb, from);
    h->nf = 0;
}

int poshist_back(PosHist *h, int cur)
{
    while (h->nb > 0) {
        int t = h->back[--h->nb];
        if (t == cur) continue;
        if (cur >= 0) push(h->fwd, &h->nf, cur);
        return t;
    }
    return -1;
}

int poshist_forward(PosHist *h, int cur)
{
    while (h->nf > 0) {
        int t = h->fwd[--h->nf];
        if (t == cur) continue;
        if (cur >= 0) push(h->back, &h->nb, cur);
        return t;
    }
    return -1;
}

double nav_chars_per_sec(int rate)
{
    double f = 1.0 + rate * 0.07;
    if (f < 0.3) f = 0.3;
    return 15.0 * f;
}

double nav_sentence_seconds(const Document *d, int s, double cps)
{
    if (s < 0 || s >= d->nsent || cps <= 0) return 0;
    return (double)d->sents[s].len / cps + 0.25;
}

double nav_seconds_before(const Document *d, int sent, double cps)
{
    if (d->nsent == 0 || cps <= 0) return 0;
    if (sent > d->nsent) sent = d->nsent;
    if (sent <= 0) return 0;
    /* characters before the sentence: its start offset minus whitespace is close enough and O(1) */
    double chars = (double)d->sents[sent < d->nsent ? sent : d->nsent - 1].start;
    if (sent >= d->nsent) chars = (double)(d->sents[d->nsent - 1].start + d->sents[d->nsent - 1].len);
    return chars / cps + 0.25 * sent;
}

double nav_total_seconds(const Document *d, double cps) { return nav_seconds_before(d, d->nsent, cps); }

/* binary search: last sentence whose start time <= t */
static int sentence_at_time(const Document *d, double t, double cps)
{
    int lo = 0, hi = d->nsent - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (nav_seconds_before(d, mid, cps) <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int nav_skip_seconds(const Document *d, int from, double seconds, double cps)
{
    if (d->nsent == 0) return -1;
    if (from < 0) from = 0;
    if (from >= d->nsent) from = d->nsent - 1;
    double t = nav_seconds_before(d, from, cps) + seconds;
    if (t <= 0) return 0;
    int s = sentence_at_time(d, t, cps);
    if (seconds > 0 && s <= from && from + 1 < d->nsent) s = from + 1;      /* always make progress */
    if (seconds < 0 && s >= from && from > 0) s = from - 1;
    return s;
}

int nav_at_fraction(const Document *d, double frac, double cps)
{
    if (d->nsent == 0) return -1;
    if (frac <= 0) return 0;
    if (frac >= 1) return d->nsent - 1;
    return sentence_at_time(d, nav_total_seconds(d, cps) * frac, cps);
}

int goto_parse(const wchar_t *t, GotoSpec *o)
{
    memset(o, 0, sizeof *o);
    while (*t == L' ') t++;
    if (!*t) return 0;
    const wchar_t *p = t;
    int sign = 0;
    if (*p == L'+') { sign = 1; p++; } else if (*p == L'-') { sign = -1; p++; }
    wchar_t *end;
    if (!sign && iswalpha(*p)) {
        wchar_t c = (wchar_t)towlower(*p);
        int kind = c == L'c' ? GT_CHAPTER : c == L'p' ? GT_PARAGRAPH : c == L's' ? GT_SENTENCE : c == L'm' ? GT_MARK : 0;
        if (!kind) return 0;
        p++;
        while (*p == L' ') p++;
        double v = wcstod(p, &end);
        if (end == p || v < 1) return 0;
        while (*end == L' ') end++;
        if (*end) return 0;
        o->kind = kind; o->value = v;
        return 1;
    }
    double v = wcstod(p, &end);
    if (end == p || v < 0) return 0;
    while (*end == L' ') end++;
    if (*end == L'%') { if (sign || v > 100) return 0; end++; o->kind = GT_PERCENT; o->value = v; }
    else if (*end == L's' || *end == L'm' || *end == L'h') {
        double mul = *end == L's' ? 1 : *end == L'm' ? 60 : 3600;
        end++;
        if (!sign) return 0;                                  /* plain "30s" would be ambiguous with a sentence number */
        o->kind = GT_SKIP; o->value = sign * v * mul;
    } else return 0;
    while (*end == L' ') end++;
    return *end == 0;
}

int caption_chunks(const wchar_t *t, size_t n, size_t maxc, size_t *starts, size_t *lens, int cap)
{
    int k = 0;
    size_t i = 0;
    if (maxc < 8) maxc = 8;
    while (i < n && k < cap) {
        while (i < n && t[i] <= L' ') i++;
        if (i >= n) break;
        size_t end = i + maxc;
        if (end >= n) end = n;
        else {
            size_t b = end;
            while (b > i && t[b] > L' ') b--;                 /* back to the last space within the limit */
            if (b > i) end = b;
            else { while (end < n && t[end] > L' ') end++; }   /* one very long word */
        }
        size_t e = end;
        while (e > i && t[e - 1] <= L' ') e--;
        starts[k] = i; lens[k] = e - i; k++;
        i = end;
    }
    return k;
}

size_t text_resume_offset(const wchar_t *t, size_t n, double frac)
{
    if (n == 0 || frac <= 0.04) return 0;
    if (frac >= 0.96) return n;
    size_t p = (size_t)(frac * (double)n);
    p = p > 4 ? p - 4 : 0;
    while (p > 0 && t[p - 1] > L' ') p--;          /* back to the start of the word */
    return p;
}
