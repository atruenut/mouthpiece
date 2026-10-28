/* ui_cleanup.h - modal "Cleanup options & preview" dialog. */
#ifndef UI_CLEANUP_H
#define UI_CLEANUP_H
#include <windows.h>
#include "preproc.h"

/* Shows ORIGINAL and PROCESSED side by side with a clickable list of every
 * individual change. `opts` is edited live; returns 1 if the user pressed
 * "Use these settings" (opts updated), 0 if cancelled (opts untouched).
 * `src` is borrowed for the duration of the call. */
int cleanup_dialog(HWND parent, HFONT font, int dpi, const wchar_t *src, size_t srclen, PreprocOptions *opts);

#endif
