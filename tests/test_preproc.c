/* Tests for preproc.c (cleanup + offset map) and its integration with doc.c. */
#include "../src/preproc.h"
#include "../src/doc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run(const wchar_t *src, const PreprocOptions *o, PreprocResult *r)
{
    CHECK(preproc_run(src, wcslen(src), o, 1, r), "preproc_run failed");
}
static PreprocOptions defaults(void) { PreprocOptions o; preproc_default_options(&o); return o; }

static void expect(const char *name, const wchar_t *src, const PreprocOptions *o, const wchar_t *want)
{
    PreprocResult r;
    run(src, o, &r);
    CHECK(r.text && wcscmp(r.text, want) == 0, "%s:\n  want \"%ls\"\n  got  \"%ls\"", name, want, r.text ? r.text : L"(null)");
    preproc_free(&r);
}

/* A hard-wrapped body: each paragraph is `n` lines of ~66 chars. */
static const wchar_t *LINES[] = {
    L"The economy of the early period was shaped by many forces",
    L"that interacted in ways the older histories rarely noted",
    L"and which only careful study of the records has revealed",
    L"to us in the years since the archives were first opened",
};
#define NLINES 4

static wchar_t *make_book(int pages, int lines_per_page, int with_header, int with_pagenum)
{
    size_t cap = (size_t)pages * lines_per_page * 80 + 4096;
    wchar_t *b = (wchar_t *)malloc(cap * sizeof(wchar_t));
    size_t n = 0;
    int li = 0;
    for (int p = 1; p <= pages; p++) {
        if (with_header) n += (size_t)swprintf(b + n, cap - n, L"THE ECONOMICS OF INDUSTRY\n");
        for (int k = 0; k < lines_per_page; k++) {
            n += (size_t)swprintf(b + n, cap - n, L"%ls\n", LINES[li++ % NLINES]);
        }
        if (with_pagenum) n += (size_t)swprintf(b + n, cap - n, L"%d\n", 36 + p);
    }
    b[n] = 0;
    return b;
}

static void test_hyphens(void)
{
    PreprocOptions o = defaults();
    expect("hyphen", L"The indus-\ntrial revolution began.", &o, L"The industrial revolution began.");
    expect("well-known midline", L"A well-known fact.", &o, L"A well-known fact.");
    expect("well- break keeps hyphen", L"It was a well-\nknown fact.", &o, L"It was a well-known fact.");
    expect("capital keeps hyphen", L"The Anglo-\nSaxon period.", &o, L"The Anglo-Saxon period.");
    expect("soft hyphen", L"The indus\u00ad\ntrial revolution.", &o, L"The industrial revolution.");
    expect("e- keeps", L"Send an e-\nmail now.", &o, L"Send an e-mail now.");
    /* header between the halves of a hyphenated word (page break) */
    PreprocOptions off = o; off.fix_hyphens = 0; off.join_lines = 0;
    expect("hyphen fix off", L"The indus-\ntrial revolution.", &off, L"The indus-\ntrial revolution.");
}

static void test_headers_and_pagenums(void)
{
    PreprocOptions o = defaults();
    wchar_t *book = make_book(12, 30, 1, 1);
    PreprocResult r;
    run(book, &o, &r);
    CHECK(r.n_headers == 12, "12 headers removed, got %zu", r.n_headers);
    CHECK(r.n_pagenums == 12, "12 page numbers removed, got %zu", r.n_pagenums);
    CHECK(wcsstr(r.text, L"ECONOMICS OF INDUSTRY") == NULL, "header text gone");
    CHECK(wcsstr(r.text, L"\n37\n") == NULL && wcsstr(r.text, L"\n40\n") == NULL, "numbers gone");
    CHECK(r.hard_wrapped, "book detected as hard-wrapped");
    CHECK(wcsstr(r.text, L"forces that interacted") != NULL, "lines joined");
    preproc_free(&r);

    PreprocOptions nh = o; nh.repeated_lines = 0;
    run(book, &nh, &r);
    CHECK(r.n_headers == 0 && wcsstr(r.text, L"ECONOMICS OF INDUSTRY") != NULL, "header kept when option off");
    preproc_free(&r);
    free(book);

    /* Headers with changing page numbers inside the line: "42 THE ECONOMIC REVIEW" */
    wchar_t *b2 = (wchar_t *)malloc(400000 * sizeof(wchar_t));
    size_t n = 0;
    for (int p = 1; p <= 10; p++) {
        n += (size_t)swprintf(b2 + n, 400000 - n, L"%d THE ECONOMIC REVIEW\n", 40 + p);
        for (int k = 0; k < 25; k++) n += (size_t)swprintf(b2 + n, 400000 - n, L"%ls\n", LINES[k % NLINES]);
    }
    run(b2, &o, &r);
    CHECK(r.n_headers == 10, "numbered running heads removed (got %zu)", r.n_headers);
    preproc_free(&r);
    free(b2);

    /* Not removed: a lone number in prose, a repeated line with irregular spacing, chapter headings, list numbers */
    expect("lone number kept", L"I counted\n42\nsheep in the field today.", &o, L"I counted\n42\nsheep in the field today.");
    expect("decorated page", L"end of page text here and it keeps going on and on\n- 12 -\nnext page text", &o,
           L"end of page text here and it keeps going on and on\nnext page text");
    expect("Page N of M", L"some text\nPage 3 of 90\nmore text", &o, L"some text\nmore text");

    wchar_t *b3 = (wchar_t *)malloc(400000 * sizeof(wchar_t));
    n = 0;
    for (int p = 1; p <= 8; p++) {
        n += (size_t)swprintf(b3 + n, 400000 - n, L"Chapter %d\n", p);
        for (int k = 0; k < 20; k++) n += (size_t)swprintf(b3 + n, 400000 - n, L"%ls\n", LINES[k % NLINES]);
    }
    run(b3, &o, &r);
    CHECK(r.n_headers == 0, "'Chapter N' headings are structure, not running heads (got %zu)", r.n_headers);
    CHECK(wcsstr(r.text, L"Chapter 5") != NULL, "chapter 5 heading present");
    preproc_free(&r);
    free(b3);

    n = 0;
    b3 = (wchar_t *)malloc(400000 * sizeof(wchar_t));
    const int gaps[] = { 3, 40, 2, 90, 5, 11 };
    for (int g = 0; g < 6; g++) {
        n += (size_t)swprintf(b3 + n, 400000 - n, L"Refrain of the song\n");
        for (int k = 0; k < gaps[g]; k++) n += (size_t)swprintf(b3 + n, 400000 - n, L"%ls\n", LINES[k % NLINES]);
    }
    run(b3, &o, &r);
    CHECK(r.n_headers == 0, "irregularly repeated line kept (got %zu)", r.n_headers);
    preproc_free(&r);
    free(b3);
}

static wchar_t *make_front(const wchar_t *const *marks, int n, int gap)
{
    size_t cap = 200000, k = 0;
    wchar_t *b = (wchar_t *)malloc(cap * sizeof(wchar_t));
    for (int p = 0; p < n; p++) {
        for (int i = 0; i < gap; i++) k += (size_t)swprintf(b + k, cap - k, L"%ls\n", LINES[i % NLINES]);
        k += (size_t)swprintf(b + k, cap - k, L"%ls\n", marks[p]);
    }
    for (int i = 0; i < gap; i++) k += (size_t)swprintf(b + k, cap - k, L"%ls\n", LINES[i % NLINES]);
    b[k] = 0;
    return b;
}

static void test_roman(void)
{
    PreprocOptions o = defaults();
    PreprocResult r;
    const wchar_t *lower[] = { L"i", L"ii", L"iii", L"iv", L"v" };
    wchar_t *b = make_front(lower, 5, 10);
    run(b, &o, &r);
    CHECK(r.n_pagenums == 5, "lower-case roman front matter removed (got %zu)", r.n_pagenums);
    preproc_free(&r); free(b);

    const wchar_t *upper[] = { L"IX", L"X", L"XI", L"XII" };
    b = make_front(upper, 4, 9);
    run(b, &o, &r);
    CHECK(r.n_pagenums == 4, "upper-case roman removed (got %zu)", r.n_pagenums);
    preproc_free(&r); free(b);

    const wchar_t *lone[] = { L"iv" };                         /* no evidence: kept */
    b = make_front(lone, 1, 10);
    run(b, &o, &r);
    CHECK(r.n_pagenums == 0, "lone roman numeral kept (got %zu)", r.n_pagenums);
    preproc_free(&r); free(b);

    const wchar_t *mixed[] = { L"iv", L"5", L"ix", L"43" };    /* 4 and 5 are adjacent, but arabic can't vouch for roman */
    b = make_front(mixed, 4, 10);
    run(b, &o, &r);
    CHECK(r.n_pagenums == 0, "roman and arabic do not give each other evidence (got %zu)", r.n_pagenums);
    preproc_free(&r); free(b);

    const wchar_t *words[] = { L"mix", L"mid", L"dim", L"did" };   /* roman-looking words */
    b = make_front(words, 4, 10);
    run(b, &o, &r);
    CHECK(r.n_pagenums == 0, "words made of roman letters are kept (got %zu)", r.n_pagenums);
    preproc_free(&r); free(b);

    expect("list of roman items stays", L"I\nII\nIII\nIV\n", &o, L"I\nII\nIII\nIV");
    expect("decorated roman", L"some text here\nPage iv\nmore text here", &o, L"some text here\nmore text here");
    expect("dashed roman", L"some text here\n- xii -\nmore text here", &o, L"some text here\nmore text here");
    expect("invalid roman", L"some text here\nIIII\nmore text here", &o, L"some text here\nIIII\nmore text here");
}

static void test_join_rules(void)
{
    PreprocOptions o = defaults();
    wchar_t *book = make_book(1, 12, 0, 0);
    PreprocResult r;
    run(book, &o, &r);
    CHECK(r.hard_wrapped, "wrapped detected");
    CHECK(wcschr(r.text, L'\n') == NULL, "single paragraph: all lines joined");
    preproc_free(&r);
    free(book);

    /* paragraph break survives; heading kept as its own paragraph; list items stay separate */
    const wchar_t *src =
        L"Introduction\n"
        L"The economy of the early period was shaped by many forces\n"
        L"that interacted in ways the older histories rarely noted\n"
        L"and which only careful study of the records has revealed\n"
        L"to us in the years since the archives were first opened.\n"
        L"\n"
        L"The second paragraph is also long enough to look wrapped,\n"
        L"and it too continues over several lines of about the same\n"
        L"width, until it finally ends here.\n"
        L"Another paragraph without a blank line starts right here and\n"
        L"runs on over a second line of ordinary prose for a while now\n"
        L"before stopping.\n"
        L"- first item of a list that is somewhat long as well, really\n"
        L"- second item of the list which is also fairly long indeed\n";
    run(src, &o, &r);
    CHECK(wcsstr(r.text, L"Introduction\n\nThe economy") != NULL, "heading separated from body: \"%ls\"", r.text);
    CHECK(wcsstr(r.text, L"opened.\n\nThe second") != NULL, "blank-line paragraph break kept");
    CHECK(wcsstr(r.text, L"ends here.\n\nAnother paragraph") != NULL, "short sentence-ending line starts a new paragraph");
    CHECK(wcsstr(r.text, L"stopping.\n\n- first") != NULL, "list item starts a paragraph");
    CHECK(wcsstr(r.text, L"really\n\n- second") != NULL || wcsstr(r.text, L"really\n- second") != NULL, "list items kept separate");
    preproc_free(&r);

    /* Text that is NOT hard-wrapped (paragraph-per-line) is left alone */
    expect("unwrapped untouched",
           L"Short line one.\nA much longer second line that does go on and on for quite a bit of text here, indeed.\nThree.\n",
           &o, L"Short line one.\nA much longer second line that does go on and on for quite a bit of text here, indeed.\nThree.");
}

static void test_change_log(void)
{
    PreprocOptions o = defaults();
    const wchar_t *src = L"Page 3 of 90\nThe indus-\ntrial \ufb01rst revolution began in Britain and then it\nspread widely afterwards.\n";
    PreprocResult r;
    run(src, &o, &r);
    int seen[8] = {0};
    size_t prev = 0;
    for (int i = 0; i < r.nchanges; i++) {
        seen[r.changes[i].kind]++;
        CHECK(r.changes[i].src_start >= prev, "change log sorted by source offset");
        prev = r.changes[i].src_start;
    }
    CHECK(seen[CH_PAGENUM] == 1 && seen[CH_HYPHEN] == 1 && seen[CH_UNICODE] == 1, "page/hyphen/unicode logged");
    preproc_free(&r);
    wchar_t *book = make_book(1, 12, 0, 0);
    run(book, &o, &r);
    int joins = 0;
    for (int i = 0; i < r.nchanges; i++) if (r.changes[i].kind == CH_JOIN) joins++;
    CHECK(joins == 11, "11 line joins logged for a 12-line paragraph (got %d)", joins);
    preproc_free(&r);
    free(book);
}

static void test_unicode_ws_urls(void)
{
    PreprocOptions o = defaults();
    expect("ligatures", L"The \ufb01nal \ufb02ight o\ufb00ers.", &o, L"The final flight offers.");
    expect("nbsp and zero width", L"A\u00a0B\u200bC  D\t\tE", &o, L"A BC D E");
    expect("crlf and blank runs", L"One.\r\n\r\n\r\n\r\nTwo.\r\n", &o, L"One.\n\nTwo.");
    expect("trailing spaces", L"alpha   \n   beta", &o, L"alpha\nbeta");

    PreprocOptions u = o; u.remove_urls = 1; u.remove_citations = 1;
    expect("url removal", L"Visit https://example.com/a?b=1, or www.test.org. Thanks.", &u, L"Visit, or. Thanks.");
    expect("citation removal", L"This is known [12]. Also [1, 2] and [3\u20135] here.", &u, L"This is known. Also and here.");
    expect("brackets that are not citations", L"The array [i] and [a-z] stay.", &u, L"The array [i] and [a-z] stay.");
    expect("urls kept by default", L"See www.example.com now.", &o, L"See www.example.com now.");

    PreprocOptions off; memset(&off, 0, sizeof off);
    expect("disabled = identity (LF only)", L"A \ufb01  b\r\nc", &off, L"A \ufb01  b\nc");
}

/* The key property: every normalized char that was copied 1:1 maps back to the
 * identical char in the SOURCE; mapped spans round-trip through src_to_norm. */
static void check_map(const wchar_t *src, const PreprocOptions *o, const char *label)
{
    PreprocResult r;
    size_t sl = wcslen(src);
    if (!preproc_run(src, sl, o, 0, &r)) { CHECK(0, "run failed"); return; }
    size_t bad = 0;
    for (size_t i = 0; i < r.len; i++) {
        wchar_t c = r.text[i];
        if (c == ' ' || c == '\n') continue;
        size_t s = preproc_norm_to_src(&r, i);
        if (s >= sl) { bad++; continue; }
        wchar_t sc = src[s];
        int lig = sc >= 0xFB00 && sc <= 0xFB06;
        if (sc != c && !lig && !(sc == 0x2010 || sc == 0x2011)) bad++;
    }
    CHECK(bad == 0, "%s: %zu normalized chars map to the wrong source char", label, bad);
    /* monotone, and src_to_norm(norm_to_src(i)) == i for copied chars */
    size_t prev = 0, rt_bad = 0;
    for (size_t i = 0; i < r.len; i++) {
        size_t s = preproc_norm_to_src(&r, i);
        if (s < prev) { rt_bad++; }
        prev = s;
        wchar_t c = r.text[i];
        if (c != ' ' && c != '\n') {
            wchar_t sc = src[s < sl ? s : sl - 1];
            if (sc == c && preproc_src_to_norm(&r, s) != i) rt_bad++;
        }
    }
    CHECK(rt_bad == 0, "%s: %zu monotonicity/round-trip failures", label, rt_bad);
    CHECK(preproc_norm_end_to_src(&r, r.len) <= sl, "%s: end beyond source", label);
    preproc_free(&r);
}

static void test_map(void)
{
    PreprocOptions o = defaults();
    o.remove_urls = o.remove_citations = 1;
    wchar_t *book = make_book(6, 30, 1, 1);
    check_map(book, &o, "book");
    free(book);
    check_map(L"The indus-\ntrial \ufb01rst  revolution [3] at www.x.org, ok.\r\n\r\n\r\nNext\u00a0para\u200b here.", &o, "mixed");
    check_map(L"a\n\n\n\nb\n- 5 -\nc", &o, "blanks");
}

static void test_integration(void)
{
    /* Sentence spans (in normalized space) map back to the right words in the source. */
    PreprocOptions o = defaults();
    const wchar_t *src =
        L"Page 3 of 90\n"
        L"The indus-\n"
        L"trial revolution began in Britain. It spread\n"
        L"widely afterwards.\n";
    PreprocResult r; Document d; doc_init(&d);
    size_t sl = wcslen(src);
    CHECK(preproc_run(src, sl, &o, 0, &r), "run");
    wchar_t *txt = r.text; size_t tl = r.len; r.text = NULL;
    CHECK(doc_build_take(&d, txt, tl), "build");
    CHECK(d.nsent == 2, "two sentences, got %d (\"%ls\")", d.nsent, d.text);
    if (d.nsent == 2) {
        wchar_t *s0 = doc_speech_text(&d, 0);
        CHECK(wcscmp(s0, L"The industrial revolution began in Britain.") == 0, "first sentence is repaired: \"%ls\"", s0);
        free(s0);
        size_t a = preproc_norm_to_src(&r, d.sents[1].start);
        size_t b = preproc_norm_end_to_src(&r, d.sents[1].start + d.sents[1].len);
        CHECK(wcsncmp(src + a, L"It spread", 9) == 0, "second sentence starts at \"It spread\" in source");
        CHECK(b == sl - 1 || b == sl, "second sentence ends at end of source (%zu vs %zu)", b, sl);
        size_t caret = (size_t)(wcsstr(src, L"widely") - src);
        size_t nn = preproc_src_to_norm(&r, caret);
        CHECK(doc_sentence_at(&d, nn) == 1, "caret in 'widely' is in sentence 2");
        caret = (size_t)(wcsstr(src, L"Page 3") - src);     /* inside a removed header */
        CHECK(doc_sentence_at(&d, preproc_src_to_norm(&r, caret)) == 0, "caret in removed header -> first sentence");
    }
    doc_free(&d);
    preproc_free(&r);
}

static void test_fuzz(void)
{
    static const wchar_t alphabet[] = L"abcdefXYZ  \n\n\r-.!?,0123456789\u00ad\u00a0\ufb01\u200b[]/:\tPage of ";
    size_t na = wcslen(alphabet);
    srand(777);
    for (int iter = 0; iter < 4000; iter++) {
        size_t n = (size_t)(rand() % 600);
        wchar_t *buf = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        for (size_t i = 0; i < n; i++) buf[i] = alphabet[rand() % na];
        buf[n] = 0;
        PreprocOptions o = defaults();
        o.remove_urls = rand() & 1; o.remove_citations = rand() & 1;
        o.join_lines = rand() & 1; o.fix_hyphens = rand() & 1; o.normalize_ws = rand() & 1;
        o.normalize_unicode = rand() & 1; o.repeated_lines = rand() & 1; o.page_numbers = rand() & 1;
        PreprocResult r;
        if (!preproc_run(buf, n, &o, 1, &r)) { CHECK(0, "fuzz run failed"); free(buf); break; }
        CHECK(wcslen(r.text) == r.len, "text length consistent");
        size_t cover = 0;
        for (int m = 0; m < r.nmap; m++) {
            CHECK(r.map[m].norm_start == cover, "map contiguous in normalized space");
            cover += r.map[m].norm_len;
            CHECK(r.map[m].src_start + r.map[m].src_len <= n, "map entry inside source");
        }
        CHECK(cover == r.len, "map covers whole text");
        for (int c = 0; c < r.nchanges; c++)
            CHECK(r.changes[c].src_start + r.changes[c].src_len <= n, "change inside source");
        for (size_t i = 0; i < r.len; i += 7) CHECK(preproc_norm_to_src(&r, i) <= n, "norm_to_src in range");
        for (size_t i = 0; i <= n; i += 5) CHECK(preproc_src_to_norm(&r, i) <= r.len, "src_to_norm in range");
        Document d; doc_init(&d);
        wchar_t *t2 = r.text; size_t l2 = r.len; r.text = NULL;
        CHECK(doc_build_take(&d, t2, l2), "doc build after preproc");
        doc_free(&d);
        preproc_free(&r);
        free(buf);
    }
}

int main(void)
{
    test_hyphens();
    test_headers_and_pagenums();
    test_roman();
    test_join_rules();
    test_change_log();
    test_unicode_ws_urls();
    test_map();
    test_integration();
    test_fuzz();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
