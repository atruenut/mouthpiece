#include "../src/chapters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    const wchar_t *t = L"Chapter 1\n\nAlpha one. Beta two. Gamma three.\n\nChapter 2\n\nDelta four. Epsilon five.\n";
    Document d; doc_init(&d); CHECK(doc_build(&d, t, wcslen(t)));
    ChapterList chs; memset(&chs, 0, sizeof chs); CHECK(chapters_detect(&chs, &d, NULL));
    BookmarkList l; memset(&l, 0, sizeof l);
    int beta = -1, delta = -1;
    for (int i = 0; i < d.nsent; i++) { wchar_t *s = doc_speech_text(&d, i); if (!wcsncmp(s, L"Beta", 4)) beta = i; if (!wcsncmp(s, L"Delta", 5)) delta = i; free(s); }
    CHECK(beta > 0 && delta > beta);
    CHECK(bm_add_kind(&l, &d, &chs, beta, MK_QUESTION, L"why?\nreally") >= 0);
    CHECK(bm_add_kind(&l, &d, &chs, beta, MK_QUESTION, NULL) == -2);            /* same kind twice refused */
    CHECK(bm_add_kind(&l, &d, &chs, beta, MK_IMPORTANT, NULL) >= 0);             /* other kind fine */
    CHECK(bm_add_kind(&l, &d, &chs, delta, MK_BOOKMARK, NULL) >= 0);
    CHECK(bm_add_kind(&l, &d, &chs, delta, 99, NULL) == -1);
    CHECK(l.n == 3 && l.b[0].note && !wcschr(l.b[0].note, L'\n'));
    CHECK(bm_find_kind_at(&l, &d, &chs, beta, MK_IMPORTANT) == 1 && bm_find_kind_at(&l, &d, &chs, beta, MK_DOUBT) < 0);
    /* next with masks */
    unsigned q = 1u << MK_QUESTION;
    int i = bm_next_mask(&l, &d, &chs, 0, +1, q);
    CHECK(i == 0);
    i = bm_next_mask(&l, &d, &chs, beta, +1, q);       /* wraps back to itself: nothing else */
    CHECK(i == 0);
    i = bm_next_mask(&l, &d, &chs, beta, +1, ~0u);
    CHECK(i == 2);
    /* persistence */
    Ini ini; ini_init(&ini);
    bm_to_ini(&l, &ini);
    BookmarkList m; memset(&m, 0, sizeof m);
    bm_from_ini(&m, &ini);
    CHECK(m.n == 3 && m.b[0].kind == MK_QUESTION && m.b[0].note && !wcscmp(m.b[0].note, L"why? really") && m.b[2].kind == MK_BOOKMARK && !m.b[2].note);
    CHECK(bm_resolve(&m.b[0], &d, &chs) == beta);
    /* editing a note and removing */
    CHECK(bm_set_note(&m.b[0], L"changed") && !wcscmp(m.b[0].note, L"changed"));
    CHECK(bm_set_note(&m.b[0], L"") && m.b[0].note == NULL);
    bm_remove(&m, 0);
    CHECK(m.n == 2 && m.b[0].kind == MK_IMPORTANT);
    CHECK(!wcscmp(mark_kind_name(MK_UNCLEAR), L"Unclear") && mark_kind_glyph(MK_QUESTION)[0] == L'?');
    bm_free(&l); bm_free(&m); ini_free(&ini); chapters_free(&chs); doc_free(&d);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
