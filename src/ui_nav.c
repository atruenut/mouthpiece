#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_nav.h"

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static void run_modal(HWND hw, volatile int *done)
{
    MSG m;
    while (!*done) {
        int r = GetMessageW(&m, NULL, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void register_class(const wchar_t *name, WNDPROC proc)
{
    WNDCLASSEXW wc;
    if (GetClassInfoExW(GetModuleHandleW(NULL), name, &wc)) return;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

/* ============================================================== input box === */

static struct { HWND hwnd, edit; HFONT font; int dpi, done, ok; const wchar_t *title, *prompt, *initial; wchar_t *out; int cap; } B;
#define BS(x) MulDiv((x), B.dpi, 96)

static LRESULT CALLBACK b_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        B.hwnd = hwnd;
        HINSTANCE hi = GetModuleHandleW(NULL);
        CreateWindowExW(0, L"STATIC", B.prompt, WS_CHILD | WS_VISIBLE | SS_LEFT, BS(10), BS(10), BS(380), BS(36), hwnd, NULL, hi, NULL);
        B.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", B.initial, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 BS(10), BS(50), BS(380), BS(24), hwnd, NULL, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, BS(214), BS(88), BS(84), BS(28), hwnd, (HMENU)(INT_PTR)IDOK, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, BS(306), BS(88), BS(84), BS(28), hwnd, (HMENU)(INT_PTR)IDCANCEL, hi, NULL);
        EnumChildWindows(hwnd, font_cb, (LPARAM)B.font);
        SendMessageW(B.edit, EM_SETLIMITTEXT, 150, 0);
        SendMessageW(B.edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { GetWindowTextW(B.edit, B.out, B.cap); B.ok = 1; B.done = 1; return 0; }
        if (LOWORD(wp) == IDCANCEL) { B.done = 1; return 0; }
        break;
    case WM_CLOSE: B.done = 1; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int input_box(HWND parent, HFONT font, int dpi, const wchar_t *title, const wchar_t *prompt,
              const wchar_t *initial, wchar_t *out, int cap)
{
    memset(&B, 0, sizeof B);
    B.font = font; B.dpi = dpi; B.title = title; B.prompt = prompt; B.initial = initial ? initial : L""; B.out = out; B.cap = cap;
    register_class(L"MouthpieceInput", b_proc);
    RECT r = { 0, 0, BS(400), BS(128) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceInput", title, style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return 0; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(B.edit);
    run_modal(hw, &B.done);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    return B.ok;
}

/* ================================================================ navigator === */

enum { N_CH = 1000, N_BM, N_GO, N_ADD, N_REMOVE, N_RENAME, N_BMDEL, N_BMREN, N_BMNOTE, N_STATIC };

static struct {
    HWND hwnd, ch, bm;
    NavUi *ui;
    int dpi;
    volatile int done;
    int last_focus;                  /* 0 chapters, 1 bookmarks */
} N;
#define NS(x) MulDiv((x), N.dpi, 96)

static HWND nmk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, NS(x), NS(y), NS(w), NS(h),
                           N.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void n_fill(void)
{
    NavUi *u = N.ui;
    int selc = (int)SendMessageW(N.ch, LB_GETCURSEL, 0, 0), selb = (int)SendMessageW(N.bm, LB_GETCURSEL, 0, 0);
    SendMessageW(N.ch, LB_RESETCONTENT, 0, 0);
    SendMessageW(N.bm, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < u->chapters->n; i++) {
        wchar_t b[260];
        _snwprintf(b, 260, L"%3d   %ls%ls", i + 1, u->chapters->c[i].title, u->chapters->c[i].manual ? L"   [added by you]" : L"");
        b[259] = 0;
        SendMessageW(N.ch, LB_ADDSTRING, 0, (LPARAM)b);
    }
    if (u->chapters->n == 0) SendMessageW(N.ch, LB_ADDSTRING, 0, (LPARAM)L"(no chapter headings found)");
    for (int i = 0; i < u->bookmarks->n; i++) {
        int s = bm_resolve(&u->bookmarks->b[i], u->doc, u->chapters);
        wchar_t b[300];
        const Bookmark *bk = &u->bookmarks->b[i];
        _snwprintf(b, 300, L"[%ls]  %ls%ls%ls%ls", mark_kind_name(bk->kind), bk->label, bk->note ? L"   -   " : L"", bk->note ? bk->note : L"",
                   s < 0 ? L"   (text not found)" : L"");
        b[299] = 0;
        SendMessageW(N.bm, LB_ADDSTRING, 0, (LPARAM)b);
    }
    if (u->bookmarks->n == 0) SendMessageW(N.bm, LB_ADDSTRING, 0, (LPARAM)L"(no marks yet - Ctrl+B bookmarks, Alt+1..5 flag a passage)");
    if (selc >= 0 && selc < u->chapters->n) SendMessageW(N.ch, LB_SETCURSEL, (WPARAM)selc, 0);
    if (selb >= 0 && selb < u->bookmarks->n) SendMessageW(N.bm, LB_SETCURSEL, (WPARAM)selb, 0);
}

static void n_go(int which)
{
    NavUi *u = N.ui;
    if (which == 0) {
        int i = (int)SendMessageW(N.ch, LB_GETCURSEL, 0, 0);
        if (i < 0 || i >= u->chapters->n) return;
        u->goto_sent = u->chapters->c[i].first_sent;
    } else {
        int i = (int)SendMessageW(N.bm, LB_GETCURSEL, 0, 0);
        if (i < 0 || i >= u->bookmarks->n) return;
        int s = bm_resolve(&u->bookmarks->b[i], u->doc, u->chapters);
        if (s < 0) { MessageBoxW(N.hwnd, L"The text this bookmark pointed at no longer exists.", L"Navigator", MB_OK | MB_ICONINFORMATION); return; }
        u->goto_sent = s;
    }
    N.done = 1;
}

static LRESULT CALLBACK n_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NavUi *u = N.ui;
    switch (msg) {
    case WM_CREATE: {
        N.hwnd = hwnd;
        nmk(L"STATIC", L"Chapters (detected headings; correct them with the buttons below):", SS_LEFT, 0, 0, 8, 8, 600, 16);
        N.ch = nmk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, N_CH, 8, 26, 620, 200);
        nmk(L"BUTTON", L"&Go to chapter", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, N_GO, 8, 232, 120, 28);
        nmk(L"BUTTON", L"&Rename...", BS_PUSHBUTTON | WS_TABSTOP, 0, N_RENAME, 136, 232, 100, 28);
        nmk(L"BUTTON", L"Remo&ve heading", BS_PUSHBUTTON | WS_TABSTOP, 0, N_REMOVE, 244, 232, 120, 28);
        nmk(L"BUTTON", L"&Add heading at cursor...", BS_PUSHBUTTON | WS_TABSTOP, 0, N_ADD, 372, 232, 160, 28);
        nmk(L"STATIC", L"Bookmarks and flags (revisit, important, unclear, question, doubt):", SS_LEFT, 0, 0, 8, 272, 300, 16);
        N.bm = nmk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, N_BM, 8, 290, 620, 120);
        nmk(L"BUTTON", L"Go to &mark", BS_PUSHBUTTON | WS_TABSTOP, 0, 0x7001, 8, 416, 130, 28);
        nmk(L"BUTTON", L"Re&name...", BS_PUSHBUTTON | WS_TABSTOP, 0, N_BMREN, 146, 416, 90, 28);
        nmk(L"BUTTON", L"Edit &note...", BS_PUSHBUTTON | WS_TABSTOP, 0, N_BMNOTE, 244, 416, 100, 28);
        nmk(L"BUTTON", L"&Delete", BS_PUSHBUTTON | WS_TABSTOP, 0, N_BMDEL, 352, 416, 80, 28);
        nmk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 538, 416, 90, 28);
        EnumChildWindows(hwnd, font_cb, (LPARAM)u->font);
        n_fill();
        int cur = chapter_of_sentence(u->chapters, u->cursor_sent);
        if (cur >= 0) SendMessageW(N.ch, LB_SETCURSEL, (WPARAM)cur, 0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case N_CH:
            if (HIWORD(wp) == LBN_DBLCLK) n_go(0);
            return 0;
        case N_BM:
            if (HIWORD(wp) == LBN_DBLCLK) n_go(1);
            return 0;
        case N_GO: n_go(0); return 0;
        case 0x7001: n_go(1); return 0;
        case N_RENAME: {
            int i = (int)SendMessageW(N.ch, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= u->chapters->n) return 0;
            wchar_t t[160];
            if (input_box(hwnd, u->font, N.dpi, L"Rename chapter", L"Chapter title (also used for the exported file name):", u->chapters->c[i].title, t, 160) && t[0]) {
                overrides_rename(u->overrides, u->doc, u->chapters->c[i].para, t);
                if (u->redetect) u->redetect(u->ctx);
                n_fill();
            }
            return 0;
        }
        case N_REMOVE: {
            int i = (int)SendMessageW(N.ch, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= u->chapters->n) return 0;
            overrides_remove_heading(u->overrides, u->doc, u->chapters->c[i].para);
            if (u->redetect) u->redetect(u->ctx);
            n_fill();
            return 0;
        }
        case N_ADD: {
            if (u->cursor_sent < 0 || u->cursor_sent >= u->doc->nsent) return 0;
            int para = u->doc->sents[u->cursor_sent].para;
            wchar_t init[160], t[160];
            size_t n = u->doc->paras[para].len < 60 ? u->doc->paras[para].len : 60, k = 0;
            for (size_t j = 0; j < n; j++) { wchar_t c = u->doc->text[u->doc->paras[para].start + j]; init[k++] = (c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c; }
            init[k] = 0;
            if (input_box(hwnd, u->font, N.dpi, L"Add chapter heading", L"A chapter will start at the paragraph under the cursor. Title:", init, t, 160) && t[0]) {
                overrides_add_heading(u->overrides, u->doc, para, t);
                if (u->redetect) u->redetect(u->ctx);
                n_fill();
            }
            return 0;
        }
        case N_BMDEL: {
            int i = (int)SendMessageW(N.bm, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= u->bookmarks->n) return 0;
            bm_remove(u->bookmarks, i);
            if (u->redetect) u->redetect(u->ctx);
            n_fill();
            return 0;
        }
        case N_BMREN: {
            int i = (int)SendMessageW(N.bm, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= u->bookmarks->n) return 0;
            wchar_t t[160];
            if (input_box(hwnd, u->font, N.dpi, L"Rename bookmark", L"Bookmark name:", u->bookmarks->b[i].label, t, 160) && t[0]) {
                size_t l = wcslen(t);
                wchar_t *nl = (wchar_t *)malloc((l + 1) * sizeof(wchar_t));
                if (nl) { memcpy(nl, t, (l + 1) * sizeof(wchar_t)); free(u->bookmarks->b[i].label); u->bookmarks->b[i].label = nl; }
                if (u->redetect) u->redetect(u->ctx);
                n_fill();
            }
            return 0;
        }
        case N_BMNOTE: {
            int i = (int)SendMessageW(N.bm, LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= u->bookmarks->n) return 0;
            wchar_t t[300];
            if (input_box(hwnd, u->font, N.dpi, L"Note on this mark", L"Note (leave empty to remove it):", u->bookmarks->b[i].note ? u->bookmarks->b[i].note : L"", t, 300)) {
                bm_set_note(&u->bookmarks->b[i], t);
                if (u->redetect) u->redetect(u->ctx);
                n_fill();
            }
            return 0;
        }
        case IDCANCEL: N.done = 1; return 0;
        }
        break;
    case WM_CLOSE: N.done = 1; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void nav_dialog(HWND parent, NavUi *ui)
{
    memset(&N, 0, sizeof N);
    N.ui = ui; N.dpi = ui->dpi;
    ui->goto_sent = -1;
    register_class(L"MouthpieceNav", n_proc);
    RECT r = { 0, 0, NS(636), NS(456) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceNav", L"Navigator - chapters and bookmarks", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(N.ch);
    run_modal(hw, &N.done);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
}
