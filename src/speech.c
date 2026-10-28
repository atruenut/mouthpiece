#include "speech.h"
#include "textutil.h"
#include <stdlib.h>
#include <string.h>

typedef struct Out {
    SpeechText *t;
    int want_spans, ok;
} Out;

void speech_default_options(SpeechOptions *o) { o->abbreviations = o->numbers = o->urls = o->technical = 1; }

void speech_text_free(SpeechText *t)
{
    free(t->text); free(t->spans);
    memset(t, 0, sizeof *t);
}

static int ensure(Out *o, size_t extra)
{
    SpeechText *t = o->t;
    if (t->len + extra + 1 <= t->cap) return 1;
    size_t nc = t->cap ? t->cap * 2 : 256;
    while (nc < t->len + extra + 1) nc *= 2;
    wchar_t *p = (wchar_t *)realloc(t->text, nc * sizeof(wchar_t));
    if (!p) { o->ok = 0; return 0; }
    t->text = p; t->cap = nc;
    return 1;
}
static void put(Out *o, const wchar_t *s, size_t n)
{
    if (!o->ok || !ensure(o, n)) return;
    memcpy(o->t->text + o->t->len, s, n * sizeof(wchar_t));
    o->t->len += n;
}
static void putc_(Out *o, wchar_t c) { put(o, &c, 1); }
static void puts_(Out *o, const wchar_t *s) { put(o, s, wcslen(s)); }

static void span(Out *o, int kind, size_t in_start, size_t in_len, size_t out_start)
{
    SpeechText *t = o->t;
    if (!o->want_spans || !o->ok) return;
    if (t->nspans == t->span_cap) {
        int nc = t->span_cap ? t->span_cap * 2 : 16;
        SpeechSpan *p = (SpeechSpan *)realloc(t->spans, (size_t)nc * sizeof *p);
        if (!p) { o->ok = 0; return; }
        t->spans = p; t->span_cap = nc;
    }
    SpeechSpan *s = &t->spans[t->nspans++];
    s->kind = kind; s->in_start = in_start; s->in_len = in_len; s->out_start = out_start; s->out_len = t->len - out_start;
}

/* ------------------------------------------------------------ number words */

static const wchar_t *const ONES[] = { L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven",
    L"eight", L"nine", L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
    L"seventeen", L"eighteen", L"nineteen" };
static const wchar_t *const TENS[] = { L"", L"", L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety" };

static void below_100(Out *o, int n)
{
    if (n < 20) { puts_(o, ONES[n]); return; }
    puts_(o, TENS[n / 10]);
    if (n % 10) { putc_(o, '-'); puts_(o, ONES[n % 10]); }
}

static void below_1000(Out *o, int n)
{
    if (n >= 100) { puts_(o, ONES[n / 100]); puts_(o, L" hundred"); n %= 100; if (n) putc_(o, ' '); }
    if (n) below_100(o, n);
}

static void cardinal(Out *o, unsigned long long n)
{
    if (n == 0) { puts_(o, L"zero"); return; }
    static const struct { unsigned long long v; const wchar_t *name; } SC[] = {
        { 1000000000ULL, L"billion" }, { 1000000ULL, L"million" }, { 1000ULL, L"thousand" } };
    int first = 1;
    for (int i = 0; i < 3; i++) {
        unsigned long long g = n / SC[i].v;
        if (g) {
            if (!first) putc_(o, ' ');
            if (g >= 1000) { cardinal(o, g); } else below_1000(o, (int)g);
            putc_(o, ' '); puts_(o, SC[i].name);
            n %= SC[i].v; first = 0;
        }
    }
    if (n) { if (!first) putc_(o, ' '); below_1000(o, (int)n); }
}

wchar_t *speech_number_words(unsigned long long n)
{
    SpeechText t; memset(&t, 0, sizeof t);
    Out o = { &t, 0, 1 };
    cardinal(&o, n);
    if (!o.ok || !ensure(&o, 0)) { speech_text_free(&t); return NULL; }
    t.text[t.len] = 0;
    return t.text;
}

static void digits_words(Out *o, const wchar_t *d, size_t n)       /* "007" -> "zero zero seven" */
{
    for (size_t i = 0; i < n; i++) { if (i) putc_(o, ' '); puts_(o, ONES[d[i] - '0']); }
}

/* Turn the last word of out[from..] into its ordinal form ("twenty-one" -> "twenty-first"). */
static void ordinalize(Out *o, size_t from)
{
    SpeechText *t = o->t;
    size_t e = t->len, s = e;
    while (s > from && t->text[s - 1] != ' ' && t->text[s - 1] != '-') s--;
    size_t n = e - s;
    static const struct { const wchar_t *a, *b; } IRR[] = { { L"one", L"first" }, { L"two", L"second" },
        { L"three", L"third" }, { L"five", L"fifth" }, { L"eight", L"eighth" }, { L"nine", L"ninth" }, { L"twelve", L"twelfth" } };
    for (size_t i = 0; i < sizeof IRR / sizeof IRR[0]; i++)
        if (wcslen(IRR[i].a) == n && wcsncmp(t->text + s, IRR[i].a, n) == 0) {
            t->len = s; puts_(o, IRR[i].b); return;
        }
    if (n && t->text[e - 1] == 'y') { t->len = e - 1; puts_(o, L"ieth"); }
    else puts_(o, L"th");
}

static void pluralize(Out *o)
{
    SpeechText *t = o->t;
    if (t->len && t->text[t->len - 1] == 'y') { t->len--; puts_(o, L"ies"); } else puts_(o, L"s");
}

static void year_words(Out *o, int y)
{
    int hi = y / 100, lo = y % 100;
    if (y >= 2000 && y <= 2009) {
        puts_(o, L"two thousand");
        if (lo) { putc_(o, ' '); below_100(o, lo); }
        return;
    }
    below_100(o, hi);
    if (lo == 0) puts_(o, L" hundred");
    else if (lo < 10) { puts_(o, L" oh "); puts_(o, ONES[lo]); }
    else { putc_(o, ' '); below_100(o, lo); }
}

/* ------------------------------------------------------------ helpers */

static int word_start(const wchar_t *in, size_t i) { return i == 0 || !is_alnum_c(in[i - 1]); }

static int ieq(const wchar_t *in, size_t i, size_t n, const char *p)
{
    for (; *p; p++, i++) {
        if (i >= n) return 0;
        wchar_t c = in[i];
        if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
        if (c != (wchar_t)*p) return 0;
    }
    return 1;
}

/* only closers/spaces remain from position j to the end? */
static int at_end(const wchar_t *in, size_t n, size_t j)
{
    for (; j < n; j++)
        if (!(is_space(in[j]) || in[j] == '"' || in[j] == '\'' || in[j] == ')' || in[j] == ']' || in[j] == 0x201D || in[j] == 0x2019)) return 0;
    return 1;
}

/* ------------------------------------------------------------ abbreviations */

static const struct { const char *abbr; const wchar_t *full; int needs_number, may_end; } ABBR[] = {
    /* titles never end a sentence */
    { "Mr", L"Mister", 0, 0 }, { "Mrs", L"Missus", 0, 0 }, { "Ms", L"Miz", 0, 0 }, { "Dr", L"Doctor", 0, 0 },
    { "Prof", L"Professor", 0, 0 }, { "Mt", L"Mount", 0, 0 }, { "Rev", L"Reverend", 0, 0 }, { "Gen", L"General", 0, 0 },
    { "Col", L"Colonel", 0, 0 }, { "Lt", L"Lieutenant", 0, 0 }, { "Capt", L"Captain", 0, 0 }, { "Sgt", L"Sergeant", 0, 0 },
    { "Sen", L"Senator", 0, 0 }, { "Rep", L"Representative", 0, 0 }, { "Gov", L"Governor", 0, 0 },
    { "Fig", L"Figure", 0, 0 }, { "Figs", L"Figures", 0, 0 }, { "Eq", L"Equation", 0, 0 },
    { "Vol", L"Volume", 0, 0 }, { "Vols", L"Volumes", 0, 0 },
    /* these can finish a sentence: keep the full stop then */
    { "Jr", L"Junior", 0, 1 }, { "Sr", L"Senior", 0, 1 }, { "Inc", L"Incorporated", 0, 1 }, { "Ltd", L"Limited", 0, 1 },
    { "Corp", L"Corporation", 0, 1 }, { "Co", L"Company", 0, 1 },
    { "vs", L"versus", 0, 1 }, { "cf", L"compare", 0, 0 }, { "approx", L"approximately", 0, 1 }, { "etc", L"et cetera", 0, 1 },
    { "pp", L"pages", 0, 0 }, { "No", L"Number", 1, 0 }, { "p", L"page", 1, 0 },
};

static size_t try_abbrev(Out *o, const wchar_t *in, size_t n, size_t i)
{
    if (!word_start(in, i)) return 0;
    if (i + 3 < n && in[i + 1] == '.' && in[i + 3] == '.') {            /* e.g. / i.e. */
        if (in[i] == 'e' && in[i + 2] == 'g') { puts_(o, L"for example"); return 4; }
        if (in[i] == 'i' && in[i + 2] == 'e') { puts_(o, L"that is"); return 4; }
    }
    size_t j = i;
    while (j < n && ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z'))) j++;
    if (j >= n || in[j] != '.' || j == i) return 0;
    size_t len = j - i;
    for (size_t k = 0; k < sizeof ABBR / sizeof ABBR[0]; k++) {
        if (strlen(ABBR[k].abbr) != len) continue;
        int same = 1;
        for (size_t c = 0; c < len; c++) if (in[i + c] != (wchar_t)ABBR[k].abbr[c]) { same = 0; break; }
        if (!same) continue;
        if (ABBR[k].needs_number) {
            size_t m = j + 1;
            while (m < n && in[m] == ' ') m++;
            if (m >= n || !is_digit(in[m])) return 0;
        }
        puts_(o, ABBR[k].full);
        return (ABBR[k].may_end && at_end(in, n, j + 1)) ? len : len + 1;      /* keep '.' when it ends the sentence */
    }
    return 0;
}

/* ------------------------------------------------------------ urls / e-mail / tech */

static int is_mail_char(wchar_t c) { return (c < 128 && (is_alnum_c(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-')); }

static size_t try_url_email(Out *o, const wchar_t *in, size_t n, size_t i)
{
    if (!word_start(in, i)) return 0;
    if (ieq(in, i, n, "http://") || ieq(in, i, n, "https://") || ieq(in, i, n, "ftp://") || ieq(in, i, n, "www.")) {
        size_t j = i;
        while (j < n && !is_space(in[j])) j++;
        while (j > i + 4 && (in[j - 1] == '.' || in[j - 1] == ',' || in[j - 1] == ';' || in[j - 1] == ':' || in[j - 1] == '!' ||
                             in[j - 1] == '?' || in[j - 1] == ')' || in[j - 1] == ']' || in[j - 1] == '"' || in[j - 1] == '\'')) j--;
        if (j <= i + 4) return 0;
        puts_(o, L"web address");
        return j - i;
    }
    size_t j = i;
    while (j < n && is_mail_char(in[j])) j++;
    if (j > i && j < n && in[j] == '@') {
        size_t d = j + 1, k = d;
        while (k < n && is_mail_char(in[k])) k++;
        while (k > d && in[k - 1] == '.') k--;
        size_t dot = k;
        while (dot > d && in[dot - 1] != '.') dot--;
        if (dot > d && k - dot >= 2) { puts_(o, L"email address"); return k - i; }
    }
    return 0;
}

static size_t try_tech(Out *o, const wchar_t *in, size_t n, size_t i)
{
    if (in[i] == '.' && ieq(in, i + 1, n, "net") && (i == 0 || !is_alnum_c(in[i - 1])) &&
        (i + 4 >= n || !is_alnum_c(in[i + 4])) && in[i + 1] == 'N') { puts_(o, L"dot net"); return 4; }
    if (!word_start(in, i)) return 0;
    if (in[i] == 'C' && i + 2 < n && in[i + 1] == '+' && in[i + 2] == '+' && (i + 3 >= n || !is_alnum_c(in[i + 3]))) { puts_(o, L"C plus plus"); return 3; }
    if ((in[i] == 'C' || in[i] == 'F') && i + 1 < n && in[i + 1] == '#' && (i + 2 >= n || !is_alnum_c(in[i + 2]))) {
        putc_(o, in[i]); puts_(o, L" sharp"); return 2;
    }
    return 0;
}

/* ------------------------------------------------------------ numbers */

static int in_list(const wchar_t *w, size_t n, const char *const *list)
{
    for (; *list; list++) {
        size_t l = strlen(*list);
        if (l != n) continue;
        size_t k = 0;
        for (; k < n; k++) {
            wchar_t c = w[k];
            if (c >= 'A' && c <= 'Z') c = (wchar_t)(c + 32);
            if (c != (wchar_t)(*list)[k]) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

static int year_context(const wchar_t *in, size_t n, size_t i, size_t end)
{
    static const char *const CTX[] = { "in", "since", "during", "by", "until", "till", "from", "of", "circa", "c", "ca",
        "year", "years", "early", "mid", "late", "before", "after", "between", "spring", "summer", "autumn", "fall",
        "winter", "january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
        "november", "december", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec", NULL };
    (void)n;
    size_t p = i;
    while (p > 0 && is_space(in[p - 1])) p--;
    if (p > 0 && in[p - 1] == '(') return 1;
    if (p > 0 && (in[p - 1] == '-' || in[p - 1] == 0x2013) && p >= 5 && is_digit(in[p - 2]) && is_digit(in[p - 5])) return 1;  /* 1990-2000 */
    size_t e = p;
    while (p > 0 && is_alpha(in[p - 1])) p--;
    if (e > p && in_list(in + p, e - p, CTX)) return 1;
    if (end + 4 < n && (in[end] == '-' || in[end] == 0x2013) && is_digit(in[end + 1]) && is_digit(in[end + 4])) return 1;
    return 0;
}

static void emit_number(Out *o, const wchar_t *d, size_t dn, int allow_digits_mode)
{
    if ((dn > 1 && d[0] == '0' && allow_digits_mode) || dn > 12) { digits_words(o, d, dn); return; }
    unsigned long long v = 0;
    for (size_t i = 0; i < dn; i++) v = v * 10 + (unsigned)(d[i] - '0');
    cardinal(o, v);
}

/* Collect digits of d[0..) with thousands commas removed into buf. */
static size_t strip_commas(const wchar_t *s, size_t n, wchar_t *buf)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (s[i] != ',') buf[k++] = s[i];
    return k;
}

/* Returns consumed length; *handled = 0 means "copy those chars unchanged". */
static size_t try_number(Out *o, const wchar_t *in, size_t n, size_t i, int *handled)
{
    *handled = 0;
    wchar_t cur = 0;
    int neg = 0;
    size_t j = i;
    if ((in[i] == '$' || in[i] == 0x20AC || in[i] == 0x00A3) && i + 1 < n && is_digit(in[i + 1])) { cur = in[i]; j = i + 1; }
    else if (in[i] == '-' && i + 1 < n && is_digit(in[i + 1]) && (i == 0 || is_space(in[i - 1]) || in[i - 1] == '(')) { neg = 1; j = i + 1; }
    else if (!is_digit(in[i])) return 0;
    size_t ds = j;
    if (!cur && !neg && i > 0 && (is_alpha(in[i - 1]) || in[i - 1] == '_')) {          /* x86, v2, mp3 */
        size_t k = i;
        while (k < n && is_digit(in[k])) k++;
        return k - i;
    }
    size_t k = ds;
    while (k < n && is_digit(in[k])) k++;
    size_t r0 = k - ds;
    if (r0 <= 3) {                                                  /* thousands separators */
        while (k + 3 < n && in[k] == ',' && is_digit(in[k + 1]) && is_digit(in[k + 2]) && is_digit(in[k + 3]) &&
               !(k + 4 < n && is_digit(in[k + 4]))) k += 4;
    }
    size_t int_end = k, dec_s = 0, end = k;
    int has_dec = 0;
    if (k + 1 < n && in[k] == '.' && is_digit(in[k + 1])) {
        dec_s = k + 1;
        size_t m = dec_s;
        while (m < n && is_digit(in[m])) m++;
        if (m + 1 < n && in[m] == '.' && is_digit(in[m + 1])) {      /* 3.2.1: leave alone */
            while (m < n && (is_digit(in[m]) || (in[m] == '.' && m + 1 < n && is_digit(in[m + 1])))) m++;
            return m - i;
        }
        has_dec = 1; end = m;
    }
    if (end + 1 < n && (in[end] == ':' || in[end] == '/') && is_digit(in[end + 1])) {    /* times, dates, fractions */
        size_t m = end;
        while (m + 1 < n && (in[m] == ':' || in[m] == '/' || in[m] == '.') && is_digit(in[m + 1])) { m++; while (m < n && is_digit(in[m])) m++; }
        return m - i;
    }
    wchar_t dig[64], dec[64];
    size_t dn = strip_commas(in + ds, int_end - ds, dig);
    if (dn >= 63) return end - i;
    size_t decn = has_dec ? end - dec_s : 0;
    if (decn >= 63) return end - i;
    for (size_t q = 0; q < decn; q++) dec[q] = in[dec_s + q];

    /* suffixes */
    if (!cur && !has_dec && end + 1 < n && is_alpha(in[end]) && is_alpha(in[end + 1]) && !(end + 2 < n && is_alnum_c(in[end + 2]))) {
        if (ieq(in, end, n, "st") || ieq(in, end, n, "nd") || ieq(in, end, n, "rd") || ieq(in, end, n, "th")) {
            if (neg) puts_(o, L"minus ");
            size_t from = o->t->len;
            emit_number(o, dig, dn, 1);
            ordinalize(o, from);
            *handled = 1;
            return end + 2 - i;
        }
    }
    int decade = 0;
    if (!cur && !has_dec && dn == 4 && end < n && in[end] == 's' && !(end + 1 < n && is_alnum_c(in[end + 1]))) decade = 1;
    if (end < n && is_alpha(in[end]) && !decade) return end - i;                        /* 5km, 3D: leave as written */
    if (cur) {                                                       /* currency */
        const wchar_t *major = cur == '$' ? L"dollars" : cur == 0x20AC ? L"euros" : L"pounds";
        const wchar_t *major1 = cur == '$' ? L"dollar" : cur == 0x20AC ? L"euro" : L"pound";
        const wchar_t *minor = cur == 0x00A3 ? L"pence" : L"cents";
        size_t m = end;
        while (m < n && in[m] == ' ') m++;
        size_t ws = m;
        while (m < n && is_alpha(in[m])) m++;
        static const char *const MAG[] = { "thousand", "million", "billion", "trillion", "hundred", NULL };
        int mag = (m > ws && ws > end && in_list(in + ws, m - ws, MAG));
        if (mag) {
            emit_number(o, dig, dn, 0);
            if (has_dec) { puts_(o, L" point "); digits_words(o, dec, decn); }
            putc_(o, ' '); put(o, in + ws, m - ws); putc_(o, ' '); puts_(o, major);
            *handled = 1;
            return m - i;
        }
        if (has_dec && decn == 2) {
            int cents = (dec[0] - '0') * 10 + (dec[1] - '0');
            int zero_major = (dn == 1 && dig[0] == '0');
            if (!zero_major) {
                emit_number(o, dig, dn, 0);
                putc_(o, ' ');
                puts_(o, (dn == 1 && dig[0] == '1') ? major1 : major);
                if (cents) puts_(o, L" and ");
            }
            if (cents || zero_major) { below_100(o, cents); putc_(o, ' '); puts_(o, cents == 1 && cur != 0x00A3 ? L"cent" : minor); }
        } else {
            emit_number(o, dig, dn, 0);
            if (has_dec) { puts_(o, L" point "); digits_words(o, dec, decn); }
            putc_(o, ' ');
            puts_(o, (!has_dec && dn == 1 && dig[0] == '1') ? major1 : major);
        }
        *handled = 1;
        return end - i;
    }
    if (neg) puts_(o, L"minus ");
    if (end < n && in[end] == '%') {
        emit_number(o, dig, dn, 1);
        if (has_dec) { puts_(o, L" point "); digits_words(o, dec, decn); }
        puts_(o, L" percent");
        *handled = 1;
        return end + 1 - i;
    }
    unsigned v = 0;
    int plain4 = (!has_dec && dn == 4 && int_end - ds == 4);
    if (plain4) v = (unsigned)((dig[0] - '0') * 1000 + (dig[1] - '0') * 100 + (dig[2] - '0') * 10 + (dig[3] - '0'));
    if (plain4 && !neg && v >= 1100 && v <= 2099 && (decade || year_context(in, n, i, end))) {
        year_words(o, (int)v);
        if (decade) { pluralize(o); *handled = 1; return end + 1 - i; }
        *handled = 1;
        /* 1990-2000 -> "nineteen ninety to two thousand" (only between two years) */
        if (end + 4 < n && (in[end] == '-' || in[end] == 0x2013) && is_digit(in[end + 1]) && is_digit(in[end + 2]) &&
            is_digit(in[end + 3]) && is_digit(in[end + 4]) && !(end + 5 < n && is_digit(in[end + 5]))) {
            unsigned v2 = (unsigned)((in[end + 1] - '0') * 1000 + (in[end + 2] - '0') * 100 + (in[end + 3] - '0') * 10 + (in[end + 4] - '0'));
            if (v2 >= 1100 && v2 <= 2099) { puts_(o, L" to "); return end + 1 - i; }
        }
        return end - i;
    }
    emit_number(o, dig, dn, 1);
    if (has_dec) { puts_(o, L" point "); digits_words(o, dec, decn); }
    *handled = 1;
    return end - i;
}

/* ------------------------------------------------------------ driver */

int speech_render(const Speech *sp, const wchar_t *in, size_t n, int want_spans, SpeechText *t)
{
    memset(t, 0, sizeof *t);
    Out o = { t, want_spans, 1 };
    if (!ensure(&o, n + 16)) return 0;
    const PronDict *d = sp ? sp->dict : NULL;
    SpeechOptions opt;
    if (sp) opt = sp->opts; else memset(&opt, 0, sizeof opt);

    size_t i = 0;
    while (i < n && o.ok) {
        size_t start_out = t->len, used = 0;
        if (d && d->n) {
            size_t ml = 0;
            int e = pron_match_ex(d, in, n, i, sp->sentence1 - 1, &ml);
            if (e >= 0) {
                size_t rl = pron_replacement(d, e, in, n, i, NULL);
                if (ensure(&o, rl)) { pron_replacement(d, e, in, n, i, t->text + t->len); t->len += rl; }
                span(&o, SPAN_DICT, i, ml, start_out);
                i += ml;
                continue;
            }
        }
        if (opt.urls && (used = try_url_email(&o, in, n, i)) > 0) { span(&o, SPAN_NORM, i, used, start_out); i += used; continue; }
        if (opt.technical && (used = try_tech(&o, in, n, i)) > 0) { span(&o, SPAN_NORM, i, used, start_out); i += used; continue; }
        if (opt.abbreviations && (used = try_abbrev(&o, in, n, i)) > 0) { span(&o, SPAN_NORM, i, used, start_out); i += used; continue; }
        if (opt.numbers) {
            int handled = 0;
            used = try_number(&o, in, n, i, &handled);
            if (used > 0) {
                if (handled) span(&o, SPAN_NORM, i, used, start_out);
                else put(&o, in + i, used);
                i += used;
                continue;
            }
        }
        putc_(&o, in[i]);
        i++;
    }
    if (!o.ok || !ensure(&o, 0)) { speech_text_free(t); return 0; }
    t->text[t->len] = 0;
    return 1;
}

wchar_t *speech_sentence_text(const Speech *sp, const Document *doc, int sent)
{
    wchar_t *plain = doc_speech_text(doc, sent);
    if (!plain) return NULL;
    SpeechText t;
    Speech here;
    if (sp) here = *sp; else memset(&here, 0, sizeof here);
    here.sentence1 = sent + 1;                                   /* lets scoped pronunciation rules apply */
    if (!speech_render(&here, plain, wcslen(plain), 0, &t)) { free(plain); return NULL; }
    free(plain);
    return t.text;                       /* caller frees; spans were not requested */
}
