#include "ui_gutter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GUTTER_CLASS L"MouthpieceGutter"
#define COL_CHAPTER RGB(214, 140, 20)
#define COL_BOOKMARK RGB(30, 110, 200)

typedef struct { HWND edit; GutterMark *m; int n; HFONT font; } GState;

static int mark_y(HWND edit, size_t off, int *ok)
{
    LRESULT r = SendMessageW(edit, EM_POSFROMCHAR, (WPARAM)off, 0);
    int y = (short)HIWORD(r);
    RECT rc;
    GetClientRect(edit, &rc);
    *ok = !(r == -1 || y < 0 || y > rc.bottom - 2);
    return y;
}

static void paint(HWND h, GState *s)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ of = SelectObject(mem, s->font ? s->font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm;
    GetTextMetricsW(mem, &tm);
    int lineh = tm.tmHeight + 2;
    for (int i = 0; i < s->n; i++) {
        int ok, y = mark_y(s->edit, s->m[i].offset, &ok);
        if (!ok) continue;
        COLORREF col = s->m[i].kind == GM_CHAPTER ? COL_CHAPTER : s->m[i].kind == GM_FLAG ? (COLORREF)s->m[i].color : COL_BOOKMARK;
        HBRUSH b = CreateSolidBrush(col);
        RECT r = { 1, y, rc.right - 1, y + lineh };
        if (s->m[i].kind == GM_FLAG) {
            int same = 0, idx = 0;                                  /* flags on one line share the strip side by side */
            for (int k = 0; k < s->n; k++) {
                if (s->m[k].kind != GM_FLAG) continue;
                int ok2, y2 = mark_y(s->edit, s->m[k].offset, &ok2);
                if (ok2 && y2 == y) { if (k < i) idx++; same++; }
            }
            if (same < 1) same = 1;
            int wtot = rc.right - 2, cw = wtot / same;
            RECT rf = { 1 + idx * cw, y + 1, 1 + (idx + 1) * cw, y + lineh - 1 };
            FillRect(mem, &rf, b);
            SetTextColor(mem, RGB(255, 255, 255));
            if (s->m[i].glyph && cw >= 7) DrawTextW(mem, &s->m[i].glyph, 1, &rf, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (s->m[i].kind == GM_CHAPTER) {
            FillRect(mem, &r, b);
            SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, L"\x00A7", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);   /* section sign */
        } else {
            RECT r2 = { 1, y + 2, rc.right - 1, y + lineh - 2 };
            FillRect(mem, &r2, b);
            RECT tip = { 1, y, rc.right - 1, y + 2 };
            FillRect(mem, &tip, b);
        }
        DeleteObject(b);
    }
    SelectObject(mem, of);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    GState *s = (GState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (s) paint(h, s); else { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); } return 0;
    case WM_SETFONT: if (s) s->font = (HFONT)wp; return 0;
    case WM_LBUTTONDOWN: if (s) {
        int y = (short)HIWORD(lp);
        for (int i = 0; i < s->n; i++) {
            int ok, my = mark_y(s->edit, s->m[i].offset, &ok);
            if (ok && y >= my - 2 && y <= my + 20) {
                SetFocus(s->edit);
                SendMessageW(s->edit, EM_SETSEL, (WPARAM)s->m[i].offset, (LPARAM)s->m[i].offset);
                SendMessageW(s->edit, EM_SCROLLCARET, 0, 0);
                break;
            }
        }
        return 0;
    } break;
    case WM_RBUTTONUP: if (s) {
        int y = (short)HIWORD(lp);
        for (int i = 0; i < s->n; i++) {
            int ok, my = mark_y(s->edit, s->m[i].offset, &ok);
            if (ok && y >= my - 2 && y <= my + 20) {
                POINT pt = { (short)LOWORD(lp), y };
                ClientToScreen(h, &pt);
                SendMessageW(GetParent(h), WM_GUTTER_RCLICK, (WPARAM)s->m[i].tag, MAKELONG(pt.x, pt.y));
                return 0;
            }
        }
        return 0;
    } break;
    case WM_MOUSEMOVE: if (s) {
        int y = (short)HIWORD(lp);
        for (int i = 0; i < s->n; i++) {
            int ok, my = mark_y(s->edit, s->m[i].offset, &ok);
            if (ok && y >= my - 2 && y <= my + 20) { SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); return 0; }
        }
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
        return 0;
    } break;
    case WM_NCDESTROY:
        if (s) { free(s->m); free(s); }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND gutter_create(HWND parent, HWND edit, int id)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = GUTTER_CLASS;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    GState *s = (GState *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->edit = edit;
    HWND h = CreateWindowExW(0, GUTTER_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), s);
    if (!h) free(s);
    return h;
}

void gutter_set_marks(HWND h, const GutterMark *marks, int n)
{
    GState *s = (GState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!s) return;
    free(s->m);
    s->m = NULL;
    s->n = 0;
    if (n > 0 && marks) {
        s->m = (GutterMark *)malloc((size_t)n * sizeof(GutterMark));
        if (s->m) { memcpy(s->m, marks, (size_t)n * sizeof(GutterMark)); s->n = n; }
    }
    InvalidateRect(h, NULL, FALSE);
}

void gutter_refresh(HWND h)
{
    if (h) InvalidateRect(h, NULL, FALSE);
}
