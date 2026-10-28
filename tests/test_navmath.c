#include "../src/navmath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    PosHist h; poshist_clear(&h);
    poshist_jump(&h, 10);
    poshist_jump(&h, 50);                       /* at 90 now */
    CHECK(poshist_back(&h, 90) == 50);
    CHECK(poshist_back(&h, 50) == 10);
    CHECK(poshist_back(&h, 10) == -1);
    CHECK(poshist_forward(&h, 10) == 50);
    CHECK(poshist_forward(&h, 50) == 90);
    CHECK(poshist_forward(&h, 90) == -1);
    poshist_jump(&h, 5); CHECK(h.nf == 0);       /* a new jump clears forward */
    for (int i = 0; i < 200; i++) poshist_jump(&h, i);
    CHECK(h.nb <= POSHIST_MAX);

    GotoSpec g;
    CHECK(goto_parse(L"40%", &g) && g.kind == GT_PERCENT && g.value == 40);
    CHECK(goto_parse(L" c12 ", &g) && g.kind == GT_CHAPTER && g.value == 12);
    CHECK(goto_parse(L"P 300", &g) && g.kind == GT_PARAGRAPH);
    CHECK(goto_parse(L"s1200", &g) && g.kind == GT_SENTENCE && g.value == 1200);
    CHECK(goto_parse(L"m3", &g) && g.kind == GT_MARK);
    CHECK(goto_parse(L"+30s", &g) && g.kind == GT_SKIP && g.value == 30);
    CHECK(goto_parse(L"-2m", &g) && g.kind == GT_SKIP && g.value == -120);
    CHECK(!goto_parse(L"", &g) && !goto_parse(L"abc", &g) && !goto_parse(L"101%", &g) && !goto_parse(L"c0", &g) && !goto_parse(L"30s", &g) && !goto_parse(L"c3x", &g));

    wchar_t *txt = (wchar_t *)calloc(20000, sizeof(wchar_t));
    size_t n = 0;
    for (int i = 0; i < 300; i++) n += (size_t)swprintf(txt + n, 60, L"This is sentence number %d of the test. ", i);
    Document d; doc_init(&d);
    CHECK(doc_build(&d, txt, n));
    CHECK(d.nsent == 300);
    double cps = nav_chars_per_sec(0);
    CHECK(cps == 15.0 && nav_chars_per_sec(-10) < nav_chars_per_sec(0) && nav_chars_per_sec(10) > cps);
    double tot = nav_total_seconds(&d, cps);
    CHECK(tot > 300);
    CHECK(nav_at_fraction(&d, 0, cps) == 0 && nav_at_fraction(&d, 1, cps) == 299);
    int mid = nav_at_fraction(&d, 0.5, cps);
    CHECK(mid > 120 && mid < 180);
    int f = nav_skip_seconds(&d, 10, 30, cps), b = nav_skip_seconds(&d, 100, -30, cps);
    CHECK(f > 10 && f < 40 && b < 100 && b > 70);
    CHECK(nav_skip_seconds(&d, 299, 60, cps) == 299 && nav_skip_seconds(&d, 0, -60, cps) == 0);
    CHECK(nav_skip_seconds(&d, 5, 0.1, cps) == 6);
    doc_free(&d); free(txt);

    const wchar_t *c = L"one two three four five six seven eight nine ten";
    size_t st[16], ln[16];
    int k = caption_chunks(c, wcslen(c), 15, st, ln, 16);
    CHECK(k >= 3);
    for (int i = 0; i < k; i++) CHECK(ln[i] <= 15 && ln[i] > 0);
    size_t tot_len = 0; for (int i = 0; i < k; i++) tot_len += ln[i];
    CHECK(tot_len >= wcslen(c) - 10);
    k = caption_chunks(L"supercalifragilisticexpialidocious", 34, 10, st, ln, 16);
    CHECK(k == 1 && ln[0] == 34);
    CHECK(caption_chunks(L"   ", 3, 10, st, ln, 16) == 0);
    const wchar_t *r = L"The quick brown fox jumps over the lazy dog";
    size_t rn = wcslen(r);
    CHECK(text_resume_offset(r, rn, 0.0) == 0 && text_resume_offset(r, rn, 0.03) == 0 && text_resume_offset(r, rn, 0.99) == rn);
    size_t o = text_resume_offset(r, rn, 0.5);
    CHECK(o > 0 && o < rn && r[o - 1] == L' ' && o <= rn / 2);
    CHECK(text_resume_offset(L"", 0, 0.5) == 0);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
