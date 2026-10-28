/* ui_hotkeys.h - global shortcut settings window: leader key, optional direct shortcuts, media keys. */
#ifndef UI_HOTKEYS_H
#define UI_HOTKEYS_H
#include <windows.h>
#include "hotkeys.h"
/* Edits *cfg in place. Returns 1 if anything was changed. `note` (may be NULL) is shown at the top, e.g. which shortcuts
 * another program already owns. */
int hotkeys_dialog(HWND parent, HFONT font, int dpi, HotkeyCfg *cfg, const wchar_t *note);
/* Read-only window listing every configured global shortcut (leader keys, direct shortcuts, media keys). */
void hotkeys_help_dialog(HWND parent, HFONT font, int dpi, const HotkeyCfg *cfg, const wchar_t *problems);
#endif
