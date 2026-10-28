#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_profiles.h"
#include "ui_nav.h"

enum { PR_LIST = 950, PR_APPLY, PR_SAVE, PR_DEL };

static struct { HWND hwnd, list; const ProfileUi *ui; volatile int done; int dpi; } P;
#define PS(x) MulDiv((x), P.dpi, 96)

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static void fill(int sel)
{
    SendMessageW(P.list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < P.ui->list->n; i++) {
        const VoiceProfile *v = &P.ui->list->p[i];
        const wchar_t *tail = wcsrchr(v->voice_id, L'\\');
        wchar_t line[400];
        _snwprintf(line, 400, L"%ls    -    %ls, rate %+d, volume %d%%", v->name, tail ? tail + 1 : v->voice_id, v->rate, v->volume);
        line[399] = 0;
        SendMessageW(P.list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < P.ui->list->n) SendMessageW(P.list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h), P.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void apply(void)
{
    int i = (int)SendMessageW(P.list, LB_GETCURSEL, 0, 0);
    if (i < 0) { MessageBoxW(P.hwnd, L"Select a profile first.", L"Voice profiles", MB_OK | MB_ICONINFORMATION); return; }
    if (!P.ui->apply(P.ui->ctx, &P.ui->list->p[i]))
        MessageBoxW(P.hwnd, L"The voice of this profile is not installed. The rate and volume were applied.", L"Voice profiles", MB_OK | MB_ICONWARNING);
    else P.done = 1;
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        P.hwnd = h;
        mk(L"STATIC", L"Profiles (a voice with its rate and volume):", SS_LEFT, 0, 0, 8, 8, 460, 16);
        P.list = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, PR_LIST, 8, 26, 464, 170);
        mk(L"BUTTON", L"&Use selected", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, PR_APPLY, 8, 206, 110, 28);
        mk(L"BUTTON", L"&Save current as...", BS_PUSHBUTTON | WS_TABSTOP, 0, PR_SAVE, 126, 206, 130, 28);
        mk(L"BUTTON", L"&Delete", BS_PUSHBUTTON | WS_TABSTOP, 0, PR_DEL, 264, 206, 80, 28);
        mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 392, 206, 80, 28);
        mk(L"STATIC", L"\"Save current as...\" stores the voice, rate and volume that are set in the main window under a name; "
                      L"saving under an existing name replaces it. Profiles can also be used from the command line (--profile).",
           SS_LEFT, 0, 0, 8, 244, 464, 48);
        EnumChildWindows(h, font_cb, (LPARAM)P.ui->font);
        fill(-1);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case PR_LIST: if (HIWORD(wp) == LBN_DBLCLK) apply(); return 0;
        case PR_APPLY: apply(); return 0;
        case PR_SAVE: {
            wchar_t name[64];
            name[0] = 0;
            int sel = (int)SendMessageW(P.list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) wcsncpy(name, P.ui->list->p[sel].name, 63);
            wchar_t initial[64];
            wcscpy(initial, name);
            if (!input_box(h, P.ui->font, P.ui->dpi, L"Save voice profile", L"Profile name:", initial, name, 64)) return 0;
            VoiceProfile cur;
            memset(&cur, 0, sizeof cur);
            P.ui->current(P.ui->ctx, &cur);
            if (!profiles_set(P.ui->list, name, cur.voice_id, cur.rate, cur.volume)) {
                MessageBoxW(h, L"That name cannot be used (empty, too long, or containing = or a line break).", L"Voice profiles", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (P.ui->changed) P.ui->changed(P.ui->ctx);
            fill(profiles_find(P.ui->list, name));
            return 0;
        }
        case PR_DEL: {
            int sel = (int)SendMessageW(P.list, LB_GETCURSEL, 0, 0);
            if (sel < 0) return 0;
            profiles_remove(P.ui->list, sel);
            if (P.ui->changed) P.ui->changed(P.ui->ctx);
            fill(sel < P.ui->list->n ? sel : P.ui->list->n - 1);
            return 0;
        }
        case IDCANCEL: P.done = 1; return 0;
        }
        break;
    case WM_CLOSE: P.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void profiles_dialog(HWND parent, const ProfileUi *ui)
{
    memset(&P, 0, sizeof P);
    P.ui = ui; P.dpi = ui->dpi;
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceProfiles", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceProfiles";
        RegisterClassExW(&wc);
    }
    RECT r = { 0, 0, PS(480), PS(300) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceProfiles", L"Voice profiles", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(P.list);
    MSG m;
    while (!P.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) { if (rr == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
}
