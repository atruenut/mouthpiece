#include "../src/config.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    ProfileList l; memset(&l, 0, sizeof l);
    CHECK(profiles_set(&l, L"Narrator", L"HKLM\\Voices\\Anna", -2, 90));
    CHECK(profiles_set(&l, L"Fast", L"HKLM\\Voices\\Bob", 5, 100));
    CHECK(profiles_set(&l, L"narrator", L"HKLM\\Voices\\Carl", 1, 80));          /* same name, other case: replaces */
    CHECK(l.n == 2);
    CHECK(!profiles_set(&l, L"", L"x", 0, 100));
    CHECK(!profiles_set(&l, L"a=b", L"x", 0, 100));
    CHECK(!profiles_set(&l, L"a\nb", L"x", 0, 100));
    Ini ini; ini_init(&ini);
    profiles_to_ini(&l, &ini);
    ProfileList m; memset(&m, 0, sizeof m);
    profiles_from_ini(&m, &ini);
    CHECK(m.n == 2);
    int i = profiles_find(&m, L"NARRATOR");
    CHECK(i >= 0 && m.p[i].rate == 1 && m.p[i].volume == 80 && !wcscmp(m.p[i].voice_id, L"HKLM\\Voices\\Carl"));
    CHECK(profiles_find(&m, L"nobody") < 0);
    profiles_remove(&m, profiles_find(&m, L"Fast"));
    CHECK(m.n == 1);
    profiles_free(&l); profiles_free(&m); ini_free(&ini);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
