#include "../src/rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* returns matched substring of first search hit or NULL text if none */
static int find(const wchar_t *pat, int ic, const wchar_t *text, wchar_t *out, size_t *start)
{
    wchar_t err[200];
    Rx *r = rx_compile(pat, ic, err, 200);
    if (!r) return -1;
    size_t caps[40];
    int ok = rx_search(r, text, wcslen(text), 0, 1, caps);
    if (ok) { size_t l = caps[1] - caps[0]; wcsncpy(out, text + caps[0], l); out[l] = 0; if (start) *start = caps[0]; }
    rx_free(r);
    return ok;
}
static int is(const wchar_t *pat, const wchar_t *text, const wchar_t *want)
{
    wchar_t o[200];
    int r = find(pat, 0, text, o, NULL);
    return r == 1 && wcscmp(o, want) == 0;
}
static int no(const wchar_t *pat, const wchar_t *text) { wchar_t o[200]; return find(pat, 0, text, o, NULL) == 0; }

int main(void)
{
    CHECK(is(L"abc", L"xxabcxx", L"abc"));
    CHECK(no(L"abc", L"ab"));
    CHECK(is(L"a.c", L"abc", L"abc"));
    CHECK(no(L"a.c", L"a\nc"));
    CHECK(is(L"ab*c", L"abbbc", L"abbbc"));
    CHECK(is(L"ab*c", L"ac", L"ac"));
    CHECK(is(L"ab+c", L"abbc", L"abbc"));
    CHECK(no(L"ab+c", L"ac"));
    CHECK(is(L"colou?r", L"color", L"color"));
    CHECK(is(L"colou?r", L"colour", L"colour"));
    CHECK(is(L"\\d+", L"abc 123 def", L"123"));
    CHECK(is(L"\\d{2,3}", L"12345", L"123"));
    CHECK(is(L"\\d{2,3}?", L"12345", L"12"));
    CHECK(is(L"\\d{4}", L"in 1999 ok", L"1999"));
    CHECK(no(L"^\\d{4}$", L"12345"));
    CHECK(is(L"^\\d{4}$", L"1234", L"1234"));
    CHECK(is(L"[a-c]+", L"xxabcabcxx", L"abcabc"));
    CHECK(is(L"[^a-c]+", L"abcxyzabc", L"xyz"));
    CHECK(is(L"[\\d.]+", L"v3.14!", L"3.14"));
    CHECK(is(L"cat|dog", L"hotdog", L"dog"));
    CHECK(is(L"(cat|dog)s", L"dogs", L"dogs"));
    CHECK(is(L"\\bcat\\b", L"a cat sat", L"cat"));
    CHECK(no(L"\\bcat\\b", L"concatenate"));
    CHECK(is(L"\\Bcat", L"concat", L"cat"));
    CHECK(is(L"a.*b", L"aXbYb", L"aXbYb"));
    CHECK(is(L"a.*?b", L"aXbYb", L"aXb"));
    CHECK(is(L"(?:ab)+", L"ababab", L"ababab"));
    CHECK(is(L"(ab){2}", L"ababab", L"abab"));
    CHECK(is(L"\\$\\d+", L"cost $45", L"$45"));
    CHECK(is(L"a{", L"a{", L"a{"));                      /* literal brace */
    CHECK(is(L"\\.", L"a.b", L"."));
    CHECK(is(L"x*", L"aaa", L""));                       /* empty match allowed */

    wchar_t err[200];
    CHECK(rx_compile(L"(abc", 0, err, 200) == NULL && err[0]);
    CHECK(rx_compile(L"abc)", 0, err, 200) == NULL);
    CHECK(rx_compile(L"[abc", 0, err, 200) == NULL);
    CHECK(rx_compile(L"*a", 0, err, 200) == NULL);
    CHECK(rx_compile(L"a\\", 0, err, 200) == NULL);
    CHECK(rx_compile(L"a{3,2}", 0, err, 200) == NULL);

    /* case-insensitive */
    wchar_t o[100];
    CHECK(find(L"hello", 1, L"Say HeLLo", o, NULL) == 1 && wcscmp(o, L"HeLLo") == 0);
    CHECK(find(L"[a-z]+", 1, L"ABC", o, NULL) == 1 && wcscmp(o, L"ABC") == 0);

    /* captures and expansion */
    Rx *r = rx_compile(L"(\\d+)-(\\d+)", 0, err, 200);
    size_t caps[6];
    const wchar_t *t = L"pages 12-34 end";
    CHECK(r && rx_groups(r) == 2);
    CHECK(rx_search(r, t, wcslen(t), 0, 1, caps));
    CHECK(caps[0] == 6 && caps[1] == 11 && caps[2] == 6 && caps[3] == 8 && caps[4] == 9 && caps[5] == 11);
    size_t l = rx_expand(L"$1 to $2 ($$ $0)", t, caps, 2, NULL);
    wchar_t *buf = (wchar_t *)malloc((l + 1) * sizeof(wchar_t));
    rx_expand(L"$1 to $2 ($$ $0)", t, caps, 2, buf);
    CHECK(wcscmp(buf, L"12 to 34 ($ 12-34)") == 0);
    free(buf);
    CHECK(rx_search(r, t, wcslen(t), 20, -1, caps) == 1 && caps[0] == 7);     /* backward: the last start that matches */
    rx_free(r);

    /* optional group not taking part */
    r = rx_compile(L"a(b)?c", 0, err, 200);
    size_t c2[4];
    CHECK(rx_search(r, L"ac", 2, 0, 1, c2) && c2[2] == (size_t)-1);
    rx_free(r);

    /* catastrophic pattern ends (step limit) rather than hanging */
    r = rx_compile(L"(a*)*b", 0, err, 200);
    wchar_t as[60];
    for (int i = 0; i < 59; i++) as[i] = 'a';
    as[59] = 0;
    CHECK(r != NULL);
    size_t c3[4];
    CHECK(rx_search(r, as, 59, 0, 1, c3) == 0);
    rx_free(r);

    /* unicode */
    CHECK(is(L"café", L"un café", L"café"));
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
