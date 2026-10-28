#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_analyze.h"

enum { AZ_LIST = 960, AZ_GOTO, AZ_PRON, AZ_HINT, AZ_HIDE };

static struct { HWND hwnd, list, hint; const Analysis *a; HFONT font; int dpi, result, index, hide_handled; volatile int done; int *map; int nmap; } Z;
#define PS(x) MulDiv((x), Z.dpi, 96)

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h), Z.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void fill(void)
{
    SendMessageW(Z.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(Z.list, LB_RESETCONTENT, 0, 0);
    Z.nmap = 0;
    for (int i = 0; i < Z.a->n; i++) {
        const Issue *is = &Z.a->issues[i];
        if (Z.hide_handled && is->handled) continue;
        wchar_t line[200];
        _snwprintf(line, 200, L"%ls:  %ls   (%d time%ls, first at sentence %d)%ls", analysis_kind_name(is->kind), is->token, is->count,
                   is->count == 1 ? L"" : L"s", is->first_sent + 1, is->handled ? L"   - handled" : L"");
        line[199] = 0;
        SendMessageW(Z.list, LB_ADDSTRING, 0, (LPARAM)line);
        Z.map[Z.nmap++] = i;
    }
    SendMessageW(Z.list, WM_SETREDRAW, TRUE, 0);
    if (Z.nmap) SendMessageW(Z.list, LB_SETCURSEL, 0, 0);
    SetWindowTextW(Z.hint, Z.nmap ? analysis_kind_hint(Z.a->issues[Z.map[0]].kind) : L"Nothing to report.");
}

static int selected(void)
{
    int s = (int)SendMessageW(Z.list, LB_GETCURSEL, 0, 0);
    return (s >= 0 && s < Z.nmap) ? Z.map[s] : -1;
}

static void choose(int result)
{
    int i = selected();
    if (i < 0) { MessageBoxW(Z.hwnd, L"Select an item first.", L"Problem words", MB_OK | MB_ICONINFORMATION); return; }
    Z.result = result; Z.index = i; Z.done = 1;
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        Z.hwnd = h;
        wchar_t head[300];
        int unhandled = 0;
        for (int i = 0; i < Z.a->n; i++) if (!Z.a->issues[i].handled) unhandled++;
        _snwprintf(head, 300, L"%llu words, %d paragraphs, %d sentences, %d chapters.  %d places a voice may stumble on (%d not handled yet). Nothing is changed by this report.",
                   (unsigned long long)Z.a->words, Z.a->paragraphs, Z.a->sentences, Z.a->chapters, Z.a->n, unhandled);
        head[299] = 0;
        mk(L"STATIC", head, SS_LEFT, 0, 0, 8, 8, 564, 32);
        Z.list = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, AZ_LIST, 8, 44, 564, 220);
        Z.hint = mk(L"STATIC", L"", SS_LEFT, 0, AZ_HINT, 8, 272, 564, 34);
        mk(L"BUTTON", L"&Go to first", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, AZ_GOTO, 8, 314, 100, 28);
        mk(L"BUTTON", L"&Add pronunciation...", BS_PUSHBUTTON | WS_TABSTOP, 0, AZ_PRON, 116, 314, 150, 28);
        mk(L"BUTTON", L"&Hide handled", BS_AUTOCHECKBOX | WS_TABSTOP, 0, AZ_HIDE, 276, 318, 130, 22);
        mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 492, 314, 80, 28);
        EnumChildWindows(h, font_cb, (LPARAM)Z.font);
        fill();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case AZ_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) { int i = selected(); if (i >= 0) SetWindowTextW(Z.hint, analysis_kind_hint(Z.a->issues[i].kind)); }
            else if (HIWORD(wp) == LBN_DBLCLK) choose(ANALYZE_GOTO);
            return 0;
        case AZ_GOTO: choose(ANALYZE_GOTO); return 0;
        case AZ_PRON: choose(ANALYZE_PRON); return 0;
        case AZ_HIDE: Z.hide_handled = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; fill(); return 0;
        case IDCANCEL: Z.done = 1; return 0;
        }
        break;
    case WM_CLOSE: Z.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int analyze_dialog(HWND parent, HFONT font, int dpi, const Analysis *a, int *index)
{
    memset(&Z, 0, sizeof Z);
    Z.a = a; Z.font = font; Z.dpi = dpi;
    Z.map = (int *)malloc(((size_t)(a->n ? a->n : 1)) * sizeof(int));
    if (!Z.map) return ANALYZE_CLOSE;
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceAnalyze", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceAnalyze";
        RegisterClassExW(&wc);
    }
    RECT r = { 0, 0, PS(580), PS(354) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceAnalyze", L"Problem words", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); free(Z.map); return ANALYZE_CLOSE; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(Z.list);
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
    free(Z.map);
    if (index) *index = Z.index;
    return Z.result;
}
