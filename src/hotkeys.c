#include "hotkeys.h"
#include <stdio.h>
#include <string.h>

static const struct { const wchar_t *key, *name; } A[ACT_COUNT] = {
    { L"None", L"" }, { L"PlayPause", L"Play / pause" }, { L"Stop", L"Stop" },
    { L"PrevSentence", L"Previous sentence" }, { L"NextSentence", L"Next sentence" },
    { L"PrevParagraph", L"Previous paragraph" }, { L"NextParagraph", L"Next paragraph" },
    { L"PrevChapter", L"Previous chapter" }, { L"NextChapter", L"Next chapter" },
    { L"Replay", L"Replay this sentence" }, { L"JumpBack", L"Jump back to where I was" }, { L"JumpForward", L"Jump forward again" },
    { L"SkipBack", L"Skip back 30 seconds" }, { L"SkipForward", L"Skip forward 30 seconds" },
    { L"Bookmark", L"Bookmark this passage" }, { L"Revisit", L"Flag: revisit" }, { L"Important", L"Flag: important" },
    { L"Unclear", L"Flag: unclear" }, { L"Question", L"Flag: question" }, { L"Doubt", L"Flag: doubt" },
    { L"NextMark", L"Next mark" }, { L"PrevMark", L"Previous mark" },
    { L"Faster", L"Faster" }, { L"Slower", L"Slower" }, { L"Captions", L"Show / hide captions" }, { L"ShowWindow", L"Show the Mouthpiece window" },
};

const wchar_t *action_name(int a) { return (a > 0 && a < ACT_COUNT) ? A[a].name : L""; }
const wchar_t *action_key(int a) { return (a > 0 && a < ACT_COUNT) ? A[a].key : L"None"; }
int action_from_key(const wchar_t *k)
{
    for (int i = 1; i < ACT_COUNT; i++) if (!wcscmp(A[i].key, k)) return i;
    return ACT_NONE;
}

#define VK_SPACE_ 0x20
static const LeaderKey LK[] = {
    { 0x20, ACT_PLAYPAUSE, 0, L"Space play/pause" },
    { 0x25, ACT_PREV_SENT, 1, L"\x2190 sentence" },       /* VK_LEFT */
    { 0x27, ACT_NEXT_SENT, 1, L"\x2192" },                /* VK_RIGHT */
    { 0x26, ACT_PREV_PARA, 1, L"\x2191 paragraph" },      /* VK_UP */
    { 0x28, ACT_NEXT_PARA, 1, L"\x2193" },                /* VK_DOWN */
    { 0xDB, ACT_PREV_CH, 1, L"[ chapter" },               /* VK_OEM_4 */
    { 0xDD, ACT_NEXT_CH, 1, L"]" },                       /* VK_OEM_6 */
    { 'J', ACT_JUMP_BACK, 0, L"J back" },
    { 'K', ACT_JUMP_FWD, 0, L"K forward" },
    { 'A', ACT_SKIP_BACK, 1, L"A/D \xB1" "30s" },
    { 'D', ACT_SKIP_FWD, 1, L"" },
    { 'B', ACT_MARK_BOOKMARK, 0, L"B bookmark" },
    { 'R', ACT_MARK_REVISIT, 0, L"R revisit" },
    { 'I', ACT_MARK_IMPORTANT, 0, L"I important" },
    { 'U', ACT_MARK_UNCLEAR, 0, L"U unclear" },
    { 'Q', ACT_MARK_QUESTION, 0, L"Q question" },
    { 'X', ACT_MARK_DOUBT, 0, L"X doubt" },
    { 'N', ACT_NEXT_MARK, 1, L"N/P marks" },
    { 'P', ACT_PREV_MARK, 1, L"" },
    { 0xBB, ACT_FASTER, 1, L"+/- speed" },                /* VK_OEM_PLUS */
    { 0xBD, ACT_SLOWER, 1, L"" },                         /* VK_OEM_MINUS */
    { 'C', ACT_CAPTIONS, 0, L"C captions" },
    { 'W', ACT_SHOW_WINDOW, 0, L"W window" },
    { 'S', ACT_STOP, 0, L"S stop" },
};
const LeaderKey *leader_keys(int *n) { *n = (int)(sizeof LK / sizeof LK[0]); return LK; }
int leader_lookup(int vk) { for (size_t i = 0; i < sizeof LK / sizeof LK[0]; i++) if (LK[i].vk == vk) return LK[i].action; return ACT_NONE; }
int leader_stays_armed(int vk) { for (size_t i = 0; i < sizeof LK / sizeof LK[0]; i++) if (LK[i].vk == vk) return LK[i].stays_armed; return 0; }

void leader_help(wchar_t *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof LK / sizeof LK[0]; i++) {
        if (!LK[i].label[0]) continue;
        int w = swprintf(out + n, cap - n, L"%ls%ls", n ? L"  \x2022  " : L"", LK[i].label);
        if (w < 0) break;
        n += (size_t)w;
    }
    wcsncat(out, L"  \x2022  Esc cancel", cap - wcslen(out) - 1);
}

int leader_vk_of(int action)
{
    for (size_t i = 0; i < sizeof LK / sizeof LK[0]; i++) if (LK[i].action == action) return LK[i].vk;
    return 0;
}

void hotkeys_restore_defaults(HotkeyCfg *c)
{
    int en = c->enabled;
    hotkeys_default(c);
    c->enabled = en;
}

void hotkeys_default(HotkeyCfg *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->leader_mod = HK_MOD_CONTROL | HK_MOD_ALT;
    c->leader_vk = 0x20;
    c->arm_seconds = 5;
    c->media_keys = 1;
    /* Sensible direct shortcuts. All use Ctrl+Alt (nothing in Windows itself uses these, and the letters match the leader keys).
     * A combination another program already owns is reported at start and can be changed in the settings. */
    static const struct { int act, vk, extra; } D[] = {
        { ACT_PLAYPAUSE, 'P', 0 }, { ACT_STOP, 'S', 0 },
        { ACT_PREV_SENT, 0x25, 0 }, { ACT_NEXT_SENT, 0x27, 0 }, { ACT_PREV_PARA, 0x26, 0 }, { ACT_NEXT_PARA, 0x28, 0 },
        { ACT_PREV_CH, 0x21, 0 }, { ACT_NEXT_CH, 0x22, 0 },                      /* PageUp / PageDown */
        { ACT_REPLAY, 0x24, 0 },                                                  /* Home */
        { ACT_JUMP_BACK, 0x08, 0 }, { ACT_JUMP_FWD, 0x08, HK_MOD_SHIFT },         /* Backspace */
        { ACT_SKIP_BACK, 0xBC, 0 }, { ACT_SKIP_FWD, 0xBE, 0 },                    /* , . */
        { ACT_MARK_BOOKMARK, 'B', 0 }, { ACT_MARK_REVISIT, 'R', 0 }, { ACT_MARK_IMPORTANT, 'I', 0 },
        { ACT_MARK_UNCLEAR, 'U', 0 }, { ACT_MARK_QUESTION, 'Q', 0 }, { ACT_MARK_DOUBT, 'X', 0 },
        { ACT_NEXT_MARK, 'M', 0 }, { ACT_PREV_MARK, 'M', HK_MOD_SHIFT },
        { ACT_FASTER, 0xBB, 0 }, { ACT_SLOWER, 0xBD, 0 },                         /* + - */
        { ACT_CAPTIONS, 'C', 0 }, { ACT_SHOW_WINDOW, 'W', 0 },
    };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) {
        c->direct[D[i].act].mod = HK_MOD_CONTROL | HK_MOD_ALT | D[i].extra;
        c->direct[D[i].act].vk = D[i].vk;
    }
}

void hotkeys_from_ini(HotkeyCfg *c, const Ini *d)
{
    hotkeys_default(c);
    /* DefaultsVersion < 3: written by builds that could save these settings before they were loaded, which stored "everything off"
     * over the real values. Those files get the defaults back (global shortcuts on, media keys on, default shortcuts for every
     * action without one); the "Enabled" value is not trusted. */
    int ver = (int)ini_get_int(d, L"Hotkeys", L"DefaultsVersion", 0);
    c->enabled = ver < 3 ? 1 : ini_get_int(d, L"Hotkeys", L"Enabled", 1) != 0;
    c->leader_mod = (int)ini_get_int(d, L"Hotkeys", L"LeaderMod", c->leader_mod) & 15;
    c->leader_vk = (int)ini_get_int(d, L"Hotkeys", L"LeaderVk", c->leader_vk);
    if (c->leader_vk < 0 || c->leader_vk > 255) c->leader_vk = 0;
    if (c->leader_vk && !c->leader_mod) c->leader_vk = 0;           /* a bare key as the leader would steal it everywhere */
    if (ver < 3 && (!c->leader_vk || !c->leader_mod)) { c->leader_mod = HK_MOD_CONTROL | HK_MOD_ALT; c->leader_vk = 0x20; }
    c->arm_seconds = (int)ini_get_int(d, L"Hotkeys", L"ArmSeconds", 5);
    if (ver < 3 && c->arm_seconds <= 0) c->arm_seconds = 5;
    if (c->arm_seconds < 2) c->arm_seconds = 2;
    if (c->arm_seconds > 15) c->arm_seconds = 15;
    /* Before version 3 a "none" entry cannot be told from "never set": the defaults apply to every action without a shortcut, and
     * the media keys come on. Anything the user did choose is kept. */
    c->media_keys = ver < 3 ? 1 : ini_get_int(d, L"Hotkeys", L"MediaKeys", 1) != 0;
    for (int i = 1; i < ACT_COUNT; i++) {
        wchar_t k[64];
        swprintf(k, 64, L"%ls.Mod", A[i].key);
        int mod = (int)ini_get_int(d, L"HotkeysDirect", k, -1);
        swprintf(k, 64, L"%ls.Vk", A[i].key);
        int vk = (int)ini_get_int(d, L"HotkeysDirect", k, -1);
        if (vk < 0 || (vk == 0 && ver < 3)) continue;                 /* not stored: keep the default */
        mod &= 15;
        if (vk > 255 || !mod) vk = 0;
        c->direct[i].mod = vk ? mod : 0;
        c->direct[i].vk = vk;
    }
    /* one combination can only be registered once: the earlier action keeps it */
    for (int i = 1; i < ACT_COUNT; i++)
        for (int j = i + 1; j < ACT_COUNT; j++)
            if (c->direct[i].vk && c->direct[j].vk == c->direct[i].vk && c->direct[j].mod == c->direct[i].mod) {
                c->direct[j].vk = 0; c->direct[j].mod = 0;
            }
}

void hotkeys_to_ini(const HotkeyCfg *c, Ini *d)
{
    ini_set_int(d, L"Hotkeys", L"Enabled", c->enabled);
    ini_set_int(d, L"Hotkeys", L"LeaderMod", c->leader_mod);
    ini_set_int(d, L"Hotkeys", L"LeaderVk", c->leader_vk);
    ini_set_int(d, L"Hotkeys", L"ArmSeconds", c->arm_seconds);
    ini_set_int(d, L"Hotkeys", L"MediaKeys", c->media_keys);
    ini_set_int(d, L"Hotkeys", L"DefaultsVersion", 3);
    ini_remove_section(d, L"HotkeysDirect");
    for (int i = 1; i < ACT_COUNT; i++) {                 /* "none" is written too, so a removed default stays removed */
        wchar_t k[64];
        swprintf(k, 64, L"%ls.Mod", A[i].key); ini_set_int(d, L"HotkeysDirect", k, c->direct[i].mod);
        swprintf(k, 64, L"%ls.Vk", A[i].key);  ini_set_int(d, L"HotkeysDirect", k, c->direct[i].vk);
    }
}

void vk_name(int vk, wchar_t *k, size_t cap)
{
    if (vk == 0x20) wcsncpy(k, L"Space", cap);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { k[0] = (wchar_t)vk; k[1] = 0; }
    else if (vk >= 0x70 && vk <= 0x87) swprintf(k, cap, L"F%d", vk - 0x6F);
    else if (vk == 0x25) wcsncpy(k, L"Left", cap); else if (vk == 0x27) wcsncpy(k, L"Right", cap);
    else if (vk == 0x26) wcsncpy(k, L"Up", cap); else if (vk == 0x28) wcsncpy(k, L"Down", cap);
    else if (vk == 0xDB) wcsncpy(k, L"[", cap); else if (vk == 0xDD) wcsncpy(k, L"]", cap);
    else if (vk == 0xBB) wcsncpy(k, L"+", cap); else if (vk == 0xBD) wcsncpy(k, L"-", cap);
    else if (vk == 0xBC) wcsncpy(k, L",", cap); else if (vk == 0xBE) wcsncpy(k, L".", cap);
    else if (vk == 0x21) wcsncpy(k, L"PageUp", cap); else if (vk == 0x22) wcsncpy(k, L"PageDown", cap);
    else if (vk == 0x24) wcsncpy(k, L"Home", cap); else if (vk == 0x23) wcsncpy(k, L"End", cap);
    else if (vk == 0x08) wcsncpy(k, L"Backspace", cap); else if (vk == 0x1B) wcsncpy(k, L"Esc", cap);
    else swprintf(k, cap, L"Key%d", vk);
    k[cap - 1] = 0;
}

void hotkey_text(int mod, int vk, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (!vk) { wcsncpy(out, L"(none)", cap - 1); out[cap - 1] = 0; return; }
    wchar_t k[24];
    vk_name(vk, k, 24);
    swprintf(out, cap, L"%ls%ls%ls%ls%ls", (mod & HK_MOD_CONTROL) ? L"Ctrl+" : L"", (mod & HK_MOD_ALT) ? L"Alt+" : L"",
             (mod & HK_MOD_SHIFT) ? L"Shift+" : L"", (mod & HK_MOD_WIN) ? L"Win+" : L"", k);
}

void hotkeys_help_text(const HotkeyCfg *c, const wchar_t *problems, wchar_t *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
#define ADD(...) do { if (n < cap) { int w_ = swprintf(out + n, cap - n, __VA_ARGS__); if (w_ > 0) n += (size_t)w_; } } while (0)
    wchar_t t[80], k[24];
    if (!c->enabled) ADD(L"Global shortcuts are switched OFF, so none of the keys below work until you switch them on\r\n"
                         L"(tray icon > Shortcut keys > Global shortcuts on). This is the setup that will apply:\r\n\r\n");
    else ADD(L"Global shortcuts are ON. They work while any other program is in front.\r\n\r\n");
    if (c->leader_vk) {
        hotkey_text(c->leader_mod, c->leader_vk, t, 80);
        ADD(L"LEADER KEY:  %ls%ls\r\n  Press it, then within %d seconds press ONE key from the last column below.\r\n"
            L"  Esc cancels. Keys marked * stay active so you can press them again (arrows, skips, speed).\r\n\r\n",
            t, c->failed[HK_FAIL_LEADER] ? L"   (not free: another program owns it)" : L"", c->arm_seconds);
    } else ADD(L"LEADER KEY:  (none set)\r\n\r\n");
    ADD(L"%-30ls %-26ls %ls\r\n", L"ACTION", L"SHORTCUT (always active)", L"AFTER THE LEADER KEY");
    ADD(L"%-30ls %-26ls %ls\r\n", L"------------------------------", L"--------------------------", L"--------------------");
    for (int a = 1; a < ACT_COUNT; a++) {
        wchar_t sc[60], lk[24];
        if (c->direct[a].vk) {
            hotkey_text(c->direct[a].mod, c->direct[a].vk, t, 80);
            swprintf(sc, 60, L"%ls%ls", t, c->failed[a] ? L" (in use)" : L"");
        } else wcscpy(sc, L"-");
        sc[59] = 0;
        int lv = c->leader_vk ? leader_vk_of(a) : 0;
        if (lv) { vk_name(lv, k, 24); swprintf(lk, 24, L"%ls%ls", k, leader_stays_armed(lv) ? L" *" : L""); lk[23] = 0; }
        else wcscpy(lk, L"-");
        ADD(L"%-30ls %-26ls %ls\r\n", action_name(a), sc, lk);
    }
    ADD(L"\r\nMEDIA KEYS  %ls\r\n", c->media_keys ? L"(on)" : L"(off)");
    static const wchar_t *mn[4] = { L"Play/Pause = play / pause", L"Next track = next paragraph", L"Previous track = previous paragraph", L"Stop = stop" };
    for (int i = 0; i < 4; i++) ADD(L"  %ls%ls\r\n", mn[i], (c->media_keys && c->failed[HK_FAIL_MEDIA(i)]) ? L"   (in use by another program)" : L"");
    if (!c->media_keys) ADD(L"  They usually belong to your music player; switch them on in Shortcut settings if you want them here.\r\n");
    if (problems && problems[0]) ADD(L"\r\nPROBLEMS\r\n  %ls\r\n", problems);
    ADD(L"\r\nChange or clear any of these in Tools > Tray and global shortcuts > Shortcut settings (\"Restore defaults\" is there too).\r\n");
#undef ADD
}
