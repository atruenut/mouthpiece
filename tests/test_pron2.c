#include "../src/pronscope.h"
#include "../src/speech.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static wchar_t *render(const Speech *sp, const wchar_t *in)
{
    SpeechText t;
    if (!speech_render(sp, in, wcslen(in), 0, &t)) return NULL;
    return t.text;
}
static Speech mk(PronDict *d) { Speech s; memset(&s, 0, sizeof s); s.dict = d; memset(&s.opts, 0, sizeof s.opts); return s; }

int main(void)
{
    PronDict d;
    pron_init(&d);

    /* regex rules */
    CHECK(pron_add_ex(&d, L"(\\d+)x(\\d+)", L"$1 by $2", PRON_REGEX, NULL) >= 0);
    CHECK(pron_add_ex(&d, L"(", L"x", PRON_REGEX, NULL) == -2);            /* bad regex refused */
    CHECK(pron_add_ex(&d, L"Dr\\.?\\s+Who", L"Doctor Who", PRON_REGEX, NULL) >= 0);
    CHECK(pron_add(&d, L"NASA", L"nassa", 0, 0) >= 0);
    pron_prepare(&d);
    Speech sp = mk(&d);
    wchar_t *o = render(&sp, L"A 12x34 grid, Dr  Who and NASA.");
    CHECK(o && wcscmp(o, L"A 12 by 34 grid, Doctor Who and nassa.") == 0);
    free(o);
    /* plain entry beats a regex at the same position */
    pron_add(&d, L"12x34", L"plain", 0, 0);
    pron_prepare(&d);
    o = render(&sp, L"12x34"); CHECK(o && wcscmp(o, L"plain") == 0); free(o);
    /* same word may be both plain and regex */
    CHECK(pron_find_ex(&d, L"12x34", 0, NULL) >= 0 && pron_find_ex(&d, L"12x34", 1, NULL) < 0);

    /* persistence round trip incl. flags and scope */
    CHECK(pron_add_ex(&d, L"colour", L"color", PRON_CASE, L"chapter:Chapter Two") >= 0);
    size_t len;
    char *u = pron_to_utf8(&d, &len);
    PronDict d2; pron_init(&d2);
    int bad = 0;
    int n = pron_from_utf8(&d2, u, len, &bad);
    CHECK(n == d.n && bad == 0);
    int ri = pron_find_ex(&d2, L"(\\d+)x(\\d+)", 1, NULL);
    CHECK(ri >= 0 && d2.e[ri].regex);
    int si = pron_find_ex(&d2, L"colour", 0, L"chapter:Chapter Two");
    CHECK(si >= 0 && d2.e[si].match_case && d2.e[si].scope && wcscmp(d2.e[si].scope, L"chapter:Chapter Two") == 0);
    CHECK(pron_signature(&d) == pron_signature(&d2));
    free(u);
    /* an old file without scope column still loads */
    const char *old = "c\tFoo\tbar\n-\tBaz\tqux\n";
    CHECK(pron_from_utf8(&d2, old, strlen(old), &bad) == 2 && bad == 0 && d2.e[0].scope == NULL);
    pron_free(&d2);

    /* scopes against a real document */
    const wchar_t *text = L"Chapter One\n\nIt was the colour red. Another sentence here.\n\nChapter Two\n\nThe colour blue is here. Then end.\n";
    Document doc;
    CHECK(doc_build(&doc, text, wcslen(text)));
    ChapterList chs; memset(&chs, 0, sizeof chs);
    CHECK(chapters_detect(&chs, &doc, NULL));
    CHECK(chs.n == 2);
    pron_resolve_scopes(&d, &doc, &chs);
    pron_prepare(&d);
    /* find sentences */
    int s_red = -1, s_blue = -1;
    for (int i = 0; i < doc.nsent; i++) {
        wchar_t *t = doc_speech_text(&doc, i);
        if (wcsstr(t, L"red")) s_red = i;
        if (wcsstr(t, L"blue")) s_blue = i;
        free(t);
    }
    CHECK(s_red >= 0 && s_blue >= 0);
    Speech sp2 = mk(&d);
    wchar_t *a = speech_sentence_text(&sp2, &doc, s_red), *b = speech_sentence_text(&sp2, &doc, s_blue);
    CHECK(a && wcsstr(a, L"colour red"));                    /* chapter Two rule does not touch chapter One */
    CHECK(b && wcsstr(b, L"color blue"));                    /* ...but applies in chapter Two */
    free(a); free(b);

    /* range scope: only the sentence 'Then end.' */
    int s_end = -1;
    for (int i = 0; i < doc.nsent; i++) { wchar_t *t = doc_speech_text(&doc, i); if (wcsstr(t, L"Then end")) s_end = i; free(t); }
    wchar_t *rs = pronscope_range(&doc, s_end, s_end);
    CHECK(rs != NULL);
    pron_add_ex(&d, L"end", L"finish", 0, rs);
    pron_resolve_scopes(&d, &doc, &chs);
    pron_prepare(&d);
    a = speech_sentence_text(&sp2, &doc, s_end);
    CHECK(a && wcsstr(a, L"finish"));
    free(a);
    a = speech_sentence_text(&sp2, &doc, s_blue);
    CHECK(a && !wcsstr(a, L"finish"));
    free(a);
    /* scoped entries do not apply outside a document */
    o = render(&sp2, L"colour end");
    CHECK(o && wcscmp(o, L"colour end") == 0);
    free(o);
    /* editing the anchor text disables the range entry */
    const wchar_t *text2 = L"Chapter One\n\nIt was the colour red. Another sentence here.\n\nChapter Two\n\nThe colour blue is here. And so it ended.\n";
    Document doc2;
    CHECK(doc_build(&doc2, text2, wcslen(text2)));
    ChapterList chs2; memset(&chs2, 0, sizeof chs2);
    chapters_detect(&chs2, &doc2, NULL);
    pron_resolve_scopes(&d, &doc2, &chs2);
    int ei = pron_find_ex(&d, L"end", 0, rs);
    CHECK(ei >= 0 && d.e[ei].s_first > d.e[ei].s_last);
    wchar_t desc[200];
    pronscope_describe(&d.e[ei], desc, 200);
    CHECK(wcsstr(desc, L"not applied") != NULL);
    /* a missing chapter disables the chapter entry */
    wchar_t *cs = pronscope_chapter(L"Chapter Nine");
    pron_add_ex(&d, L"zz", L"yy", 0, cs);
    pron_resolve_scopes(&d, &doc, &chs);
    int zi = pron_find_ex(&d, L"zz", 0, cs);
    CHECK(zi >= 0 && d.e[zi].s_first > d.e[zi].s_last);
    free(cs); free(rs);

    chapters_free(&chs); chapters_free(&chs2);
    doc_free(&doc); doc_free(&doc2);
    pron_free(&d);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
