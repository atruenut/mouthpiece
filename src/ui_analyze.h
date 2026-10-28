/* ui_analyze.h - problem-word report window (results of analyze_document). */
#ifndef UI_ANALYZE_H
#define UI_ANALYZE_H
#include <windows.h>
#include "analyze.h"

enum { ANALYZE_CLOSE = 0, ANALYZE_GOTO = 1, ANALYZE_PRON = 2 };
/* Returns what the user chose; *index is the selected issue for GOTO and PRON. */
int analyze_dialog(HWND parent, HFONT font, int dpi, const Analysis *a, int *index);
#endif
