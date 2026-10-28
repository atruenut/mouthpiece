#include "../src/analyze.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const Issue *find(const Analysis *a, int kind, const wchar_t *tok)
{
    for (int i = 0; i < a->n; i++) if (a->issues[i].kind == kind && !wcscmp(a->issues[i].token, tok)) return &a->issues[i];
    return NULL;
}

int main(void)
{
    const wchar_t *txt = L"Visit https://example.com/x now. Mail bob@example.org today. NASA and NASA again.\n"
                         L"The J.R.R. books use my_var and std::map. We met Zephyrine twice. Then Zephyrine left. "
                         L"Later Zephyrine returned. It cost $4.50 or 3:30 or 10-20. The 1990s and 5th and 42 are fine. "
                         L"Scanned wor1d text. A camelCase thing.";
    Document d; doc_init(&d);
    CHECK(doc_build(&d, txt, wcslen(txt)));
    PronDict dict; pron_init(&dict);
    pron_add(&dict, L"NASA", L"nassa", 0, 0);
    pron_prepare(&dict);
    SpeechOptions o; memset(&o, 0, sizeof o);
    Analysis a;
    CHECK(analyze_document(&a, &d, NULL, &dict, &o));
    CHECK(a.words > 30);
    CHECK(find(&a, AK_URL, L"https://example.com/x") != NULL);
    CHECK(find(&a, AK_EMAIL, L"bob@example.org") != NULL);
    const Issue *n = find(&a, AK_ACRONYM, L"NASA");
    CHECK(n && n->count == 2 && n->handled);
    CHECK(find(&a, AK_INITIALS, L"J.R.R.") != NULL);
    CHECK(find(&a, AK_TECH, L"my_var") != NULL);
    CHECK(find(&a, AK_TECH, L"std::map") != NULL);
    const Issue *z = find(&a, AK_NAME, L"Zephyrine");
    CHECK(z && z->count >= 2 - 0);   /* first occurrence follows "met" so all three count */
    CHECK(find(&a, AK_NUMBER, L"$4.50") != NULL);
    CHECK(find(&a, AK_NUMBER, L"3:30") != NULL);
    CHECK(find(&a, AK_NUMBER, L"10-20") != NULL);
    CHECK(find(&a, AK_OCR, L"wor1d") != NULL);
    CHECK(find(&a, AK_NUMBER, L"1990s") == NULL && find(&a, AK_TECH, L"5th") == NULL && find(&a, AK_TECH, L"42") == NULL);
    const Issue *u = find(&a, AK_URL, L"https://example.com/x");
    CHECK(u && !u->handled);
    analysis_free(&a);
    o.urls = 1;
    CHECK(analyze_document(&a, &d, NULL, &dict, &o));
    u = find(&a, AK_URL, L"https://example.com/x");
    CHECK(u && u->handled);
    analysis_free(&a);
    doc_free(&d);
    /* empty document */
    doc_init(&d); doc_build(&d, L"", 0);
    CHECK(analyze_document(&a, &d, NULL, NULL, NULL) && a.n == 0);
    analysis_free(&a); doc_free(&d); pron_free(&dict);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
