/* Tests for ini.c, chapters.c (detection, overrides, bookmarks, file names) and chapter-aware IR/assembly. */
#include "../src/ini.h"
#include "../src/chapters.h"
#include "../src/ir.h"
#include "../src/assemble.h"
#include "../src/wav.h"
#include "../src/plat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------- ini */

static void test_ini(void)
{
    Ini d; ini_init(&d);
    CHECK(ini_set(&d, L"Speech", L"VoiceId", L"HKLM\\Voices\\Tokens\\David"), "set");
    CHECK(ini_set(&d, L"Speech", L"Rate", L"-2"), "set");
    CHECK(ini_set(&d, L"Project", L"Name", L"Caf\u00e9 \u0394\u03b5\u03bb\u03c6\u03bf\u03af \u65e5\u672c\u8a9e"), "unicode");
    CHECK(ini_set(&d, L"Project", L"Note", L"line one\nline two\r\nwith = equals and \\ backslash"), "multiline");
    wchar_t *big = (wchar_t *)malloc(3001 * sizeof(wchar_t));
    for (int i = 0; i < 3000; i++) big[i] = (wchar_t)(i % 97 == 0 ? '\n' : (i % 89 == 0 ? 0x00e9 : 'a' + i % 26));
    big[3000] = 0;
    CHECK(ini_set(&d, L"Project", L"Big", big), "long value");
    CHECK(ini_set(&d, L"Speech", L"Rate", L"-3"), "replace existing");
    CHECK(!ini_set(&d, L"Bad", L"key=with=equals", L"x") && !ini_set(&d, L"", L"k", L"x") && !ini_set(&d, L"Bad", L"", L"x"), "bad names rejected");
    ini_set_int(&d, L"Audio", L"ChunkSentences", 3);

    size_t len = 0;
    char *bytes = ini_to_utf8(&d, &len);
    CHECK(bytes && len > 0, "serialize");
    /* sections keep first-appearance order */
    const char *sp = strstr(bytes, "[Speech]"), *pr = strstr(bytes, "[Project]"), *au = strstr(bytes, "[Audio]");
    CHECK(sp && pr && au && sp < pr && pr < au, "section order preserved");

    Ini e; ini_init(&e);
    int n = ini_from_utf8(&e, bytes, len);
    CHECK(n == 6, "6 entries read back, got %d", n);
    CHECK(wcscmp(ini_get(&e, L"Speech", L"Rate"), L"-3") == 0, "replacement kept");
    CHECK(wcscmp(ini_get(&e, L"Project", L"Name"), L"Caf\u00e9 \u0394\u03b5\u03bb\u03c6\u03bf\u03af \u65e5\u672c\u8a9e") == 0, "unicode round trip");
    CHECK(wcscmp(ini_get(&e, L"Project", L"Note"), L"line one\nline two\r\nwith = equals and \\ backslash") == 0, "escapes round trip");
    CHECK(wcscmp(ini_get(&e, L"Project", L"Big"), big) == 0, "3000-char value round trip");
    CHECK(wcscmp(ini_get(&e, L"Speech", L"VoiceId"), L"HKLM\\Voices\\Tokens\\David") == 0, "backslashes round trip");
    CHECK(ini_get_int(&e, L"Audio", L"ChunkSentences", 9) == 3 && ini_get_int(&e, L"Audio", L"Missing", 9) == 9, "ints and defaults");
    CHECK(ini_get(&e, L"Audio", L"Nope") == NULL, "absent -> NULL");
    free(bytes); free(big);

    const char *messy = "\xEF\xBB\xBF; comment\r\n# another\r\n\r\n[A]\r\nk=v1\r\nbad line without equals\r\n=nokey\r\n[B]\r\nx= spaced \r\nk=v2\r\n";
    n = ini_from_utf8(&e, messy, strlen(messy));
    CHECK(n == 3 && wcscmp(ini_get(&e, L"A", L"k"), L"v1") == 0 && wcscmp(ini_get(&e, L"B", L"k"), L"v2") == 0, "BOM/CRLF/comments/bad lines (%d)", n);
    CHECK(wcscmp(ini_get(&e, L"B", L"x"), L" spaced ") == 0, "value whitespace preserved");
    ini_remove_section(&e, L"A");
    CHECK(ini_get(&e, L"A", L"k") == NULL && ini_get(&e, L"B", L"k") != NULL, "remove section");
    ini_free(&e); ini_free(&d);
}

/* -------------------------------------------------------------- chapters */

static const wchar_t *BOOK =
    L"Introduction\n\nText of the intro. More text here.\n\n"
    L"CHAPTER 1\n\nThe Beginning\n\nIt was a dark night. It rained.\n\n"
    L"CHAPTER TWO\n\nAnother paragraph here. Second sentence.\n\n"
    L"PART II\n\nChapter 3: The Third\n\nThird chapter text.\n\n"
    L"IV.\n\nRoman chapter text here.\n\n"
    L"Epilogue\n\nThe end.\n";

static void doc_from(Document *d, const wchar_t *t) { doc_init(d); doc_build(d, t, wcslen(t)); }

static void test_detect(void)
{
    Document d; doc_from(&d, BOOK);
    ChapterList l;
    CHECK(chapters_detect(&l, &d, NULL), "detect");
    CHECK(l.n == 6, "6 chapters, got %d", l.n);
    if (l.n == 6) {
        CHECK(wcscmp(l.c[0].title, L"Introduction") == 0, "special heading: \"%ls\"", l.c[0].title);
        CHECK(wcscmp(l.c[1].title, L"CHAPTER 1 - The Beginning") == 0, "title taken from next paragraph: \"%ls\"", l.c[1].title);
        CHECK(wcscmp(l.c[2].title, L"CHAPTER TWO") == 0, "next paragraph is prose, not a title: \"%ls\"", l.c[2].title);
        CHECK(wcscmp(l.c[3].title, L"Chapter 3: The Third") == 0, "PART folded into the chapter after it: \"%ls\"", l.c[3].title);
        CHECK(wcsncmp(d.text + d.paras[l.c[3].para].start, L"PART II", 7) == 0, "folded chapter starts at the PART heading");
        CHECK(wcscmp(l.c[4].title, L"IV.") == 0, "bare roman numeral: \"%ls\"", l.c[4].title);
        CHECK(wcscmp(l.c[5].title, L"Epilogue") == 0, "epilogue");
        for (int i = 1; i < l.n; i++) CHECK(l.c[i].first_sent > l.c[i - 1].first_sent, "chapters ordered");
        CHECK(chapter_of_sentence(&l, 0) == 0, "sentence 0 is in the introduction");
        CHECK(chapter_of_sentence(&l, d.nsent - 1) == 5, "last sentence in the last chapter");
        CHECK(chapter_last_sentence(&l, &d, 5) == d.nsent - 1 && chapter_last_sentence(&l, &d, 0) == l.c[1].first_sent - 1, "chapter ranges");
    }
    chapters_free(&l);
    doc_free(&d);

    /* false positives must not be headings */
    const wchar_t *prose =
        L"Chapter 3 shows that results vary widely across all of the experiments.\n\n"
        L"Part of the problem is obvious.\n\n"
        L"Book three was long.\n\n"
        L"The introduction was dull.\n\n"
        L"I went home.\n\n"
        L"Mix\n\n"
        L"Some ordinary closing paragraph with several words in it.\n";
    doc_from(&d, prose);
    chapters_detect(&l, &d, NULL);
    CHECK(l.n == 0, "no false headings in ordinary prose (got %d: \"%ls\")", l.n, l.n ? l.c[0].title : L"");
    chapters_free(&l);
    doc_free(&d);

    /* a lone PART with no chapter after it stands alone; bare front matter before the first chapter has chapter -1 */
    doc_from(&d, L"Some front matter text.\n\nPART I\n\nProse that is not a chapter heading at all, just text.\n");
    chapters_detect(&l, &d, NULL);
    CHECK(l.n == 1 && l.c[0].level == 1, "standalone part");
    CHECK(chapter_of_sentence(&l, 0) == -1, "front matter is chapter -1");
    chapters_free(&l);
    doc_free(&d);

    /* empty doc */
    doc_from(&d, L"");
    CHECK(chapters_detect(&l, &d, NULL) && l.n == 0, "empty");
    chapters_free(&l);
    doc_free(&d);
}

static void test_overrides(void)
{
    Document d; doc_from(&d, BOOK);
    ChapterList l;
    Overrides ov; memset(&ov, 0, sizeof ov);
    chapters_detect(&l, &d, NULL);
    int para_two = l.c[2].para;
    int para_prose = d.sents[l.c[2].first_sent + 1].para;           /* the paragraph after CHAPTER TWO */
    CHECK(para_prose == para_two + 1, "test setup");
    CHECK(overrides_remove_heading(&ov, &d, para_two), "remove");
    CHECK(overrides_add_heading(&ov, &d, para_prose, L"My Section"), "add");
    int ep_para = l.c[5].para;
    CHECK(overrides_rename(&ov, &d, ep_para, L"The End"), "rename");
    chapters_free(&l);

    CHECK(chapters_detect(&l, &d, &ov), "detect with overrides");
    CHECK(l.n == 6, "one removed, one added -> still 6 (got %d)", l.n);
    int found_added = 0, found_two = 0, found_end = 0;
    for (int i = 0; i < l.n; i++) {
        if (wcscmp(l.c[i].title, L"My Section") == 0) { found_added = 1; CHECK(l.c[i].manual, "marked manual"); }
        if (wcscmp(l.c[i].title, L"CHAPTER TWO") == 0) found_two = 1;
        if (wcscmp(l.c[i].title, L"The End") == 0) found_end = 1;
    }
    CHECK(found_added && !found_two && found_end, "add/remove/rename applied");
    chapters_free(&l);

    /* persistence */
    Ini ini; ini_init(&ini);
    overrides_to_ini(&ov, &ini);
    size_t len; char *bytes = ini_to_utf8(&ini, &len);
    Ini ini2; ini_init(&ini2);
    ini_from_utf8(&ini2, bytes, len);
    Overrides ov2; memset(&ov2, 0, sizeof ov2);
    overrides_from_ini(&ov2, &ini2);
    CHECK(ov2.n == ov.n, "overrides round trip (%d vs %d)", ov2.n, ov.n);
    free(bytes); ini_free(&ini); ini_free(&ini2);

    /* the corrections survive edits elsewhere: add text at the top and a paragraph in the middle */
    wchar_t *t2 = (wchar_t *)malloc(4000 * sizeof(wchar_t));
    swprintf(t2, 4000, L"A brand new opening paragraph.\n\n%ls", BOOK);
    Document d2; doc_from(&d2, t2);
    chapters_detect(&l, &d2, &ov2);
    found_added = found_two = found_end = 0;
    for (int i = 0; i < l.n; i++) {
        if (wcscmp(l.c[i].title, L"My Section") == 0) found_added = 1;
        if (wcscmp(l.c[i].title, L"CHAPTER TWO") == 0) found_two = 1;
        if (wcscmp(l.c[i].title, L"The End") == 0) found_end = 1;
    }
    CHECK(found_added && !found_two && found_end && l.n == 6, "overrides still apply after unrelated edits (n=%d)", l.n);
    chapters_free(&l);

    /* editing the heading paragraph's own text detaches its override (documented behaviour) */
    wchar_t *t3 = (wchar_t *)malloc(4000 * sizeof(wchar_t));
    wcscpy(t3, BOOK);
    wchar_t *p = wcsstr(t3, L"Epilogue");
    wcsncpy(p, L"Epilogus ", 9);
    Document d3; doc_from(&d3, t3);
    chapters_detect(&l, &d3, &ov2);
    int still = 0;
    for (int i = 0; i < l.n; i++) if (wcscmp(l.c[i].title, L"The End") == 0) still = 1;
    CHECK(!still, "override keyed by text does not attach to a different paragraph");
    chapters_free(&l);

    /* identical heading text twice: the override targets the right occurrence */
    const wchar_t *dup = L"Intro text goes here for a while.\n\nNotes\n\nFirst notes body text.\n\nNotes\n\nSecond notes body text.\n";
    Document dd; doc_from(&dd, dup);
    Overrides ov3; memset(&ov3, 0, sizeof ov3);
    overrides_add_heading(&ov3, &dd, 3, L"Second Notes");
    chapters_detect(&l, &dd, &ov3);
    CHECK(l.n == 1 && l.c[0].para == 3 && wcscmp(l.c[0].title, L"Second Notes") == 0, "second occurrence targeted (n=%d)", l.n);
    chapters_free(&l);
    /* forgetting a manual heading removes the record entirely */
    overrides_remove_heading(&ov3, &dd, 3);
    CHECK(ov3.n == 0, "removing a manual heading deletes its record");
    overrides_free(&ov3);

    overrides_free(&ov); overrides_free(&ov2);
    doc_free(&d); doc_free(&d2); doc_free(&d3); doc_free(&dd); free(t2); free(t3);
}

static void test_filenames(void)
{
    wchar_t f[200];
    chapter_filename(f, 200, 3, L"Chapter 1: The Beginning?");
    CHECK(wcscmp(f, L"03 - Chapter 1_ The Beginning.wav") == 0, "\"%ls\"", f);
    chapter_filename(f, 200, 12, L"A/B\\C|D*E\"F<G>H");
    CHECK(wcsstr(f, L"/") == NULL && wcsstr(f, L"\\") == NULL && wcsstr(f, L"|") == NULL && wcsstr(f, L"*") == NULL &&
          wcsstr(f, L"\"") == NULL && wcsstr(f, L"<") == NULL && wcsstr(f, L">") == NULL && wcsstr(f, L"?") == NULL, "no invalid characters: \"%ls\"", f);
    chapter_filename(f, 200, 5, L"");
    CHECK(wcscmp(f, L"05 - Chapter 5.wav") == 0, "empty title: \"%ls\"", f);
    chapter_filename(f, 200, 6, L"  ...  ");
    CHECK(wcscmp(f, L"06 - Chapter 6.wav") == 0, "dots/spaces only: \"%ls\"", f);
    chapter_filename(f, 200, 7, L"CON");
    CHECK(wcscmp(f, L"07 - _CON.wav") == 0, "reserved device name: \"%ls\"", f);
    wchar_t longt[400];
    for (int i = 0; i < 399; i++) longt[i] = 'x';
    longt[399] = 0;
    chapter_filename(f, 200, 8, longt);
    CHECK(wcslen(f) < 100, "long title bounded (%zu)", wcslen(f));
    chapter_filename(f, 200, 9, L"Caf\u00e9 \u65e5\u672c");
    CHECK(wcsstr(f, L"Caf\u00e9 \u65e5\u672c") != NULL, "unicode kept");
    chapter_filename(f, 200, 10, L"Trailing dot.");
    CHECK(wcscmp(f, L"10 - Trailing dot.wav") == 0, "trailing dot trimmed: \"%ls\"", f);
}

/* -------------------------------------------------------------- bookmarks */

static void test_bookmarks(void)
{
    Document d; doc_from(&d, BOOK);
    ChapterList l;
    chapters_detect(&l, &d, NULL);
    BookmarkList bl; memset(&bl, 0, sizeof bl);
    int target = l.c[2].first_sent + 1;                      /* a sentence inside CHAPTER TWO */
    int i = bm_add(&bl, &d, &l, target, NULL);
    CHECK(i == 0 && bl.b[0].chapter == 2, "bookmark in chapter index 2 (got %d)", bl.b[0].chapter);
    CHECK(bm_resolve(&bl.b[0], &d, &l) == target, "resolves to the same sentence");
    CHECK(bm_find_at(&bl, &d, &l, target) == 0 && bm_find_at(&bl, &d, &l, 0) == -1, "find_at");
    int late = d.nsent - 1;
    bm_add(&bl, &d, &l, late, L"The end");
    bm_add(&bl, &d, &l, 1, L"Early");
    CHECK(bl.n == 3, "three bookmarks");
    CHECK(bm_next(&bl, &d, &l, 1, +1) == 0, "next after sentence 1 -> chapter-two bookmark");
    CHECK(bm_next(&bl, &d, &l, target, +1) == 1, "next after that -> the late one");
    CHECK(bm_next(&bl, &d, &l, late, +1) == 2, "next wraps to the first in the document");
    CHECK(bm_next(&bl, &d, &l, 1, -1) == 1, "previous from the start wraps to the last");
    CHECK(bm_next(&bl, &d, &l, target, -1) == 2, "previous");

    /* edits before the bookmark: a new first paragraph shifts every index */
    wchar_t *t2 = (wchar_t *)malloc(4000 * sizeof(wchar_t));
    swprintf(t2, 4000, L"A brand new opening paragraph. With two sentences.\n\n%ls", BOOK);
    Document d2; doc_from(&d2, t2);
    ChapterList l2;
    chapters_detect(&l2, &d2, NULL);
    int r = bm_resolve(&bl.b[0], &d2, &l2);
    wchar_t *want = doc_speech_text(&d, target), *got = r >= 0 ? doc_speech_text(&d2, r) : NULL;
    CHECK(r >= 0 && got && wcscmp(want, got) == 0, "bookmark follows its sentence after an earlier insertion");
    free(want); free(got);

    /* a paragraph inserted INSIDE the chapter before the bookmark: structure moves, text finds it */
    wchar_t *t3 = (wchar_t *)malloc(4000 * sizeof(wchar_t));
    wcscpy(t3, BOOK);
    wchar_t *at = wcsstr(t3, L"Another paragraph");
    const wchar_t *ins = L"Inserted paragraph before this one.\n\n";
    size_t il = wcslen(ins);
    memmove(at + il, at, (wcslen(at) + 1) * sizeof(wchar_t));
    wmemcpy(at, ins, il);
    Document d3; doc_from(&d3, t3);
    ChapterList l3;
    chapters_detect(&l3, &d3, NULL);
    r = bm_resolve(&bl.b[0], &d3, &l3);
    want = doc_speech_text(&d, target); got = r >= 0 ? doc_speech_text(&d3, r) : NULL;
    CHECK(r >= 0 && got && wcscmp(want, got) == 0, "bookmark found by text when the paragraph numbering shifted");
    free(want); free(got);

    /* deleted sentence -> unresolved, not a wrong place */
    const wchar_t *gone = L"Introduction\n\nCompletely different text now.\n";
    Document d4; doc_from(&d4, gone);
    ChapterList l4;
    chapters_detect(&l4, &d4, NULL);
    CHECK(bm_resolve(&bl.b[0], &d4, &l4) == -1, "missing sentence -> -1");

    /* identical sentences: occurrence number picks the right one */
    const wchar_t *same = L"Hello there friend. Something else. Hello there friend. More. Hello there friend.\n";
    Document ds; doc_from(&ds, same);
    BookmarkList b2; memset(&b2, 0, sizeof b2);
    bm_add(&b2, &ds, NULL, 2, NULL);
    CHECK(b2.b[0].occurrence == 1 && bm_resolve(&b2.b[0], &ds, NULL) == 2, "second identical sentence (occ %d)", b2.b[0].occurrence);
    bm_free(&b2);

    /* persistence incl. the resume anchor */
    Ini ini; ini_init(&ini);
    bm_to_ini(&bl, &ini);
    Bookmark pos;
    bm_make(&pos, &d, &l, target, L"position");
    bm_anchor_to_ini(&pos, &ini, L"Position");
    size_t len; char *bytes = ini_to_utf8(&ini, &len);
    Ini ini2; ini_init(&ini2);
    ini_from_utf8(&ini2, bytes, len);
    BookmarkList bl2; memset(&bl2, 0, sizeof bl2);
    bm_from_ini(&bl2, &ini2);
    CHECK(bl2.n == 3 && bm_resolve(&bl2.b[0], &d, &l) == target && wcscmp(bl2.b[1].label, L"The end") == 0, "bookmarks round trip");
    Bookmark pos2;
    CHECK(bm_anchor_from_ini(&pos2, &ini2, L"Position") && bm_resolve(&pos2, &d, &l) == target, "resume anchor round trip");
    /* saving fewer bookmarks removes the stale ones */
    bm_remove(&bl, 0);
    bm_to_ini(&bl, &ini2);
    BookmarkList bl3; memset(&bl3, 0, sizeof bl3);
    bm_from_ini(&bl3, &ini2);
    CHECK(bl3.n == 2, "stale bookmark sections dropped (%d)", bl3.n);
    bm_free(&bl3); bm_free(&bl2); bm_destroy(&pos); bm_destroy(&pos2);
    free(bytes); ini_free(&ini); ini_free(&ini2);

    bm_free(&bl);
    chapters_free(&l); chapters_free(&l2); chapters_free(&l3); chapters_free(&l4);
    doc_free(&d); doc_free(&d2); doc_free(&d3); doc_free(&d4); doc_free(&ds); free(t2); free(t3);
}

/* ------------------------------------------------ chapter-aware IR and assembly */

static void make_wav(const wchar_t *path, uint32_t rate, size_t nsamp)
{
    FILE *f = plat_fopen(path, "wb");
    wav_write_header(f, rate, 1, 16, (uint32_t)(nsamp * 2));
    for (size_t i = 0; i < nsamp; i++) { int16_t v = (int16_t)(5000 * sin(i * 0.05)); unsigned char b[2] = { (unsigned char)(v & 255), (unsigned char)((v >> 8) & 255) }; fwrite(b, 1, 2, f); }
    fclose(f);
}

static wchar_t g_dir[256];
static FILE *open_idx(void *ctx, int i) { (void)ctx; wchar_t p[300]; swprintf(p, 300, L"%ls/s%d.wav", g_dir, i); return plat_fopen(p, "rb"); }

static void test_ir_chapters(void)
{
    Document d; doc_from(&d, BOOK);
    ChapterList l;
    chapters_detect(&l, &d, NULL);
    Speech sp; memset(&sp, 0, sizeof sp); sp.dict = NULL; speech_default_options(&sp.opts);
    SynthParams p;
    memset(&p, 0, sizeof p);
    wcscpy(p.voice_id, L"v"); p.volume = 100; p.engine_id = L"sapi5"; p.wav_rate = 22050; p.chunk_sentences = 1; p.chunk_max_chars = 700;
    IR ir;
    CHECK(ir_build(&ir, &d, &sp, 0, d.nsent - 1, &p, &l), "ir");
    int ends = 0;
    for (int i = 0; i < ir.n; i++) ends += ir.seg[i].chapter_end;
    CHECK(ends == l.n + 0 || ends == l.n, "one chapter_end per chapter (%d vs %d)", ends, l.n);
    CHECK(ir.seg[0].chapter == chapter_of_sentence(&l, 0) && ir.seg[ir.n - 1].chapter == l.n - 1, "chapter ids");
    for (int i = 0; i + 1 < ir.n; i++)
        if (ir.seg[i].chapter_end) CHECK(ir.seg[i + 1].chapter == ir.seg[i].chapter + 1, "chapter ids advance at chapter ends");
    unsigned char kinds[64];
    ir_pause_kinds(&ir, kinds);
    int k2 = 0;
    for (int i = 0; i < ir.n - 1; i++) k2 += kinds[i] == 2;
    CHECK(k2 == l.n - 1, "chapter pause kind between chapters (%d)", k2);
    for (int i = 0; i < ir.n; i++) CHECK(!(ir.seg[i].chapter_end && !ir.seg[i].para_end), "a chapter end is also a paragraph end");

    /* assembly: three segments with kinds 0,2 */
    swprintf(g_dir, 256, L"/tmp/ttsforge_proj_test_%d", (int)getpid());
    plat_mkdir_p(g_dir);
    wchar_t pth[300];
    for (int i = 0; i < 3; i++) { swprintf(pth, 300, L"%ls/s%d.wav", g_dir, i); make_wav(pth, 22050, 1000); }
    unsigned char kk[3] = { 0, 2, 0 };
    AsmOptions o; memset(&o, 0, sizeof o);
    o.rate = 22050; o.sentence_ms = 100; o.paragraph_ms = 300; o.chapter_ms = 1000;
    swprintf(pth, 300, L"%ls/out.wav", g_dir);
    FILE *out = plat_fopen(pth, "w+b");
    AsmResult r;
    AsmStatus st = assemble_wav(out, 3, kk, NULL, open_idx, NULL, NULL, NULL, &o, &r);
    fclose(out);
    uint64_t want = 3000 + 22050ULL * 100 / 1000 + 22050ULL * 1000 / 1000;
    CHECK(st == ASM_OK && r.samples == want, "sentence + chapter pauses (%llu vs %llu)", (unsigned long long)r.samples, (unsigned long long)want);
    ir_free(&ir); chapters_free(&l); doc_free(&d);
    plat_remove_tree(g_dir);
}

int main(void)
{
    test_ini();
    test_detect();
    test_overrides();
    test_filenames();
    test_bookmarks();
    test_ir_chapters();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
