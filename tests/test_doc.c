/* Console test runner for the pure-C core (document model + segmenter).
 * Builds with any C11 compiler:  make test */
#include "../src/doc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void dump(const Document *d)
{
    for (int i = 0; i < d->nsent; i++) {
        wchar_t *s = doc_speech_text(d, i);
        printf("    [%d] \"%ls\"\n", i, s);
        free(s);
    }
}

/* Expect exactly `n` sentences whose speech text equals the given strings. */
static void expect(const wchar_t *input, int n, ...)
{
    Document d; doc_init(&d);
    CHECK(doc_build(&d, input, wcslen(input)), "build failed");
    int ok = d.nsent == n;
    CHECK(d.nsent == n, "input \"%ls\": expected %d sentences, got %d", input, n, d.nsent);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        const wchar_t *want = va_arg(ap, const wchar_t *);
        if (i < d.nsent) {
            wchar_t *got = doc_speech_text(&d, i);
            if (wcscmp(got, want) != 0) { ok = 0; CHECK(0, "sentence %d: expected \"%ls\" got \"%ls\"", i, want, got); }
            free(got);
        }
    }
    va_end(ap);
    if (!ok) dump(&d);
    doc_free(&d);
}

static void test_basic(void)
{
    expect(L"Mr. Smith went home. He returned later.", 2, L"Mr. Smith went home.", L"He returned later.");
    expect(L"Dr. J. S. Mill wrote many books. They were good.", 2, L"Dr. J. S. Mill wrote many books.", L"They were good.");
    expect(L"The value was $3.14. It was fair.", 2, L"The value was $3.14.", L"It was fair.");
    expect(L"Pi is 3.14159 exactly.", 1, L"Pi is 3.14159 exactly.");
    expect(L"Visit https://example.com for details. Then leave.", 2, L"Visit https://example.com for details.", L"Then leave.");
    expect(L"Go to example.com. Then wait.", 2, L"Go to example.com.", L"Then wait.");
    expect(L"This is e.g. an example. Next one.", 2, L"This is e.g. an example.", L"Next one.");
    expect(L"He lives in the U.S. The economy is large.", 2, L"He lives in the U.S.", L"The economy is large.");
    expect(L"The U.S. economy is large.", 1, L"The U.S. economy is large.");
    expect(L"See Fig. 4 for details. Next.", 2, L"See Fig. 4 for details.", L"Next.");
    expect(L"He bought apples, oranges, etc. and left. Then he slept.", 2,
           L"He bought apples, oranges, etc. and left.", L"Then he slept.");
    expect(L"Really?! Yes. He said \"Go.\" Then left.", 4, L"Really?!", L"Yes.", L"He said \"Go.\"", L"Then left.");
    expect(L"\"Hello.\" he said.", 1, L"\"Hello.\" he said.");
    expect(L"Wait... What? OK", 3, L"Wait...", L"What?", L"OK");
    expect(L"wait... what", 1, L"wait... what");
    expect(L"Caf\u00e9 \u00e9tait l\u00e0. O\u00f9 est-il?", 2, L"Caf\u00e9 \u00e9tait l\u00e0.", L"O\u00f9 est-il?");
    expect(L"He was born in 1984. She was not.", 2, L"He was born in 1984.", L"She was not.");
    expect(L"Ask Prof. Jones. She knows.", 2, L"Ask Prof. Jones.", L"She knows.");
    expect(L"(See the U.S.) The end.", 2, L"(See the U.S.)", L"The end.");
    expect(L"No sentence terminator here", 1, L"No sentence terminator here");
}

static void test_pdf_lines(void)
{
    expect(L"This is a very long paragraph copied from a PDF\nwhere almost every line has been broken.\n", 1,
           L"This is a very long paragraph copied from a PDF where almost every line has been broken.");
    expect(L"First line of one sentence that\r\nwraps. Second sentence\r\nalso wraps.\r\n", 2,
           L"First line of one sentence that wraps.", L"Second sentence also wraps.");
}

static void test_paragraphs(void)
{
    Document d; doc_init(&d);
    const wchar_t *src = L"One.\n\nTwo.\r\n\r\n\r\n   \nThree. Four.";
    CHECK(doc_build(&d, src, wcslen(src)), "build");
    CHECK(d.npara == 3, "paragraphs: expected 3, got %d", d.npara);
    CHECK(d.nsent == 4, "sentences: expected 4, got %d", d.nsent);
    if (d.npara == 3) {
        CHECK(d.paras[2].sentence_count == 2 && d.paras[2].first_sentence == 2, "para 2 bookkeeping");
        CHECK(d.sents[3].para == 2, "sentence->paragraph link");
    }
    doc_free(&d);

    CHECK(doc_build(&d, L"", 0) && d.nsent == 0 && d.npara == 0, "empty doc");
    CHECK(doc_sentence_at(&d, 0) == -1, "sentence_at on empty doc");
    CHECK(doc_build(&d, L" \n\t \r\n\n  ", 9) && d.nsent == 0, "whitespace-only doc");
    doc_free(&d);
}

static void test_offsets(void)
{
    Document d; doc_init(&d);
    const wchar_t *src = L"Alpha one.  Beta two.\n\nGamma three.";
    doc_build(&d, src, wcslen(src));
    CHECK(d.nsent == 3, "3 sentences, got %d", d.nsent);
    CHECK(doc_sentence_at(&d, 0) == 0, "offset 0");
    CHECK(doc_sentence_at(&d, 10) == 0, "offset at end of sentence 0 (inclusive)");
    CHECK(doc_sentence_at(&d, 11) == 1, "offset in gap -> next sentence");
    CHECK(doc_sentence_at(&d, 14) == 1, "inside sentence 1");
    CHECK(doc_sentence_at(&d, 9999) == 2, "past the end -> last");
    /* spans must map back to the untouched source */
    CHECK(wcsncmp(d.text + d.sents[2].start, L"Gamma three.", d.sents[2].len) == 0, "span maps to source");
    doc_free(&d);
}

static void test_long_sentence(void)
{
    enum { N = 3000 };
    wchar_t *buf = (wchar_t *)malloc((N + 2) * sizeof(wchar_t));
    size_t n = 0;
    while (n + 6 < N) { memcpy(buf + n, L"word, ", 6 * sizeof(wchar_t)); n += 6; }
    buf[n] = 0;
    Document d; doc_init(&d);
    doc_build(&d, buf, n);
    CHECK(d.nsent >= 4, "long run-on split into >=4 pieces, got %d", d.nsent);
    size_t total_words = 0;
    for (int i = 0; i < d.nsent; i++) {
        CHECK(d.sents[i].len <= DOC_MAX_SENTENCE, "piece %d too long (%zu)", i, d.sents[i].len);
        wchar_t *s = doc_speech_text(&d, i);
        for (wchar_t *p = s; *p; p++) if (*p == ' ' || p[1] == 0) total_words++;
        free(s);
    }
    CHECK(total_words == n / 6, "no words lost/duplicated by splitting (%zu vs %zu)", total_words, n / 6);
    doc_free(&d);

    /* no whitespace at all: hard cut must still terminate and cover the text */
    wchar_t *blob = (wchar_t *)malloc(2001 * sizeof(wchar_t));
    for (int i = 0; i < 2000; i++) blob[i] = 'x';
    doc_build(&d, blob, 2000);
    size_t covered = 0;
    for (int i = 0; i < d.nsent; i++) covered += d.sents[i].len;
    CHECK(covered == 2000 && d.nsent == 3, "hard-cut covers all text (%zu, %d pieces)", covered, d.nsent);
    doc_free(&d);
    free(blob); free(buf);
}

/* Structural invariants on garbage input: ordered, in-bounds, non-overlapping,
 * trimmed, parent links consistent. Run under ASan/UBSan via `make test`. */
static void test_fuzz(void)
{
    static const wchar_t alphabet[] = L"abcXYZ  \n\n\r.!?\"')(,;:-0123456789\u00e9\u2026\t";
    size_t na = wcslen(alphabet);
    srand(12345);
    for (int iter = 0; iter < 3000; iter++) {
        size_t n = (size_t)(rand() % 400);
        wchar_t *buf = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        for (size_t i = 0; i < n; i++) buf[i] = alphabet[rand() % na];
        buf[n] = 0;
        Document d; doc_init(&d);
        if (!doc_build(&d, buf, n)) { CHECK(0, "build failed in fuzz"); break; }
        size_t prev_end = 0;
        for (int i = 0; i < d.nsent; i++) {
            const Sentence *s = &d.sents[i];
            int bad = s->len == 0 || s->start < prev_end || s->start + s->len > n ||
                      s->para < 0 || s->para >= d.npara ||
                      d.text[s->start] == ' ' || d.text[s->start + s->len - 1] == ' ' ||
                      d.text[s->start] == '\n' || d.text[s->start + s->len - 1] == '\n';
            if (bad) { CHECK(0, "invariant violated, iter %d sentence %d", iter, i); break; }
            prev_end = s->start + s->len;
            wchar_t *sp = doc_speech_text(&d, i);
            CHECK(sp && sp[0] != ' ', "speech text trimmed");
            free(sp);
        }
        int count = 0;
        for (int p = 0; p < d.npara; p++) count += d.paras[p].sentence_count;
        CHECK(count == d.nsent, "paragraph sentence counts sum to total");
        if (n) { int k = doc_sentence_at(&d, (size_t)rand() % (n + 5)); CHECK(d.nsent ? (k >= 0 && k < d.nsent) : k == -1, "sentence_at range"); }
        doc_free(&d);
        free(buf);
    }
}

int main(void)
{
    test_basic();
    test_pdf_lines();
    test_paragraphs();
    test_offsets();
    test_long_sentence();
    test_fuzz();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
