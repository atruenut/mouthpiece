#include "../src/hotkeys.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    HotkeyCfg c; hotkeys_default(&c);
    CHECK(c.enabled && c.leader_vk == 0x20 && c.leader_mod == (HK_MOD_CONTROL | HK_MOD_ALT) && c.media_keys);
    CHECK(c.direct[ACT_PLAYPAUSE].vk == 'P' && c.direct[ACT_PLAYPAUSE].mod == 3 && c.direct[ACT_PREV_MARK].mod == 7);
    for (int i = 1; i < ACT_COUNT; i++) { CHECK(c.direct[i].vk != 0); for (int j = i + 1; j < ACT_COUNT; j++) CHECK(!(c.direct[i].vk == c.direct[j].vk && c.direct[i].mod == c.direct[j].mod)); }
    c.media_keys = 0; c.arm_seconds = 8;
    c.direct[ACT_PLAYPAUSE].vk = 0; c.direct[ACT_PLAYPAUSE].mod = 0;
    c.direct[ACT_MARK_QUESTION].mod = HK_MOD_CONTROL | HK_MOD_ALT | HK_MOD_SHIFT; c.direct[ACT_MARK_QUESTION].vk = 'Q';
    Ini d; ini_init(&d); hotkeys_to_ini(&c, &d);
    HotkeyCfg e; hotkeys_from_ini(&e, &d);
    CHECK(!e.media_keys && e.arm_seconds == 8 && e.direct[ACT_MARK_QUESTION].vk == 'Q' && e.direct[ACT_MARK_QUESTION].mod == 7 && !e.direct[ACT_PLAYPAUSE].vk);
    /* a removed default stays removed; settings from before the defaults existed get them */
    CHECK(e.direct[ACT_STOP].vk == 'S' && !e.direct[ACT_PLAYPAUSE].vk);
    Ini o; ini_init(&o);
    ini_set_int(&o, L"Hotkeys", L"MediaKeys", 0);
    ini_set_int(&o, L"HotkeysDirect", L"PlayPause.Mod", 0); ini_set_int(&o, L"HotkeysDirect", L"PlayPause.Vk", 0);
    ini_set_int(&o, L"HotkeysDirect", L"Stop.Mod", 6); ini_set_int(&o, L"HotkeysDirect", L"Stop.Vk", 'Z');
    HotkeyCfg old; hotkeys_from_ini(&old, &o);
    CHECK(old.media_keys && old.direct[ACT_PLAYPAUSE].vk == 'P' && old.direct[ACT_STOP].vk == 'Z' && old.direct[ACT_STOP].mod == 6);
    ini_free(&o);
    /* a file poisoned by "everything zero" saves (version 2 with empty values) is repaired; a version 3 file is trusted */
    Ini z; ini_init(&z);
    ini_set_int(&z, L"Hotkeys", L"Enabled", 0); ini_set_int(&z, L"Hotkeys", L"LeaderMod", 0); ini_set_int(&z, L"Hotkeys", L"LeaderVk", 0);
    ini_set_int(&z, L"Hotkeys", L"ArmSeconds", 0); ini_set_int(&z, L"Hotkeys", L"MediaKeys", 0); ini_set_int(&z, L"Hotkeys", L"DefaultsVersion", 2);
    ini_set_int(&z, L"HotkeysDirect", L"PlayPause.Mod", 0); ini_set_int(&z, L"HotkeysDirect", L"PlayPause.Vk", 0);
    HotkeyCfg zc; hotkeys_from_ini(&zc, &z);
    CHECK(zc.enabled && zc.media_keys && zc.leader_vk == 0x20 && zc.arm_seconds == 5 && zc.direct[ACT_PLAYPAUSE].vk == 'P');
    ini_set_int(&z, L"Hotkeys", L"DefaultsVersion", 3); ini_set_int(&z, L"Hotkeys", L"ArmSeconds", 6); ini_set_int(&z, L"Hotkeys", L"LeaderMod", 3); ini_set_int(&z, L"Hotkeys", L"LeaderVk", 'M');
    hotkeys_from_ini(&zc, &z);
    CHECK(!zc.enabled && !zc.media_keys && zc.leader_vk == 'M' && !zc.direct[ACT_PLAYPAUSE].vk);
    ini_free(&z);
    HotkeyCfg rd = zc; rd.enabled = 0; hotkeys_restore_defaults(&rd);
    CHECK(!rd.enabled && rd.media_keys && rd.leader_vk == 0x20 && rd.direct[ACT_PLAYPAUSE].vk == 'P');
    CHECK(leader_vk_of(ACT_MARK_QUESTION) == 'Q' && leader_vk_of(ACT_SHOW_WINDOW) == 'W' && leader_vk_of(ACT_REPLAY) == 0);
    ini_set_int(&d, L"Hotkeys", L"LeaderMod", 0);           /* a bare leader is refused */
    ini_set_int(&d, L"Hotkeys", L"ArmSeconds", 99);
    hotkeys_from_ini(&e, &d);
    CHECK(e.leader_vk == 0 && e.arm_seconds == 15);
    ini_free(&d);
    for (int i = 1; i < ACT_COUNT; i++) CHECK(action_from_key(action_key(i)) == i && action_name(i)[0]);
    CHECK(action_from_key(L"nope") == ACT_NONE);
    CHECK(leader_lookup(0x20) == ACT_PLAYPAUSE && leader_lookup('Q') == ACT_MARK_QUESTION && leader_lookup('Z') == ACT_NONE);
    CHECK(leader_stays_armed(0x27) && !leader_stays_armed('Q'));
    int n; const LeaderKey *k = leader_keys(&n);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) CHECK(k[i].vk != k[j].vk);
    wchar_t t[64]; hotkey_text(3, 0x20, t, 64); CHECK(!wcscmp(t, L"Ctrl+Alt+Space"));
    hotkey_text(7, 'Q', t, 64); CHECK(!wcscmp(t, L"Ctrl+Alt+Shift+Q"));
    wchar_t h[512]; leader_help(h, 512); CHECK(wcslen(h) > 50);
    wchar_t big[4000];
    hotkeys_default(&c); c.direct[ACT_MARK_QUESTION].mod = 7; c.direct[ACT_MARK_QUESTION].vk = 'Q';
    hotkeys_help_text(&c, L"Ctrl+Alt+Q is used by another program.", big, 4000);
    CHECK(wcsstr(big, L"Ctrl+Alt+Space") && wcsstr(big, L"Play / pause") && wcsstr(big, L"Ctrl+Alt+Shift+Q") && wcsstr(big, L"PROBLEMS") && wcsstr(big, L"MEDIA KEYS"));
    CHECK(wcsstr(big, L"Flag: question") && wcsstr(big, L"Esc"));
    CHECK(wcsstr(big, L"AFTER THE LEADER KEY") && wcsstr(big, L"Ctrl+Alt+P") && wcsstr(big, L"Ctrl+Alt+Shift+M"));
    c.enabled = 0; hotkeys_help_text(&c, NULL, big, 4000); CHECK(wcsstr(big, L"OFF"));
    hotkeys_help_text(&c, NULL, big, 50); CHECK(wcslen(big) < 50);
    wchar_t kn[24]; vk_name(0xDB, kn, 24); CHECK(!wcscmp(kn, L"["));
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
