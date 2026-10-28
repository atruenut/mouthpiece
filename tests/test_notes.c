#include "../src/preproc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static wchar_t *run(const wchar_t *in, PreprocOptions *o, PreprocResult *r)
{
    if (!preproc_run(in, wcslen(in), o, 1, r)) return NULL;
    return r->text;
}
int main(void)
{
    PreprocOptions o;
    PreprocResult r;
    preproc_default_options(&o);

    /* footnotes */
    const wchar_t *fn = L"The result was clear¹ and the claim stands.[^2]\n\n¹ See Smith, Journal of Things, 1999.\n[^2]: Another note about it.\n\nNext paragraph.\n";
    o.remove_footnotes = 0;
    wchar_t *t = run(fn, &o, &r);
    CHECK(t && wcsstr(t, L"¹") && wcsstr(t, L"See Smith"));
    preproc_free(&r);
    o.remove_footnotes = 1;
    t = run(fn, &o, &r);
    CHECK(t && !wcsstr(t, L"¹") && !wcsstr(t, L"[^") && !wcsstr(t, L"See Smith") && !wcsstr(t, L"Another note"));
    CHECK(wcsstr(t, L"The result was clear and the claim stands.") != NULL);
    CHECK(wcsstr(t, L"Next paragraph.") != NULL);
    CHECK(r.n_footnotes >= 4);
    preproc_free(&r);

    /* tables: remove */
    const wchar_t *tb = L"Intro sentence here.\n\nName\tAge\tCity\nAnn\t31\tHalifax\nBob\t42\tMoncton\n\nAfter the table.\n";
    o.remove_footnotes = 0;
    t = run(tb, &o, &r);
    CHECK(t && wcsstr(t, L"Ann"));
    preproc_free(&r);
    o.remove_tables = 1;
    t = run(tb, &o, &r);
    CHECK(t && !wcsstr(t, L"Ann") && !wcsstr(t, L"Halifax") && wcsstr(t, L"After the table.") && r.n_tables == 3);
    preproc_free(&r);

    /* tables: row by row */
    o.remove_tables = 0; o.table_rows = 1;
    t = run(tb, &o, &r);
    CHECK(t && wcsstr(t, L"Name, Age, City.") && wcsstr(t, L"Ann, 31, Halifax.") && wcsstr(t, L"Bob, 42, Moncton."));
    CHECK(wcsstr(t, L"After the table.") != NULL);
    preproc_free(&r);

    /* markdown pipe table with rule row */
    const wchar_t *md = L"Before.\n\n| Item | Price |\n|------|-------|\n| Tea | 3 |\n| Cake | 5 |\n\nAfter.\n";
    t = run(md, &o, &r);
    CHECK(t && wcsstr(t, L"Item, Price.") && wcsstr(t, L"Tea, 3.") && !wcsstr(t, L"---") && !wcsstr(t, L"|"));
    preproc_free(&r);
    o.table_rows = 0; o.remove_tables = 1;
    t = run(md, &o, &r);
    CHECK(t && !wcsstr(t, L"Tea") && wcsstr(t, L"After."));
    preproc_free(&r);

    /* wide-space columns */
    const wchar_t *ws = L"Plain text first.\n\nYear   Sales   Profit\n2001   100     20\n2002   120     25\n\nEnd.\n";
    o.remove_tables = 0; o.table_rows = 1;
    t = run(ws, &o, &r);
    CHECK(t && wcsstr(t, L"Year, Sales, Profit.") && wcsstr(t, L"2002, 120, 25."));
    preproc_free(&r);

    /* ordinary prose is not a table */
    const wchar_t *pr = L"This is a sentence | with a pipe in it.\nAnd another normal line of prose here.\n";
    t = run(pr, &o, &r);
    CHECK(t && wcsstr(t, L"with a pipe in it.") && !wcsstr(t, L", with"));
    preproc_free(&r);

    /* mapping is still consistent: src offsets of kept text */
    o.table_rows = 0; o.remove_tables = 1; o.remove_footnotes = 1;
    t = run(tb, &o, &r);
    size_t pos = (size_t)(wcsstr(t, L"After") - t);
    size_t src = preproc_norm_to_src(&r, pos);
    CHECK(wcsncmp(tb + src, L"After", 5) == 0);
    preproc_free(&r);

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
