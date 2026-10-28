#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_find.h"
#include "find.h"

enum { F_WHAT = 900, F_WITH, F_CASE, F_WORD, F_REGEX, F_NEXT, F_PREV, F_REPL, F_ALL, F_CLOSE };

static struct {
    HWND hwnd, what, with, chkCase, chkWord, chkRegex;
    FindUi ui;
    int dpi;
    wchar_t last_what[300], last_with[300];
    FindOpts opts;
} F;

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }
#define FS(x) MulDiv((x), F.dpi, 96)

static void say(const wchar_t *m) { if (F.ui.status) F.ui.status(m); }

static wchar_t *edit_text(size_t *n)
{
    int len = GetWindowTextLengthW(F.ui.edit);
    wchar_t *t = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!t) return NULL;
    t[0] = 0;
    if (len > 0) GetWindowTextW(F.ui.edit, t, len + 1);
    *n = (size_t)len;
    return t;
}

static void pull(void)
{
    GetWindowTextW(F.what, F.last_what, 300);
    GetWindowTextW(F.with, F.last_with, 300);
    F.opts.match_case = SendMessageW(F.chkCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
    F.opts.whole_word = SendMessageW(F.chkWord, BM_GETCHECK, 0, 0) == BST_CHECKED;
    F.opts.regex = SendMessageW(F.chkRegex, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* Find the next/previous match from the current selection; select it. 1 found. */
static int do_find(int dir, int quiet)
{
    if (!F.last_what[0]) { if (!quiet) say(L"Type something to find."); return 0; }
    size_t n;
    wchar_t *t = edit_text(&n);
    if (!t) return 0;
    DWORD sa = 0, sb = 0;
    SendMessageW(F.ui.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    size_t from = dir >= 0 ? sb : sa, st, ln;
    wchar_t err[200];
    err[0] = 0;
    int r = find_next(t, n, F.last_what, &F.opts, from, dir, &st, &ln, err, 200);
    int wrapped = 0;
    if (r == 0) {                                              /* wrap around once */
        r = find_next(t, n, F.last_what, &F.opts, dir >= 0 ? 0 : n, dir, &st, &ln, err, 200);
        wrapped = r == 1;
    }
    free(t);
    if (r < 0) { MessageBoxW(F.hwnd, err, L"Find", MB_OK | MB_ICONWARNING); return 0; }
    if (r == 0) { if (!quiet) say(L"Not found."); return 0; }
    SendMessageW(F.ui.edit, EM_SETSEL, (WPARAM)st, (LPARAM)(st + ln));
    SendMessageW(F.ui.edit, EM_SCROLLCARET, 0, 0);
    say(wrapped ? L"Found (continued from the other end of the document)." : L"Found.");
    return 1;
}

static void do_replace(void)
{
    if (!F.last_what[0]) return;
    size_t n;
    wchar_t *t = edit_text(&n);
    if (!t) return;
    DWORD sa = 0, sb = 0;
    SendMessageW(F.ui.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    size_t st, ln;
    wchar_t err[200];
    err[0] = 0;
    /* replace only if the selection IS a match */
    if (sb > sa && find_next(t, n, F.last_what, &F.opts, sa, 1, &st, &ln, err, 200) == 1 && st == sa && ln == sb - sa) {
        wchar_t *rep = find_replacement_for(t, n, F.last_what, F.last_with, &F.opts, st, ln);
        if (rep) { SendMessageW(F.ui.edit, EM_REPLACESEL, TRUE, (LPARAM)rep); free(rep); }
    }
    free(t);
    do_find(1, 0);
}

static void do_replace_all(void)
{
    if (!F.last_what[0]) return;
    size_t n, cnt = 0;
    wchar_t *t = edit_text(&n);
    if (!t) return;
    wchar_t err[200];
    err[0] = 0;
    wchar_t *out = find_replace_all(t, n, F.last_what, F.last_with, &F.opts, &cnt, err, 200);
    free(t);
    if (!out) { MessageBoxW(F.hwnd, err[0] ? err : L"Out of memory.", L"Replace all", MB_OK | MB_ICONWARNING); return; }
    if (cnt == 0) { free(out); say(L"Not found."); return; }
    wchar_t q[200];
    _snwprintf(q, 200, L"Replace %lu occurrence%ls in the whole text?\n\nThis is one step in the editor's undo (Ctrl+Z).",
               (unsigned long)cnt, cnt == 1 ? L"" : L"s");
    q[199] = 0;
    if (MessageBoxW(F.hwnd, q, L"Replace all", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        SendMessageW(F.ui.edit, EM_SETSEL, 0, -1);
        SendMessageW(F.ui.edit, EM_REPLACESEL, TRUE, (LPARAM)out);
        _snwprintf(q, 200, L"Replaced %lu occurrence%ls.", (unsigned long)cnt, cnt == 1 ? L"" : L"s");
        q[199] = 0;
        say(q);
    }
    free(out);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case F_NEXT: pull(); do_find(1, 0); return 0;
        case F_PREV: pull(); do_find(-1, 0); return 0;
        case F_REPL: pull(); do_replace(); return 0;
        case F_ALL: pull(); do_replace_all(); return 0;
        case F_CLOSE: case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: F.hwnd = NULL; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int hh)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, FS(x), FS(y), FS(w), FS(hh), F.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

void find_dialog_open(HWND parent, const FindUi *ui, const wchar_t *prefill)
{
    if (F.hwnd) {
        if (prefill && prefill[0]) SetWindowTextW(F.what, prefill);
        SetForegroundWindow(F.hwnd);
        SetFocus(F.what);
        SendMessageW(F.what, EM_SETSEL, 0, -1);
        return;
    }
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceFind", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceFind";
        RegisterClassExW(&wc);
    }
    F.ui = *ui;
    F.dpi = ui->dpi;
    RECT r = { 0, 0, FS(460), FS(176) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    F.hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, L"MouthpieceFind", L"Find and replace", style,
                             pr.right - (r.right - r.left) - 30, pr.top + 90, r.right - r.left, r.bottom - r.top,
                             parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!F.hwnd) return;
    mk(L"STATIC", L"Fi&nd:", SS_LEFT, 0, 0, 8, 12, 70, 18);
    F.what = mk(L"EDIT", F.last_what, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, F_WHAT, 84, 8, 366, 24);
    mk(L"STATIC", L"Replace &with:", SS_LEFT, 0, 0, 8, 44, 76, 18);
    F.with = mk(L"EDIT", F.last_with, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, F_WITH, 84, 40, 366, 24);
    F.chkCase = mk(L"BUTTON", L"Match &case", BS_AUTOCHECKBOX | WS_TABSTOP, 0, F_CASE, 84, 72, 100, 20);
    F.chkWord = mk(L"BUTTON", L"&Whole word", BS_AUTOCHECKBOX | WS_TABSTOP, 0, F_WORD, 190, 72, 100, 20);
    F.chkRegex = mk(L"BUTTON", L"Regular &expression ($1 in replacement)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, F_REGEX, 84, 94, 260, 20);
    mk(L"BUTTON", L"Find &next", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, F_NEXT, 8, 130, 84, 28);
    mk(L"BUTTON", L"Find &previous", BS_PUSHBUTTON | WS_TABSTOP, 0, F_PREV, 98, 130, 96, 28);
    mk(L"BUTTON", L"&Replace", BS_PUSHBUTTON | WS_TABSTOP, 0, F_REPL, 200, 130, 80, 28);
    mk(L"BUTTON", L"Replace &all", BS_PUSHBUTTON | WS_TABSTOP, 0, F_ALL, 286, 130, 92, 28);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, F_CLOSE, 384, 130, 66, 28);
    EnumChildWindows(F.hwnd, font_cb, (LPARAM)F.ui.font);
    SendMessageW(F.chkCase, BM_SETCHECK, F.opts.match_case ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(F.chkWord, BM_SETCHECK, F.opts.whole_word ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(F.chkRegex, BM_SETCHECK, F.opts.regex ? BST_CHECKED : BST_UNCHECKED, 0);
    if (prefill && prefill[0]) SetWindowTextW(F.what, prefill);
    ShowWindow(F.hwnd, SW_SHOW);
    SetFocus(F.what);
    SendMessageW(F.what, EM_SETSEL, 0, -1);
}

HWND find_dialog_hwnd(void) { return F.hwnd; }

void find_dialog_next(int dir)
{
    if (F.hwnd) pull();
    if (!F.last_what[0]) { say(L"Press Ctrl+F to choose what to find."); return; }
    do_find(dir, 0);
}
