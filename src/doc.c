#include "doc.h"
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

/* ---- abbreviation tables (lower-case ASCII) ---------------------------- */
/* Never end a sentence. */
static const char *const ABBR_NEVER[] = {
    "mr", "mrs", "ms", "mx", "dr", "prof", "sr", "jr", "st", "mt", "rev", "gen", "col",
    "lt", "capt", "sgt", "cpl", "hon", "messrs", "mme", "mlle", "rep", "sen", "gov", "cmdr",
    "vs", "cf", "viz", "approx", "dept", "univ", "inc", "ltd", "co", "corp", "bros",
    "e.g", "i.e", NULL
};
/* Only abbreviations when a number follows ("Fig. 4", "pp. 12"). */
static const char *const ABBR_BEFORE_NUMBER[] = {
    "fig", "figs", "no", "nos", "vol", "vols", "pp", "p", "ch", "sec", "eq", "art", "ed", "eds",
    "ca", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec", NULL
};
/* After a dotted initialism ("U.S.") a capitalised word only starts a new
 * sentence if it is a very common sentence opener. */
static const wchar_t *const STARTERS[] = {
    L"The", L"A", L"An", L"He", L"She", L"It", L"They", L"We", L"I", L"You", L"In", L"On",
    L"At", L"As", L"But", L"And", L"If", L"When", L"This", L"That", L"There", L"His", L"Her",
    L"Our", L"Their", L"After", L"Before", L"However", L"For", L"Some", L"Many", L"Most", NULL
};

static int in_list(const char *const *list, const wchar_t *tok, size_t n)
{
    char buf[16];
    if (n == 0 || n >= sizeof buf) return 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = tok[i];
        if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
        if (c >= 0x80) return 0;
        buf[i] = (char)c;
    }
    buf[n] = 0;
    for (; *list; list++) if (strcmp(*list, buf) == 0) return 1;
    return 0;
}

static int next_word_is_starter(const wchar_t *t, size_t m, size_t pe)
{
    size_t e = m;
    while (e < pe && is_alpha(t[e])) e++;
    size_t n = e - m;
    for (const wchar_t *const *s = STARTERS; *s; s++)
        if (wcslen(*s) == n && wmemcmp(*s, t + m, n) == 0) return 1;
    return 0;
}

/* "U.S", "U.S.A", "e.g": single letters separated by dots. */
static int is_dotted_initialism(const wchar_t *tok, size_t n)
{
    if (n < 3 || (n & 1) == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if ((i & 1) ? tok[i] != '.' : !is_alpha(tok[i])) return 0;
    return 1;
}

/* Is the capitalised single letter at t[m] followed by '.' (i.e. another initial)? */
static int next_is_initial(const wchar_t *t, size_t m, size_t pe)
{
    return m + 1 < pe && is_upper(t[m]) && t[m + 1] == '.';
}

/*
 * Decide whether the terminator run t[ti..tj) followed by whitespace and then
 * the word starting at t[m] ends a sentence. `pstart` bounds the backwards
 * token scan. Deliberately simple, deterministic and O(token length).
 */
static int is_boundary(const wchar_t *t, size_t pstart, size_t pe, size_t ti, size_t tj, size_t m)
{
    wchar_t nx = t[m];
    if (is_lower(nx)) return 0;
    if (!(tj - ti == 1 && t[ti] == '.')) return 1;      /* ? ! ... always end (unless lower-case next) */

    size_t ws = ti;
    while (ws > pstart && !is_space(t[ws - 1])) ws--;
    while (ws < ti && is_opener(t[ws])) ws++;
    const wchar_t *tok = t + ws;
    size_t n = ti - ws;
    if (n == 0) return 1;

    if (in_list(ABBR_NEVER, tok, n)) return 0;
    if (is_digit(nx) && in_list(ABBR_BEFORE_NUMBER, tok, n)) return 0;
    if (n == 1 && is_upper(tok[0])) {
        if (tok[0] == 'A' || tok[0] == 'I') return !next_is_initial(t, m, pe);
        return 0;                                        /* J. S. Mill */
    }
    if (is_dotted_initialism(tok, n)) return next_word_is_starter(t, m, pe);
    return 1;
}

/* ---- builder ------------------------------------------------------------ */
static int add_sentence(Document *d, size_t start, size_t len, int para)
{
    if (d->nsent == d->sent_cap) {
        int nc = d->sent_cap ? d->sent_cap * 2 : 256;
        Sentence *p = (Sentence *)realloc(d->sents, (size_t)nc * sizeof *p);
        if (!p) return 0;
        d->sents = p; d->sent_cap = nc;
    }
    d->sents[d->nsent].start = start;
    d->sents[d->nsent].len = len;
    d->sents[d->nsent].para = para;
    d->nsent++;
    return 1;
}

/* Emit [s,e) (already trimmed), splitting absurdly long ones at a clause
 * break (, ; :) or, failing that, at whitespace, so cache units stay small. */
static int emit_sentence(Document *d, size_t s, size_t e, int para)
{
    const wchar_t *t = d->text;
    while (e - s > DOC_MAX_SENTENCE) {
        size_t lo = s + DOC_MAX_SENTENCE / 2, hi = s + DOC_MAX_SENTENCE, cut = 0;
        for (size_t i = hi; i > lo; i--)
            if ((t[i - 1] == ',' || t[i - 1] == ';' || t[i - 1] == ':') && is_space(t[i])) { cut = i; break; }
        if (!cut)
            for (size_t i = hi; i > lo; i--)
                if (is_space(t[i])) { cut = i; break; }
        if (!cut) {                                       /* no whitespace at all: hard cut */
            cut = hi;
            if (cut > s && t[cut - 1] >= 0xD800 && t[cut - 1] <= 0xDBFF) cut--;   /* keep surrogate pairs whole */
            if (!add_sentence(d, s, cut - s, para)) return 0;
            s = cut;
            continue;
        }
        size_t ce = cut;
        while (ce > s && is_space(t[ce - 1])) ce--;
        if (!add_sentence(d, s, ce - s, para)) return 0;
        s = cut;
        while (s < e && is_space(t[s])) s++;
    }
    return e > s ? add_sentence(d, s, e - s, para) : 1;
}

static int split_paragraph(Document *d, size_t ps, size_t pe, int para)
{
    const wchar_t *t = d->text;
    size_t ss = ps, i = ps;
    while (i < pe) {
        if (!is_term(t[i])) { i++; continue; }
        size_t j = i;
        while (j < pe && is_term(t[j])) j++;
        size_t k = j;
        while (k < pe && is_closer(t[k])) k++;
        if (k >= pe) break;                               /* terminator ends the paragraph */
        if (!is_space(t[k])) { i = k; continue; }         /* 3.14  example.com  e.g.x */
        size_t m = k;
        while (m < pe && is_space(t[m])) m++;
        if (is_boundary(t, ps, pe, i, j, m)) {
            if (!emit_sentence(d, ss, k, para)) return 0;
            ss = m;
        }
        i = m;
    }
    return emit_sentence(d, ss, pe, para);
}

static int add_paragraph(Document *d, size_t ps, size_t pe)
{
    if (d->npara == d->para_cap) {
        int nc = d->para_cap ? d->para_cap * 2 : 64;
        Paragraph *p = (Paragraph *)realloc(d->paras, (size_t)nc * sizeof *p);
        if (!p) return 0;
        d->paras = p; d->para_cap = nc;
    }
    Paragraph *pg = &d->paras[d->npara];
    pg->start = ps; pg->len = pe - ps;
    pg->first_sentence = d->nsent;
    int idx = d->npara++;
    if (!split_paragraph(d, ps, pe, idx)) return 0;
    pg->sentence_count = d->nsent - pg->first_sentence;
    return 1;
}

void doc_init(Document *d) { memset(d, 0, sizeof *d); }

void doc_free(Document *d)
{
    free(d->text); free(d->paras); free(d->sents);
    doc_init(d);
}

int doc_build(Document *d, const wchar_t *text, size_t len)
{
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) { doc_free(d); return 0; }
    if (len) memcpy(copy, text, len * sizeof(wchar_t));
    copy[len] = 0;
    return doc_build_take(d, copy, len);
}

int doc_build_take(Document *d, wchar_t *text, size_t len)
{
    doc_free(d);
    d->text = text;                    /* text[len] must be NUL */
    d->len = len;

    /* Paragraphs = runs of non-blank lines separated by blank lines. */
    const wchar_t *t = d->text;
    size_t i = 0, ps = 0, pe = 0;
    int in_para = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && t[i] != '\n') i++;
        size_t a = ls, b = i;
        if (i < len) i++;
        while (a < b && is_space(t[a])) a++;
        while (b > a && is_space(t[b - 1])) b--;
        if (a == b) {
            if (in_para) { if (!add_paragraph(d, ps, pe)) goto oom; in_para = 0; }
        } else {
            if (!in_para) { ps = a; in_para = 1; }
            pe = b;
        }
    }
    if (in_para && !add_paragraph(d, ps, pe)) goto oom;
    return 1;
oom:
    doc_free(d);
    return 0;
}

int doc_sentence_at(const Document *d, size_t offset)
{
    if (d->nsent == 0) return -1;
    int lo = 0, hi = d->nsent - 1, ans = d->nsent - 1;
    while (lo <= hi) {                                    /* first sentence whose end >= offset */
        int mid = lo + (hi - lo) / 2;
        if (d->sents[mid].start + d->sents[mid].len >= offset) { ans = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    return ans;
}

wchar_t *doc_speech_text(const Document *d, int sentence)
{
    if (sentence < 0 || sentence >= d->nsent) return NULL;
    const Sentence *s = &d->sents[sentence];
    wchar_t *out = (wchar_t *)malloc((s->len + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    size_t j = 0;
    int pending_space = 0;
    for (size_t i = 0; i < s->len; i++) {
        wchar_t c = d->text[s->start + i];
        if (c == 0x200B || c == 0xFEFF || c == 0xAD) continue;
        if (is_space(c)) { pending_space = 1; continue; }
        if (pending_space && j) out[j++] = ' ';
        pending_space = 0;
        out[j++] = c;
    }
    out[j] = 0;
    return out;
}
