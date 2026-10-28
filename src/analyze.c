#include "analyze.h"
#include "textutil.h"
#include <stdlib.h>
#include <string.h>

typedef struct Hit { int kind; wchar_t token[48]; int sent; } Hit;

const wchar_t *analysis_kind_name(int k)
{
    switch (k) {
    case AK_URL: return L"URL";
    case AK_EMAIL: return L"E-mail";
    case AK_TECH: return L"Technical";
    case AK_INITIALS: return L"Initials";
    case AK_NUMBER: return L"Number";
    case AK_ACRONYM: return L"Acronym";
    case AK_OCR: return L"Possible OCR error";
    case AK_NAME: return L"Name";
    case AK_LONG: return L"Very long sentence";
    }
    return L"?";
}

const wchar_t *analysis_kind_hint(int k)
{
    switch (k) {
    case AK_URL: return L"Addresses are read out character by character unless the URL rule is on, or removed in Cleanup.";
    case AK_EMAIL: return L"Read with the URL rule; add a pronunciation if it still sounds wrong.";
    case AK_TECH: return L"Identifiers and file names: the Technical rule spells them out; a pronunciation entry can override it.";
    case AK_INITIALS: return L"Check how the voice reads it; add a pronunciation if it says it wrongly.";
    case AK_NUMBER: return L"Unusual number format (decimal, range, time, fraction): the Numbers rule handles common ones; check the sound.";
    case AK_ACRONYM: return L"Voices either say it as a word or spell it. Add a pronunciation (\"N A S A\" or \"nassa\") to choose.";
    case AK_OCR: return L"A digit inside a word usually means a scanning error. Fix it in the text (Find and replace).";
    case AK_NAME: return L"A name that appears often: listen to it once, and add a pronunciation if the voice gets it wrong.";
    case AK_LONG: return L"Very long sentences are hard to follow and get split for speech; consider adding punctuation.";
    }
    return L"";
}

static int is_letter(wchar_t c) { return is_alpha(c); }
static int is_dig(wchar_t c) { return c >= '0' && c <= '9'; }
static int is_upper_c(wchar_t c) { return fold_char(c) != c; }

static int add_hit(Hit **h, size_t *n, size_t *cap, int kind, const wchar_t *tok, size_t len, int sent)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        Hit *p = (Hit *)realloc(*h, nc * sizeof **h);
        if (!p) return 0;
        *h = p; *cap = nc;
    }
    Hit *x = &(*h)[(*n)++];
    x->kind = kind;
    if (len > 47) len = 47;
    memcpy(x->token, tok, len * sizeof(wchar_t));
    x->token[len] = 0;
    x->sent = sent;
    return 1;
}

static int cmp_hit(const void *a, const void *b)
{
    const Hit *x = (const Hit *)a, *y = (const Hit *)b;
    if (x->kind != y->kind) return x->kind - y->kind;
    int c = wcscmp(x->token, y->token);
    return c ? c : x->sent - y->sent;
}

static int in_dict(const PronDict *d, const wchar_t *w)
{
    if (!d || !d->n) return 0;
    size_t n = wcslen(w), ml = 0;
    int e = pron_match(d, w, n, 0, &ml);
    return e >= 0 && ml == n;
}

/* classify one trimmed token; returns kind or 0 */
static int classify(const wchar_t *t, size_t n, int sentence_start)
{
    if (n == 0) return 0;
    int letters = 0, digits = 0, upper = 0, other = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_letter(t[i])) { letters++; if (is_upper_c(t[i])) upper++; }
        else if (is_dig(t[i])) digits++;
        else other++;
    }
    for (size_t i = 0; i + 2 < n; i++) if (t[i] == ':' && t[i + 1] == '/' && t[i + 2] == '/') return AK_URL;
    if (n > 4 && (t[0] == 'w' || t[0] == 'W') && (t[1] == 'w' || t[1] == 'W') && (t[2] == 'w' || t[2] == 'W') && t[3] == '.') return AK_URL;
    const wchar_t *at = wmemchr(t, L'@', n);
    if (at && at > t && at + 2 < t + n && wmemchr(at, L'.', (size_t)(t + n - at))) return AK_EMAIL;
    int dcolon = 0;
    for (size_t i = 0; i + 1 < n; i++) if (t[i] == ':' && t[i + 1] == ':') dcolon = 1;
    if (wmemchr(t, L'_', n) || dcolon || wmemchr(t, L'\\', n)) return AK_TECH;
    if (digits == 0) {
        /* initials: J.R.R.  U.S.A. */
        if (n >= 4 && letters >= 2) {
            int ok = 1, dots = 0;
            for (size_t i = 0; i < n; i++) {
                if (i % 2 == 0) { if (!is_letter(t[i])) { ok = 0; break; } }
                else if (t[i] == '.') dots++;
                else { ok = 0; break; }
            }
            if (ok && dots >= 2) return AK_INITIALS;
        }
        if (letters == (int)n && n >= 2 && n <= 8 && upper == (int)n) return AK_ACRONYM;
        if (letters == (int)n && n >= 3 && upper >= 2 && is_upper_c(t[n - 1]) == 0 && upper == (int)n - 1 && t[n - 1] == 's') return AK_ACRONYM;   /* NASAs */
        /* internal capital after a lowercase start: camelCase identifier */
        if (letters == (int)n && n >= 4 && !is_upper_c(t[0])) {
            for (size_t i = 1; i < n; i++) if (is_upper_c(t[i])) return (i >= 2 ? AK_TECH : AK_OCR);
        }
        (void)sentence_start;
        return 0;
    }
    /* has digits */
    if (letters > 0) {
        /* digit surrounded by letters: scanning error; otherwise identifier (mp3, x86, A4, 3D) */
        for (size_t i = 1; i + 1 < n; i++)
            if (is_dig(t[i]) && is_letter(t[i - 1]) && is_letter(t[i + 1]) && letters >= 3 && digits == 1 && upper <= 1) return AK_OCR;
        /* ordinals 1st 2nd 3rd 4th, 10th: fine */
        size_t k = 0;
        while (k < n && is_dig(t[k])) k++;
        if (k > 0 && n - k == 2 && ((t[k] == 's' && t[k + 1] == 't') || (t[k] == 'n' && t[k + 1] == 'd') ||
                                   (t[k] == 'r' && t[k + 1] == 'd') || (t[k] == 't' && t[k + 1] == 'h'))) return 0;
        if (k > 0 && n - k == 1 && (t[k] == 's' || t[k] == 'x' || t[k] == 'm' || t[k] == 'k')) return 0;          /* 1990s 2x 5m 10k */
        return AK_TECH;
    }
    /* digits and punctuation only */
    if (other == 0) return 0;                                    /* plain integer */
    int inner = 0;
    for (size_t i = 1; i + 1 < n; i++) if (!is_dig(t[i]) && !is_letter(t[i]) && (t[i] == '.' || t[i] == ',' || t[i] == ':' || t[i] == '/' || t[i] == '-' || t[i] == 0x2013)) inner = 1;
    if (inner) {
        return AK_NUMBER;
    }
    if (t[0] == '$' || t[n - 1] == '%' || t[0] == 0xA3 || t[0] == 0x20AC) return AK_NUMBER;
    return 0;
}

int analyze_document(Analysis *a, const Document *d, const ChapterList *chs, const PronDict *dict, const SpeechOptions *opts)
{
    memset(a, 0, sizeof *a);
    a->paragraphs = d->npara;
    a->sentences = d->nsent;
    a->chapters = chs ? chs->n : 0;
    Hit *hits = NULL;
    size_t nh = 0, caph = 0;
    /* name candidates need a second pass: collect capitalised mid-sentence words */
    for (int s = 0; s < d->nsent; s++) {
        const Sentence *sn = &d->sents[s];
        const wchar_t *t = d->text + sn->start;
        size_t n = sn->len, i = 0;
        int first = 1;
        if (n > 500) { if (!add_hit(&hits, &nh, &caph, AK_LONG, t, 40, s)) goto oom; }
        while (i < n) {
            while (i < n && (t[i] <= ' ' || is_zero_width(t[i]))) i++;
            size_t st = i;
            while (i < n && t[i] > ' ') i++;
            if (i == st) break;
            a->words++;
            size_t b = i;
            /* strip surrounding punctuation; a trailing '.' survives only for initials (U.S.A.) */
            while (st < b && !is_alnum_c(t[st]) && t[st] != '$' && t[st] != 0xA3 && t[st] != 0x20AC) st++;
            while (b > st && !is_alnum_c(t[b - 1]) && t[b - 1] != '%' && t[b - 1] != '.') b--;
            if (b > st && t[b - 1] == '.' && !(b - st >= 4 && is_letter(t[b - 2]) && t[b - 3] == '.')) b--;
            while (b > st && !is_alnum_c(t[b - 1]) && t[b - 1] != '%' && t[b - 1] != '.') b--;
            size_t len = b > st ? b - st : 0;
            int kind = classify(t + st, len, first);
            if (kind && len <= 47) { if (!add_hit(&hits, &nh, &caph, kind, t + st, len, s)) goto oom; }
            else if (!kind && !first && len >= 4 && is_upper_c(t[st])) {
                int all_alpha = 1;
                for (size_t k = st; k < st + len; k++) if (!is_letter(t[k])) { all_alpha = 0; break; }
                int rest_lower = 1;
                for (size_t k = st + 1; k < st + len; k++) if (is_upper_c(t[k])) { rest_lower = 0; break; }
                if (all_alpha && rest_lower && len <= 47) { if (!add_hit(&hits, &nh, &caph, AK_NAME, t + st, len, s)) goto oom; }
            }
            first = 0;
        }
    }
    if (nh) qsort(hits, nh, sizeof *hits, cmp_hit);
    size_t i = 0;
    int cap = 0;
    while (i < nh) {
        size_t j = i;
        while (j < nh && hits[j].kind == hits[i].kind && wcscmp(hits[j].token, hits[i].token) == 0) j++;
        int count = (int)(j - i);
        int keep = 1;
        if (hits[i].kind == AK_NAME && count < 3) keep = 0;            /* a name seen once or twice is not worth a line */
        if (keep) {
            if (a->n == cap) {
                cap = cap ? cap * 2 : 64;
                Issue *p = (Issue *)realloc(a->issues, (size_t)cap * sizeof *p);
                if (!p) goto oom;
                a->issues = p;
            }
            Issue *is = &a->issues[a->n++];
            is->kind = hits[i].kind;
            wcsncpy(is->token, hits[i].token, 47);
            is->token[47] = 0;
            is->count = count;
            is->first_sent = hits[i].sent;
            int h = 0;
            if (in_dict(dict, is->token)) h = 1;
            else if (opts) {
                if ((is->kind == AK_URL || is->kind == AK_EMAIL) && opts->urls) h = 1;
                else if (is->kind == AK_TECH && opts->technical) h = 1;
                else if (is->kind == AK_NUMBER && opts->numbers) h = 1;
            }
            is->handled = h;
        }
        i = j;
    }
    free(hits);
    return 1;
oom:
    free(hits);
    analysis_free(a);
    return 0;
}

void analysis_free(Analysis *a) { free(a->issues); memset(a, 0, sizeof *a); }
