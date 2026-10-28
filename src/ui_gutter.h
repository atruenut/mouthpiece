/*
 * ui_gutter.h - a narrow strip beside the editor that shows where chapters and
 * bookmarks are. A plain EDIT control cannot colour text, so the markers live
 * next to it, aligned to the text lines (positions come from EM_POSFROMCHAR).
 * The strip is a pure view: it holds a list of (source offset, kind, label) and
 * the editor HWND; the application rebuilds the list when the document, chapters
 * or bookmarks change.
 */
#ifndef UI_GUTTER_H
#define UI_GUTTER_H
#include <windows.h>

typedef enum { GM_CHAPTER = 1, GM_BOOKMARK = 2, GM_FLAG = 3 } GutterKind;
/* GM_FLAG: a coloured square with one character (typed marks). tag = index in the app's mark list. */
typedef struct GutterMark { size_t offset; int kind; wchar_t label[48]; wchar_t glyph; unsigned color; int tag; } GutterMark;
#define WM_GUTTER_RCLICK (WM_APP + 41)   /* sent to the parent: wParam = tag (-1 chapters), lParam = MAKELONG(screen x, screen y) */

HWND gutter_create(HWND parent, HWND edit, int id);
/* Replaces the marks (copied). Pass n = 0 to clear. */
void gutter_set_marks(HWND g, const GutterMark *marks, int n);
/* Ask for a repaint (scroll, resize, text change). */
void gutter_refresh(HWND g);
#endif
