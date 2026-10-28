#include "../src/find.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    const wchar_t *t = L"The cat sat. Concatenate the CAT; cats!";
    size_t s, l;
    wchar_t err[100];
    FindOpts o = { 0, 0, 0 };
    CHECK(find_next(t, wcslen(t), L"cat", &o, 0, 1, &s, &l, err, 100) == 1 && s == 4 && l == 3);
    CHECK(find_next(t, wcslen(t), L"cat", &o, 5, 1, &s, &l, err, 100) == 1 && s == 16);
    CHECK(find_next(t, wcslen(t), L"cat", &o, wcslen(t), -1, &s, &l, err, 100) == 1 && s == 34);
    CHECK(find_next(t, wcslen(t), L"cat", &o, 34, -1, &s, &l, err, 100) == 1 && s == 29);
    o.match_case = 1;
    CHECK(find_next(t, wcslen(t), L"CAT", &o, 0, 1, &s, &l, err, 100) == 1 && s == 29);
    o.match_case = 0; o.whole_word = 1;
    CHECK(find_next(t, wcslen(t), L"cat", &o, 5, 1, &s, &l, err, 100) == 1 && s == 29);
    CHECK(find_next(t, wcslen(t), L"zebra", &o, 0, 1, &s, &l, err, 100) == 0);
    o.whole_word = 0; o.regex = 1;
    CHECK(find_next(t, wcslen(t), L"c[a-z]+", &o, 0, 1, &s, &l, err, 100) == 1 && s == 4 && l == 3);
    CHECK(find_next(t, wcslen(t), L"(", &o, 0, 1, &s, &l, err, 100) == -1 && err[0]);
    o.whole_word = 1;
    CHECK(find_next(t, wcslen(t), L"cat", &o, 5, 1, &s, &l, err, 100) == 1 && s == 29);

    size_t cnt;
    FindOpts p = { 0, 0, 0 };
    wchar_t *r = find_replace_all(t, wcslen(t), L"cat", L"dog", &p, &cnt, err, 100);
    CHECK(r && cnt == 4 && wcscmp(r, L"The dog sat. Condogenate the dog; dogs!") == 0);
    free(r);
    p.whole_word = 1;
    r = find_replace_all(t, wcslen(t), L"cat", L"dog", &p, &cnt, err, 100);
    CHECK(r && cnt == 2 && wcscmp(r, L"The dog sat. Concatenate the dog; cats!") == 0);
    free(r);
    FindOpts x = { 0, 0, 1 };
    r = find_replace_all(L"a1 b22 c333", 11, L"([a-z])(\\d+)", L"$2$1", &x, &cnt, err, 100);
    CHECK(r && cnt == 3 && wcscmp(r, L"1a 22b 333c") == 0);
    free(r);
    r = find_replace_all(L"abc", 3, L"x*", L"-", &x, &cnt, err, 100);          /* empty matches skipped */
    CHECK(r && cnt == 0 && wcscmp(r, L"abc") == 0);
    free(r);
    r = find_replace_all(L"abc", 3, L"(", L"-", &x, &cnt, err, 100);
    CHECK(r == NULL && err[0]);
    wchar_t *rp = find_replacement_for(L"a1", 2, L"([a-z])(\\d)", L"$2$1", &x, 0, 2);
    CHECK(rp && wcscmp(rp, L"1a") == 0);
    free(rp);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
