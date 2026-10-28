#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_goto.h"

enum { GZ_SLIDER = 990, GZ_DESC, GZ_TAB, GZ_LIST, GZ_EDIT, GZ_PLAY, GZ_GO, GZ_HINT };
enum { SRC_SLIDER = 0, SRC_LIST, SRC_TYPED };

static struct { HWND hwnd, slider, desc, tab, list, edit; GotoUi *u; int dpi, src, ok; volatile int done; } Z;
#define PS(x) MulDiv((x), Z.dpi, 96)

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h), Z.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static const GotoItem *tab_items(int *n)
{
    int t = (int)SendMessageW(Z.tab, TCM_GETCURSEL, 0, 0);
    if (t == 0) { *n = Z.u->nch; return Z.u->chapters; }
    if (t == 1) { *n = Z.u->nmk; return Z.u->marks; }
    *n = Z.u->nrec;
    return Z.u->recent;
}

static void fill_list(void)
{
    int n;
    const GotoItem *it = tab_items(&n);
    SendMessageW(Z.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(Z.list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) SendMessageW(Z.list, LB_ADDSTRING, 0, (LPARAM)it[i].label);
    if (n == 0) {
        int t = (int)SendMessageW(Z.tab, TCM_GETCURSEL, 0, 0);
        SendMessageW(Z.list, LB_ADDSTRING, 0, (LPARAM)(t == 0 ? L"(no chapters were detected)" : t == 1 ? L"(no marks yet - Ctrl+B bookmarks, Alt+1..5 flag a passage)" : L"(you have not jumped anywhere yet)"));
    }
    SendMessageW(Z.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(Z.list, NULL, TRUE);
}

static void describe_sentence(int sent)
{
    wchar_t b[300];
    Z.u->describe(Z.u->ctx, sent, b, 300);
    SetWindowTextW(Z.desc, b);
}

static int slider_sentence(void)
{
    double f = (double)SendMessageW(Z.slider, TBM_GETPOS, 0, 0) / 1000.0;
    return Z.u->at_fraction(Z.u->ctx, f);
}

static void go(void)
{
    int target = -1;
    if (Z.src == SRC_LIST) {
        int n, i = (int)SendMessageW(Z.list, LB_GETCURSEL, 0, 0);
        const GotoItem *it = tab_items(&n);
        if (i >= 0 && i < n) target = it[i].sent;
        else { MessageBoxW(Z.hwnd, L"Select an entry in the list first.", L"Go to", MB_OK | MB_ICONINFORMATION); return; }
    } else if (Z.src == SRC_TYPED) {
        wchar_t spec[80], err[160];
        GetWindowTextW(Z.edit, spec, 80);
        err[0] = 0;
        target = Z.u->parse(Z.u->ctx, spec, err, 160);
        if (target < 0) { MessageBoxW(Z.hwnd, err[0] ? err : L"That place was not understood.", L"Go to", MB_OK | MB_ICONINFORMATION); return; }
    } else target = slider_sentence();
    if (target < 0) return;
    Z.u->out_sent = target;
    Z.u->out_play = SendDlgItemMessageW(Z.hwnd, GZ_PLAY, BM_GETCHECK, 0, 0) == BST_CHECKED;
    Z.ok = 1;
    Z.done = 1;
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        Z.hwnd = h;
        mk(L"STATIC", L"Position in the whole document (drag, or use the arrow keys):", SS_LEFT, 0, 0, 10, 8, 460, 18);
        Z.slider = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, GZ_SLIDER, 10, 28, 480, 28);
        SendMessageW(Z.slider, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        SendMessageW(Z.slider, TBM_SETPAGESIZE, 0, 50);
        SendMessageW(Z.slider, TBM_SETLINESIZE, 0, 10);
        double f = Z.u->fraction_of(Z.u->ctx, Z.u->cur);
        SendMessageW(Z.slider, TBM_SETPOS, TRUE, (LPARAM)(f * 1000.0));
        Z.desc = mk(L"STATIC", L"", SS_LEFT | SS_SUNKEN | SS_CENTERIMAGE, 0, GZ_DESC, 10, 60, 480, 38);
        describe_sentence(Z.u->cur);
        Z.tab = mk(WC_TABCONTROLW, L"", WS_TABSTOP, 0, GZ_TAB, 10, 108, 480, 232);
        TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"Chapters"; SendMessageW(Z.tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L"Marks and bookmarks"; SendMessageW(Z.tab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L"Recent places"; SendMessageW(Z.tab, TCM_INSERTITEMW, 2, (LPARAM)&ti);
        Z.list = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, GZ_LIST, 16, 138, 468, 196);
        mk(L"STATIC", L"Or type a place:", SS_LEFT, 0, 0, 10, 352, 100, 18);
        Z.edit = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, GZ_EDIT, 112, 348, 120, 24);
        mk(L"STATIC", L"40%   c12 chapter   p300 paragraph   s1200 sentence   m3 mark   +30s  -2m  +1h", SS_LEFT, 0, GZ_HINT, 240, 352, 250, 34);
        mk(L"BUTTON", L"&Start playing from there", BS_AUTOCHECKBOX | WS_TABSTOP, 0, GZ_PLAY, 10, 384, 200, 22);
        mk(L"BUTTON", L"&Go", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, GZ_GO, 316, 380, 84, 28);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 406, 380, 84, 28);
        EnumChildWindows(h, font_cb, (LPARAM)Z.u->font);
        fill_list();
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == Z.slider) { Z.src = SRC_SLIDER; describe_sentence(slider_sentence()); }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == GZ_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) fill_list();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case GZ_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                Z.src = SRC_LIST;
                int n, i = (int)SendMessageW(Z.list, LB_GETCURSEL, 0, 0);
                const GotoItem *it = tab_items(&n);
                if (i >= 0 && i < n) describe_sentence(it[i].sent);
            } else if (HIWORD(wp) == LBN_DBLCLK) { Z.src = SRC_LIST; go(); }
            return 0;
        case GZ_EDIT:
            if (HIWORD(wp) == EN_CHANGE) {
                Z.src = SRC_TYPED;
                wchar_t spec[80], err[160];
                GetWindowTextW(Z.edit, spec, 80);
                err[0] = 0;
                int t = spec[0] ? Z.u->parse(Z.u->ctx, spec, err, 160) : -1;
                if (t >= 0) describe_sentence(t); else if (spec[0]) SetWindowTextW(Z.desc, err[0] ? err : L"...");
            }
            return 0;
        case GZ_GO: go(); return 0;
        case IDCANCEL: Z.done = 1; return 0;
        }
        break;
    case WM_CLOSE: Z.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int goto_dialog(HWND parent, GotoUi *ui)
{
    memset(&Z, 0, sizeof Z);
    Z.u = ui; Z.dpi = ui->dpi;
    ui->out_sent = -1; ui->out_play = 0;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceGoto", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceGoto";
        RegisterClassExW(&wc);
    }
    RECT r = { 0, 0, PS(500), PS(420) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceGoto", L"Go to", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return 0; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(Z.slider);
    MSG m;
    while (!Z.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) { if (rr == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    return Z.ok;
}
