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
#include "ui_hotkeys.h"

enum { HD_LIST = 970, HD_KEY, HD_SET, HD_CLEAR, HD_ENABLE, HD_MEDIA, HD_DEFAULTS };

static struct { HWND hwnd, list, key; HotkeyCfg *cfg; HFONT font; int dpi, changed; const wchar_t *note; volatile int done; } H;
#define PS(x) MulDiv((x), H.dpi, 96)

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h), H.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static int to_ctl_mod(int m) { return ((m & HK_MOD_SHIFT) ? HOTKEYF_SHIFT : 0) | ((m & HK_MOD_CONTROL) ? HOTKEYF_CONTROL : 0) | ((m & HK_MOD_ALT) ? HOTKEYF_ALT : 0); }
static int from_ctl_mod(int m) { return ((m & HOTKEYF_SHIFT) ? HK_MOD_SHIFT : 0) | ((m & HOTKEYF_CONTROL) ? HK_MOD_CONTROL : 0) | ((m & HOTKEYF_ALT) ? HK_MOD_ALT : 0); }

static void fill(int sel)
{
    SendMessageW(H.list, LB_RESETCONTENT, 0, 0);
    wchar_t t[64], k[24], line[240];
    hotkey_text(H.cfg->leader_mod, H.cfg->leader_vk, t, 64);
    _snwprintf(line, 240, L"LEADER KEY\t%ls%ls\t(starts the single-key mode)", t, H.cfg->failed[HK_FAIL_LEADER] ? L"  - in use" : L"");
    SendMessageW(H.list, LB_ADDSTRING, 0, (LPARAM)line);
    for (int a = 1; a < ACT_COUNT; a++) {
        int lv = H.cfg->leader_vk ? leader_vk_of(a) : 0;
        if (lv) { vk_name(lv, k, 24); } else k[0] = 0;
        hotkey_text(H.cfg->direct[a].mod, H.cfg->direct[a].vk, t, 64);
        _snwprintf(line, 240, L"%ls\t%ls%ls\t%ls%ls", action_name(a), t, H.cfg->failed[a] ? L"  - in use" : L"", lv ? L"after leader: " : L"", k);
        SendMessageW(H.list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessageW(H.list, LB_SETCURSEL, (WPARAM)(sel < 0 ? 0 : sel), 0);
}

static void on_select(void)
{
    int i = (int)SendMessageW(H.list, LB_GETCURSEL, 0, 0);
    if (i < 0) return;
    int mod = i == 0 ? H.cfg->leader_mod : H.cfg->direct[i].mod, vk = i == 0 ? H.cfg->leader_vk : H.cfg->direct[i].vk;
    SendMessageW(H.key, HKM_SETHOTKEY, MAKEWORD(vk, to_ctl_mod(mod)), 0);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        H.hwnd = h;
        mk(L"STATIC", H.note && H.note[0] ? H.note :
           L"Global shortcuts work while another program is in front. To avoid clashing with that program, only the LEADER key is always "
           L"claimed: press it, then within a few seconds press a single key (Space, arrows, R, I, U, Q, X ...). "
           L"Direct shortcuts below are optional; each needs at least one of Ctrl/Alt/Shift.", SS_LEFT, 0, 0, 8, 8, 524, 52);
        H.list = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, HD_LIST, 8, 66, 524, 200);
        { int tabs[2] = { 110, 215 }; SendMessageW(H.list, LB_SETTABSTOPS, 2, (LPARAM)tabs); }
        mk(L"STATIC", L"Press the new combination here:", SS_LEFT, 0, 0, 8, 276, 200, 16);
        H.key = mk(HOTKEY_CLASSW, L"", WS_TABSTOP, WS_EX_CLIENTEDGE, HD_KEY, 8, 294, 220, 24);
        mk(L"BUTTON", L"&Set", BS_PUSHBUTTON | WS_TABSTOP, 0, HD_SET, 236, 292, 70, 28);
        mk(L"BUTTON", L"C&lear", BS_PUSHBUTTON | WS_TABSTOP, 0, HD_CLEAR, 312, 292, 70, 28);
        HWND c1 = mk(L"BUTTON", L"&Global shortcuts on", BS_AUTOCHECKBOX | WS_TABSTOP, 0, HD_ENABLE, 8, 330, 170, 22);
        HWND c2 = mk(L"BUTTON", L"&Media keys (play/pause, next, previous, stop)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, HD_MEDIA, 186, 330, 300, 22);
        SendMessageW(c1, BM_SETCHECK, H.cfg->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c2, BM_SETCHECK, H.cfg->media_keys ? BST_CHECKED : BST_UNCHECKED, 0);
        mk(L"BUTTON", L"Restore &defaults", BS_PUSHBUTTON | WS_TABSTOP, 0, HD_DEFAULTS, 8, 362, 130, 28);
        mk(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 452, 362, 80, 28);
        EnumChildWindows(h, font_cb, (LPARAM)H.font);
        fill(0);
        on_select();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case HD_LIST: if (HIWORD(wp) == LBN_SELCHANGE) on_select(); return 0;
        case HD_ENABLE: H.cfg->enabled = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; H.changed = 1; return 0;
        case HD_MEDIA: H.cfg->media_keys = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; H.changed = 1; return 0;
        case HD_SET: case HD_CLEAR: {
            int i = (int)SendMessageW(H.list, LB_GETCURSEL, 0, 0);
            if (i < 0) return 0;
            int mod = 0, vk = 0;
            if (LOWORD(wp) == HD_SET) {
                LRESULT r = SendMessageW(H.key, HKM_GETHOTKEY, 0, 0);
                vk = LOBYTE(LOWORD(r)); mod = from_ctl_mod(HIBYTE(LOWORD(r)));
                if (!vk || !mod) { MessageBoxW(h, L"Choose a key together with Ctrl, Alt or Shift (a bare key would be taken away from every other program).", L"Global shortcuts", MB_OK | MB_ICONINFORMATION); return 0; }
            }
            if (i == 0) { H.cfg->leader_mod = mod; H.cfg->leader_vk = vk; }
            else { H.cfg->direct[i].mod = mod; H.cfg->direct[i].vk = vk; }
            H.changed = 1;
            fill(i);
            on_select();
            return 0;
        }
        case HD_DEFAULTS: {
            if (MessageBoxW(h, L"Put back the leader key, the default shortcuts and the media keys? Your own choices are replaced.", L"Global shortcuts",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            hotkeys_restore_defaults(H.cfg);
            SendDlgItemMessageW(h, HD_MEDIA, BM_SETCHECK, H.cfg->media_keys ? BST_CHECKED : BST_UNCHECKED, 0);
            H.changed = 1;
            fill(0);
            on_select();
            return 0;
        }
        case IDCANCEL: H.done = 1; return 0;
        }
        break;
    case WM_CLOSE: H.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int hotkeys_dialog(HWND parent, HFONT font, int dpi, HotkeyCfg *cfg, const wchar_t *note)
{
    memset(&H, 0, sizeof H);
    H.cfg = cfg; H.font = font; H.dpi = dpi; H.note = note;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceHotkeys", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceHotkeys";
        RegisterClassExW(&wc);
    }
    RECT r = { 0, 0, PS(540), PS(398) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceHotkeys", L"Global shortcuts", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return 0; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(H.list);
    MSG m;
    while (!H.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) { if (rr == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    return H.changed;
}

/* ------------------------------------------------------------ shortcut list */
static struct { HWND hwnd, edit; HFONT mono; int dpi; volatile int done; } L_;

static LRESULT CALLBACK help_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT r;
        GetClientRect(h, &r);
        MoveWindow(L_.edit, 8, 8, r.right - 16, r.bottom - 52, TRUE);
        MoveWindow(GetDlgItem(h, IDCANCEL), r.right - 8 - MulDiv(90, L_.dpi, 96), r.bottom - 8 - MulDiv(28, L_.dpi, 96), MulDiv(90, L_.dpi, 96), MulDiv(28, L_.dpi, 96), TRUE);
        return 0;
    }
    case WM_COMMAND: if (LOWORD(wp) == IDCANCEL) { L_.done = 1; return 0; } break;
    case WM_CLOSE: L_.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void hotkeys_help_dialog(HWND parent, HFONT font, int dpi, const HotkeyCfg *cfg, const wchar_t *problems)
{
    memset(&L_, 0, sizeof L_);
    L_.dpi = dpi;
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceShortcutList", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = help_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceShortcutList";
        RegisterClassExW(&wc);
    }
    wchar_t *text = (wchar_t *)malloc(8000 * sizeof(wchar_t));
    if (!text) return;
    hotkeys_help_text(cfg, problems, text, 8000);
    RECT r = { 0, 0, MulDiv(640, dpi, 96), MulDiv(560, dpi, 96) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceShortcutList", L"Global shortcut keys", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); free(text); return; }
    L_.hwnd = hw;
    L_.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                              0, 0, 10, 10, hw, NULL, GetModuleHandleW(NULL), NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hw, (HMENU)(INT_PTR)IDCANCEL, GetModuleHandleW(NULL), NULL);
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(10, dpi, 72);
    lf.lfPitchAndFamily = FIXED_PITCH;
    wcscpy(lf.lfFaceName, L"Consolas");
    L_.mono = CreateFontIndirectW(&lf);
    SendMessageW(L_.edit, WM_SETFONT, (WPARAM)(L_.mono ? L_.mono : font), TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
    RECT cr;
    GetClientRect(hw, &cr);
    SendMessageW(hw, WM_SIZE, 0, MAKELONG(cr.right, cr.bottom));
    ShowWindow(hw, SW_SHOW);
    SetFocus(L_.edit);
    SendMessageW(L_.edit, EM_SETSEL, 0, 0);
    MSG m;
    while (!L_.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) { if (rr == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    if (L_.mono) DeleteObject(L_.mono);
    SetForegroundWindow(parent);
    free(text);
}
