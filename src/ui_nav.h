/* ui_nav.h - Navigator: detected chapters (with corrections) and bookmarks. */
#ifndef UI_NAV_H
#define UI_NAV_H
#include <windows.h>
#include "chapters.h"

typedef struct NavUi {
    HFONT font;
    int dpi;
    const Document *doc;
    ChapterList *chapters;              /* re-detected in place by redetect() */
    Overrides *overrides;               /* edited by the dialog */
    BookmarkList *bookmarks;            /* edited by the dialog */
    int cursor_sent;                    /* where "Add heading" applies */
    void (*redetect)(void *ctx);        /* app re-runs chapter detection and persists the change */
    void *ctx;
    int goto_sent;                      /* out: sentence to jump to, -1 = stay */
} NavUi;

void nav_dialog(HWND parent, NavUi *ui);
/* Small modal text prompt. Returns 1 and fills out (cap chars) on OK. */
int  input_box(HWND parent, HFONT font, int dpi, const wchar_t *title, const wchar_t *prompt,
               const wchar_t *initial, wchar_t *out, int cap);
#endif
