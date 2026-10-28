/*
 * hotkeys.h - the actions that can be triggered from outside the window (tray menu, global shortcuts)
 * and the settings for the global shortcuts. PURE C (no Windows types: modifiers use the Win32 MOD_ bits as plain ints).
 *
 * Global shortcuts that work in any program are a shared resource, so Mouthpiece claims as few as possible:
 *   LEADER MODE (default): ONE key combination (default Ctrl+Alt+Space). Pressing it "arms" Mouthpiece for a few seconds;
 *     during that time plain single keys (Space, arrows, R, I, U, Q, D ...) control it. When the time runs out the single
 *     keys are released again, so they never interfere with typing in other programs.
 *   DIRECT shortcuts: optional, one per action, off by default, chosen by you; a combination another program already
 *     owns is reported instead of silently failing.
 *   MEDIA KEYS: optional (Play/Pause, Next, Previous, Stop).
 */
#ifndef HOTKEYS_H
#define HOTKEYS_H
#include "ini.h"

typedef enum {
    ACT_NONE = 0,
    ACT_PLAYPAUSE, ACT_STOP, ACT_PREV_SENT, ACT_NEXT_SENT, ACT_PREV_PARA, ACT_NEXT_PARA, ACT_PREV_CH, ACT_NEXT_CH,
    ACT_REPLAY, ACT_JUMP_BACK, ACT_JUMP_FWD, ACT_SKIP_BACK, ACT_SKIP_FWD,
    ACT_MARK_BOOKMARK, ACT_MARK_REVISIT, ACT_MARK_IMPORTANT, ACT_MARK_UNCLEAR, ACT_MARK_QUESTION, ACT_MARK_DOUBT,
    ACT_NEXT_MARK, ACT_PREV_MARK, ACT_FASTER, ACT_SLOWER, ACT_CAPTIONS, ACT_SHOW_WINDOW,
    ACT_COUNT
} Action;

const wchar_t *action_name(int a);          /* human name: "Play / pause" */
const wchar_t *action_key(int a);           /* stable ini key: "PlayPause" */
int            action_from_key(const wchar_t *key);

/* Leader-mode key table. vk is a Win32 virtual-key code (letters/digits are their upper-case ASCII). */
typedef struct LeaderKey { int vk; int action; int stays_armed; const wchar_t *label; } LeaderKey;
const LeaderKey *leader_keys(int *n);
int leader_lookup(int vk);                  /* action or ACT_NONE */
int leader_stays_armed(int vk);
void leader_help(wchar_t *out, size_t cap); /* one line for the on-screen reminder */

#define HK_MOD_ALT 1
#define HK_MOD_CONTROL 2
#define HK_MOD_SHIFT 4
#define HK_MOD_WIN 8
typedef struct HotkeyCfg {
    int enabled;                 /* global shortcuts on at all */
    int leader_mod, leader_vk;   /* vk 0 = no leader */
    int arm_seconds;             /* 2..15 */
    int media_keys;
    struct { int mod, vk; } direct[ACT_COUNT];     /* vk 0 = none */
    /* runtime only (never saved): what could not be registered because another program owns it. See HK_FAIL_*. */
    char failed[ACT_COUNT + 5];
} HotkeyCfg;
#define HK_FAIL_LEADER 0                 /* failed[0]: the leader key (a free alternative may have been used instead) */
#define HK_FAIL_MEDIA(i) (ACT_COUNT + (i))   /* i = 0..3: play/pause, next, previous, stop */
/* The Ctrl+Alt direct shortcuts and the media keys that are on out of the box. */
void hotkeys_default(HotkeyCfg *c);
/* Put the default direct shortcuts, leader key and media keys back (keeps "global shortcuts on" as it is). */
void hotkeys_restore_defaults(HotkeyCfg *c);
/* Key that triggers `action` after the leader key (name for display), or 0 if the action has no leader key. */
int  leader_vk_of(int action);
void hotkeys_from_ini(HotkeyCfg *c, const Ini *d);
void hotkeys_to_ini(const HotkeyCfg *c, Ini *d);
/* "Ctrl+Alt+Space"; for display in menus and messages. */
void hotkey_text(int mod, int vk, wchar_t *out, size_t cap);
/* Key name only ("Space", "Left", "["), for the leader key table. */
void vk_name(int vk, wchar_t *out, size_t cap);
/* Complete, readable list of every global shortcut that is configured: leader key and the keys that work after it, direct
 * shortcuts, media keys. `problems` (may be NULL) is appended when some combination could not be registered. */
void hotkeys_help_text(const HotkeyCfg *c, const wchar_t *problems, wchar_t *out, size_t cap);
#endif
