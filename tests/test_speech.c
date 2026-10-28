/* Tests for utf8.c, pron.c and speech.c (pure C). */
#include "../src/speech.h"
#include "../src/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static wchar_t *speak(const Speech *sp, const wchar_t *in)
{
    SpeechText t;
    if (!speech_render(sp, in, wcslen(in), 0, &t)) return NULL;
    return t.text;
}
static void expect(const Speech *sp, const wchar_t *in, const wchar_t *want)
{
    wchar_t *got = speak(sp, in);
    CHECK(got && wcscmp(got, want) == 0, "\"%ls\"\n  want \"%ls\"\n  got  \"%ls\"", in, want, got ? got : L"(null)");
    free(got);
}
static Speech defaults(const PronDict *d)
{
    Speech s; memset(&s, 0, sizeof s); s.dict = d; speech_default_options(&s.opts); return s;
}

static void test_words(void)
{
    struct { unsigned long long n; const wchar_t *w; } T[] = {
        { 0, L"zero" }, { 7, L"seven" }, { 13, L"thirteen" }, { 21, L"twenty-one" }, { 100, L"one hundred" },
        { 101, L"one hundred one" }, { 999, L"nine hundred ninety-nine" }, { 1000, L"one thousand" },
        { 1984, L"one thousand nine hundred eighty-four" }, { 12345, L"twelve thousand three hundred forty-five" },
        { 1000000, L"one million" }, { 2500000, L"two million five hundred thousand" },
        { 1000000001ULL, L"one billion one" }, { 999999999999ULL, L"nine hundred ninety-nine billion nine hundred ninety-nine million nine hundred ninety-nine thousand nine hundred ninety-nine" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        wchar_t *w = speech_number_words(T[i].n);
        CHECK(w && wcscmp(w, T[i].w) == 0, "%llu: got \"%ls\"", T[i].n, w ? w : L"(null)");
        free(w);
    }
}

static void test_numbers(void)
{
    Speech s = defaults(NULL);
    expect(&s, L"He was born in 1984.", L"He was born in nineteen eighty-four.");
    expect(&s, L"Since 2005 it grew.", L"Since two thousand five it grew.");
    expect(&s, L"In 2010 and in 1900.", L"In twenty ten and in nineteen hundred.");
    expect(&s, L"In 1905 it began.", L"In nineteen oh five it began.");
    expect(&s, L"Music of the 1980s was loud.", L"Music of the nineteen eighties was loud.");
    expect(&s, L"From 1990-2000 it grew.", L"From nineteen ninety to two thousand it grew.");
    expect(&s, L"I counted 1984 sheep.", L"I counted one thousand nine hundred eighty-four sheep.");
    expect(&s, L"There were 42 of them.", L"There were forty-two of them.");
    expect(&s, L"It has 1,234,567 parts.", L"It has one million two hundred thirty-four thousand five hundred sixty-seven parts.");
    expect(&s, L"Pi is 3.14159 exactly.", L"Pi is three point one four one five nine exactly.");
    expect(&s, L"Call 007 now.", L"Call zero zero seven now.");
    expect(&s, L"The 1st, 2nd, 3rd and 21st and 100th.", L"The first, second, third and twenty-first and one hundredth.");
    expect(&s, L"Growth was 5% and 3.5%.", L"Growth was five percent and three point five percent.");
    expect(&s, L"It fell to -5 degrees.", L"It fell to minus five degrees.");
    expect(&s, L"The year is 2025.", L"The year is two thousand twenty-five.");
}

static void test_currency(void)
{
    Speech s = defaults(NULL);
    expect(&s, L"It cost $25.", L"It cost twenty-five dollars.");
    expect(&s, L"It cost $1 only.", L"It cost one dollar only.");
    expect(&s, L"The value was $3.14.", L"The value was three dollars and fourteen cents.");
    expect(&s, L"Only $0.50 left.", L"Only fifty cents left.");
    expect(&s, L"A $1,500 fee.", L"A one thousand five hundred dollars fee.");
    expect(&s, L"Raised $5 million today.", L"Raised five million dollars today.");
    expect(&s, L"Worth \u00a320 and \u20ac7.", L"Worth twenty pounds and seven euros.");
}

static void test_leave_alone(void)
{
    Speech s = defaults(NULL);
    expect(&s, L"Meet at 10:30 or 12/05/2020.", L"Meet at 10:30 or 12/05/2020.");
    expect(&s, L"See section 3.2.1 now.", L"See section 3.2.1 now.");
    expect(&s, L"In 1990-2000 and 1990\u20132000.", L"In nineteen ninety to two thousand and nineteen ninety to two thousand.");
    expect(&s, L"Items 10-20 are due.", L"Items ten-twenty are due.");
}

static void test_abbrev_urls_tech(void)
{
    Speech s = defaults(NULL);
    expect(&s, L"The Dr. Smith study examines it.", L"The Doctor Smith study examines it.");
    expect(&s, L"Mr. and Mrs. Jones met Prof. Lee.", L"Mister and Missus Jones met Professor Lee.");
    expect(&s, L"See Fig. 4 and No. 5.", L"See Figure four and Number five.");
    expect(&s, L"Apples, etc. were sold.", L"Apples, et cetera were sold.");
    expect(&s, L"He bought apples, etc.", L"He bought apples, et cetera.");
    expect(&s, L"Use tools, e.g. a saw; i.e. a tool.", L"Use tools, for example a saw; that is a tool.");
    expect(&s, L"Acme Inc. sued Beta Ltd.", L"Acme Incorporated sued Beta Limited.");
    expect(&s, L"Visit https://example.com/a?b=1, or www.test.org. Thanks.", L"Visit web address, or web address. Thanks.");
    expect(&s, L"Mail bob@example.com now.", L"Mail email address now.");
    expect(&s, L"We use C++ and C# and .NET daily.", L"We use C plus plus and C sharp and dot net daily.");
    expect(&s, L"It runs on x86 and mp3 and 5km.", L"It runs on x86 and mp3 and 5km.");
    expect(&s, L"The dr. said so, and Dr said it.", L"The dr. said so, and Dr said it.");   /* case-sensitive: only "Dr." */

    s.opts.abbreviations = 0; s.opts.numbers = 0; s.opts.urls = 0; s.opts.technical = 0;
    expect(&s, L"Dr. Smith paid $25 for C++ at www.x.org in 1984.", L"Dr. Smith paid $25 for C++ at www.x.org in 1984.");
}

static void test_dictionary(void)
{
    PronDict d; pron_init(&d);
    CHECK(pron_add(&d, L"Machiavelli", L"Mack ee uh vell ee", 0, 0) == 0, "add 1");
    CHECK(pron_add(&d, L"Nietzsche", L"Nee chuh", 0, 0) == 1, "add 2");
    CHECK(pron_add(&d, L"machiavelli", L"x", 0, 0) == -1, "duplicate (case-insensitive) rejected");
    CHECK(pron_add(&d, L"bad\tword", L"x", 0, 0) == -2, "tab rejected");
    CHECK(pron_add(&d, L"", L"x", 0, 0) == -2, "empty rejected");
    pron_add(&d, L"Dr.", L"Dock tor", 1, 0);
    pron_add(&d, L"New York City", L"the Big Apple", 0, 0);
    pron_add(&d, L"New York", L"Noo York", 0, 0);
    pron_add(&d, L"SQL", L"sequel", 1, 0);
    pron_add(&d, L"ism", L"izzum", 0, 1);
    pron_prepare(&d);
    Speech s = defaults(&d);

    expect(&s, L"The Dr. Smith study examines Machiavelli.", L"The Dock tor Smith study examines Mack ee uh vell ee.");
    expect(&s, L"MACHIAVELLI's work and nietzsche.", L"Mack ee uh vell ee's work and Nee chuh.");
    expect(&s, L"Machiavellis is not a word here.", L"Machiavellis is not a word here.");              /* whole word only */
    expect(&s, L"I love New York City and New York.", L"I love the Big Apple and Noo York.");           /* longest first */
    expect(&s, L"We use SQL, not sql.", L"We use sequel, not sql.");                                    /* match case */
    expect(&s, L"Capitalism and truism.", L"Capitalizzum and truizzum.");                               /* partial */
    expect(&s, L"Dr. Who", L"Dock tor Who");                                                            /* beats the built-in Dr. rule */

    /* dictionary output is not rescanned by the number/abbrev rules */
    PronDict d2; pron_init(&d2);
    pron_add(&d2, L"R2D2", L"Dr. 42", 0, 0);
    pron_prepare(&d2);
    Speech s2 = defaults(&d2);
    expect(&s2, L"Meet R2D2.", L"Meet Dr. 42.");

    /* index and slow path agree */
    Speech s3 = defaults(&d);
    d.index_valid = 0;
    expect(&s3, L"I love New York City and New York.", L"I love the Big Apple and Noo York.");
    pron_prepare(&d);

    /* update / remove */
    int i = pron_find(&d, L"nietzsche");
    CHECK(i == 1, "find case-insensitively");
    CHECK(pron_set(&d, i, L"Nietzsche", L"Nee tchuh", 0, 0), "update");
    CHECK(!pron_set(&d, i, L"Machiavelli", L"x", 0, 0), "cannot rename onto another entry");
    uint64_t sig = pron_signature(&d);
    pron_remove(&d, i);
    CHECK(pron_find(&d, L"Nietzsche") < 0, "removed");
    CHECK(pron_signature(&d) != sig, "signature changes with content");

    /* serialization round trip, including unicode and an astral character */
    pron_clear(&d);
    pron_add(&d, L"caf\u00e9", L"ka fay", 0, 0);
    pron_add(&d, L"\u0394\u03b5\u03bb\u03c6\u03bf\u03af", L"del fee", 1, 1);
    wchar_t emoji[3] = { 0 };
    if (sizeof(wchar_t) == 2) { emoji[0] = 0xD83D; emoji[1] = 0xDE00; } else { emoji[0] = 0x1F600; }
    pron_add(&d, emoji, L"smile", 0, 0);
    uint64_t sig2 = pron_signature(&d);
    size_t len = 0;
    char *bytes = pron_to_utf8(&d, &len);
    CHECK(bytes && len > 0, "serialize");
    PronDict d3; pron_init(&d3);
    int bad = -1;
    int n = pron_from_utf8(&d3, bytes, len, &bad);
    CHECK(n == 3 && bad == 0, "reload %d entries, %d bad", n, bad);
    CHECK(pron_signature(&d3) == sig2, "round trip preserves content, flags and order");
    free(bytes);
    const char *messy = "\xEF\xBB\xBF# comment\r\nc\tFoo\tfoo bar\r\nnoTabsHere\r\n-\tOnly one tab\r\n\r\np\tzed\tzee\r\n";
    n = pron_from_utf8(&d3, messy, strlen(messy), &bad);
    CHECK(n == 2 && bad == 2, "messy file: %d loaded, %d bad", n, bad);
    pron_free(&d3);
    pron_free(&d2);
    pron_free(&d);
}

static void test_spans(void)
{
    PronDict d; pron_init(&d);
    pron_add(&d, L"Machiavelli", L"Mack ee uh vell ee", 0, 0);
    pron_prepare(&d);
    Speech s = defaults(&d);
    const wchar_t *in = L"The Dr. Smith study examines Machiavelli.";
    SpeechText t;
    CHECK(speech_render(&s, in, wcslen(in), 1, &t), "render");
    CHECK(t.nspans == 2, "two spans, got %d", t.nspans);
    if (t.nspans == 2) {
        CHECK(t.spans[0].kind == SPAN_NORM && wcsncmp(in + t.spans[0].in_start, L"Dr.", 3) == 0, "span 0 is Dr.");
        CHECK(wcsncmp(t.text + t.spans[0].out_start, L"Doctor", t.spans[0].out_len) == 0 && t.spans[0].out_len == 6, "span 0 output");
        CHECK(t.spans[1].kind == SPAN_DICT && wcsncmp(in + t.spans[1].in_start, L"Machiavelli", 11) == 0, "span 1 is the dictionary word");
        CHECK(wcsncmp(t.text + t.spans[1].out_start, L"Mack ee uh vell ee", t.spans[1].out_len) == 0, "span 1 output");
    }
    speech_text_free(&t);
    pron_free(&d);

    /* sentence helper: whitespace collapsed first */
    Document doc; doc_init(&doc);
    const wchar_t *src = L"Dr. Smith paid\n$25 for it.  Then left.";
    doc_build(&doc, src, wcslen(src));
    Speech s2 = defaults(NULL);
    wchar_t *x = speech_sentence_text(&s2, &doc, 0);
    CHECK(x && wcscmp(x, L"Doctor Smith paid twenty-five dollars for it.") == 0, "sentence text: \"%ls\"", x ? x : L"(null)");
    free(x);
    doc_free(&doc);
}

static void test_utf8(void)
{
    const wchar_t *w = L"A\u00e9\u20ac z";
    size_t n = utf8_from_wide(w, wcslen(w), NULL);
    char *b = (char *)malloc(n + 1);
    utf8_from_wide(w, wcslen(w), b);
    size_t m = wide_from_utf8(b, n, NULL);
    wchar_t *back = (wchar_t *)malloc((m + 1) * sizeof(wchar_t));
    wide_from_utf8(b, n, back); back[m] = 0;
    CHECK(wcscmp(back, w) == 0, "utf8 round trip");
    const char bad[] = { 'a', (char)0xC3, 'b', (char)0xFF, (char)0xE2, (char)0x82 };
    size_t bm = wide_from_utf8(bad, sizeof bad, NULL);
    CHECK(bm >= 4, "invalid bytes are replaced, not dropped silently");
    free(b); free(back);
}

static void test_fuzz(void)
{
    static const wchar_t alphabet[] = L"abcDr.Mr. 0123456789$%,.:/-+C#@wwwhttp://ste ndrdth\u00e9\u2026\u20ac()'\"s ";
    size_t na = wcslen(alphabet);
    PronDict d; pron_init(&d);
    pron_add(&d, L"ab", L"A B", 0, 0);
    pron_add(&d, L"Dr.", L"X", 1, 0);
    pron_add(&d, L"12", L"twelve!", 0, 1);
    pron_prepare(&d);
    srand(4242);
    for (int iter = 0; iter < 6000; iter++) {
        size_t n = (size_t)(rand() % 120);
        wchar_t *buf = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        for (size_t i = 0; i < n; i++) buf[i] = alphabet[rand() % na];
        buf[n] = 0;
        Speech s = defaults((iter & 1) ? &d : NULL);
        s.opts.abbreviations = rand() & 1; s.opts.numbers = rand() & 1; s.opts.urls = rand() & 1; s.opts.technical = rand() & 1;
        SpeechText t;
        if (!speech_render(&s, buf, n, 1, &t)) { CHECK(0, "render failed"); free(buf); break; }
        CHECK(wcslen(t.text) == t.len, "length consistent");
        size_t prev_in = 0, prev_out = 0;
        for (int k = 0; k < t.nspans; k++) {
            const SpeechSpan *sp = &t.spans[k];
            CHECK(sp->in_start >= prev_in && sp->in_start + sp->in_len <= n, "span inside input, ordered");
            CHECK(sp->out_start >= prev_out && sp->out_start + sp->out_len <= t.len, "span inside output, ordered");
            prev_in = sp->in_start + sp->in_len; prev_out = sp->out_start + sp->out_len;
        }
        speech_text_free(&t);
        free(buf);
    }
    pron_free(&d);
}

int main(void)
{
    test_words();
    test_numbers();
    test_currency();
    test_leave_alone();
    test_abbrev_urls_tech();
    test_dictionary();
    test_spans();
    test_utf8();
    test_fuzz();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
