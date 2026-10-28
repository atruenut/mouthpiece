/* ui_find.h - modeless Find / Replace window working directly on the main editor control. */
#ifndef UI_FIND_H
#define UI_FIND_H
#include <windows.h>

typedef struct FindUi { HWND edit; HFONT font; int dpi; void (*status)(const wchar_t *msg); } FindUi;

void  find_dialog_open(HWND parent, const FindUi *ui, const wchar_t *prefill);   /* shows or focuses the window */
HWND  find_dialog_hwnd(void);                                                     /* for IsDialogMessage; NULL if closed */
void  find_dialog_next(int dir);                                                  /* F3 / Shift+F3 with the last settings */
#endif
