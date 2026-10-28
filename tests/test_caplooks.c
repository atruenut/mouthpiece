#include "../src/caplooks.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static CaptionLook mk(const wchar_t *name, int pt, unsigned col)
{
    CaptionLook l; memset(&l, 0, sizeof l);
    wcscpy(l.name, name); wcscpy(l.face, L"Segoe UI");
    l.font_pt = pt; l.bold = 1; l.text_color = col; l.bg_color = 0x102030; l.text_opacity = 90; l.bg_opacity = 40; l.outline = 1; l.width_pct = 60;
    return l;
}
int main(void)
{
    wchar_t o[LOOK_NAME];
    CHECK(looks_clean_name(L"  Night reading ", o, LOOK_NAME) && !wcscmp(o, L"Night reading"));
    CHECK(!looks_clean_name(L"", o, LOOK_NAME) && !looks_clean_name(L"   ", o, LOOK_NAME) && !looks_clean_name(L"a=b", o, LOOK_NAME) && !looks_clean_name(L"a\nb", o, LOOK_NAME));
    wchar_t longn[80]; for (int i = 0; i < 79; i++) longn[i] = 'x'; longn[79] = 0;
    CHECK(!looks_clean_name(longn, o, LOOK_NAME));
    CaptionLooks s; memset(&s, 0, sizeof s);
    CaptionLook a = mk(L"Night", 30, 0xFFFFFF), b = mk(L"Day", 20, 0x000000);
    CHECK(looks_put(&s, &a) == 0 && looks_put(&s, &b) == 1 && s.n == 2);
    CaptionLook a2 = mk(L"night", 44, 0xABCDEF);                      /* same name, other case: replaces */
    CHECK(looks_put(&s, &a2) == 0 && s.n == 2 && s.l[0].font_pt == 44 && looks_find(&s, L"NIGHT") == 0);
    CHECK(looks_same(&s.l[0], &a2) && !looks_same(&s.l[0], &a) && looks_same(&a, &a));
    CaptionLook bad = mk(L"", 20, 0); CHECK(looks_put(&s, &bad) == -1 && s.n == 2);
    Ini d; ini_init(&d);
    looks_to_ini(&s, &d);
    CaptionLooks r; looks_from_ini(&r, &d);
    CHECK(r.n == 2 && !wcscmp(r.l[0].name, L"night") && looks_same(&r.l[0], &s.l[0]) && looks_same(&r.l[1], &s.l[1]) && r.l[1].bg_color == 0x102030);
    looks_remove(&s, 0);
    CHECK(s.n == 1 && !wcscmp(s.l[0].name, L"Day") && looks_find(&s, L"night") < 0);
    looks_to_ini(&s, &d);                                              /* writing again must not leave the deleted one behind */
    looks_from_ini(&r, &d);
    CHECK(r.n == 1 && !wcscmp(r.l[0].name, L"Day"));
    looks_remove(&s, 5); looks_remove(&s, -1); CHECK(s.n == 1);
    for (int i = 0; i < LOOK_MAX + 3; i++) { CaptionLook x = mk(L"x", 20, (unsigned)i); swprintf(x.name, LOOK_NAME, L"look %d", i); looks_put(&s, &x); }
    CHECK(s.n == LOOK_MAX);
    ini_free(&d);
    ini_init(&d); ini_set_int(&d, L"CaptionLooks", L"Count", 999); looks_from_ini(&r, &d); CHECK(r.n == 0);   /* missing entries are skipped */
    ini_free(&d);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
