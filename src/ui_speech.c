/*
 * ui_speech.c - Pronunciation editor and Speech text preview.
 *
 * Pronunciation editor: plain Win32 controls. Every edit is applied to the
 * dictionary immediately and persisted through ui->changed().
 * Speech preview: two RichEdit panes (SOURCE sentence / SPOKEN text) with the
 * rewritten regions coloured: green = your dictionary, blue = built-in rules.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_speech.h"

/* ============================================================= common === */

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

/* ===================================================== pronunciation editor === */

enum { P_LIST = 700, P_WORD, P_REPL, P_CASE, P_INSIDE, P_SPEAK_O, P_SPEAK_R, P_ADD, P_UPD, P_DEL, P_REGEX,
       P_SC_ALL, P_SC_CH, P_SC_SEL, P_SC_KEEP };

static struct {
    HWND hwnd, list, word, repl, chkCase, chkInside, chkRegex, rAll, rCh, rSel, rKeep;
    wchar_t keep_scope[600];
    const SpeechUi *ui;
    const wchar_t *initial;
    volatile int done;
    int dpi;
} P;

#define PS(x) MulDiv((x), P.dpi, 96)

static HWND pmk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h),
                           P.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void p_fill_list(int select)
{
    SendMessageW(P.list, LB_RESETCONTENT, 0, 0);
    const PronDict *d = P.ui->dict;
    for (int i = 0; i < d->n; i++) {
        wchar_t line[400];
        wchar_t sc[160];
        sc[0] = 0;
        if (d->e[i].scope) { wchar_t t[140]; pronscope_describe(&d->e[i], t, 140); _snwprintf(sc, 160, L"   [%ls]", t); sc[159] = 0; }
        _snwprintf(line, 400, L"%ls   \u2192   %ls%ls%ls%ls%ls", d->e[i].word, d->e[i].repl,
                   d->e[i].regex ? L"   [regex]" : L"", d->e[i].match_case ? L"   [case]" : L"",
                   d->e[i].partial ? L"   [inside words]" : L"", sc);
        line[399] = 0;
        SendMessageW(P.list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (select >= 0 && select < d->n) SendMessageW(P.list, LB_SETCURSEL, (WPARAM)select, 0);
}

static void p_load_fields(int i)
{
    const PronDict *d = P.ui->dict;
    if (i < 0 || i >= d->n) return;
    SetWindowTextW(P.word, d->e[i].word);
    SetWindowTextW(P.repl, d->e[i].repl);
    SendMessageW(P.chkCase, BM_SETCHECK, d->e[i].match_case ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(P.chkInside, BM_SETCHECK, d->e[i].partial ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(P.chkRegex, BM_SETCHECK, d->e[i].regex ? BST_CHECKED : BST_UNCHECKED, 0);
    int which = P_SC_ALL;
    if (d->e[i].scope && d->e[i].scope[0]) {
        wcsncpy(P.keep_scope, d->e[i].scope, 599);
        P.keep_scope[599] = 0;
        which = P_SC_KEEP;
    }
    for (int k = P_SC_ALL; k <= P_SC_KEEP; k++) SendMessageW(GetDlgItem(P.hwnd, k), BM_SETCHECK, k == which ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(P.rKeep, which == P_SC_KEEP);
}

/* The scope string chosen by the radio buttons (NULL = everywhere). */
static const wchar_t *p_scope(void)
{
    if (SendMessageW(P.rCh, BM_GETCHECK, 0, 0) == BST_CHECKED) return P.ui->scope_chapter;
    if (SendMessageW(P.rSel, BM_GETCHECK, 0, 0) == BST_CHECKED) return P.ui->scope_selection;
    if (SendMessageW(P.rKeep, BM_GETCHECK, 0, 0) == BST_CHECKED) return P.keep_scope;
    return NULL;
}

static void p_text(HWND ed, wchar_t *buf, int cap)
{
    GetWindowTextW(ed, buf, cap);
    int n = (int)wcslen(buf);                                  /* trim */
    while (n > 0 && (buf[n - 1] == L' ' || buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) buf[--n] = 0;
    int s = 0;
    while (buf[s] == L' ') s++;
    if (s) memmove(buf, buf + s, ((size_t)(n - s) + 1) * sizeof(wchar_t));
}

static void p_changed(void)
{
    pron_prepare(P.ui->dict);
    if (P.ui->changed) P.ui->changed(P.ui->changed_ctx);
}

static void p_add_or_update(int update)
{
    wchar_t w[256], r[256];
    p_text(P.word, w, 256);
    p_text(P.repl, r, 256);
    int mc = SendMessageW(P.chkCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int in = SendMessageW(P.chkInside, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int rxf = SendMessageW(P.chkRegex, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int flags = (mc ? PRON_CASE : 0) | (in ? PRON_PARTIAL : 0) | (rxf ? PRON_REGEX : 0);
    const wchar_t *scope = p_scope();
    if (rxf) {
        wchar_t re[160];
        if (!pron_regex_check(w, mc, re, 160)) {
            wchar_t m[260];
            _snwprintf(m, 260, L"The regular expression is not valid:\n\n%ls", re);
            m[259] = 0;
            MessageBoxW(P.hwnd, m, L"Pronunciation", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    if (!w[0]) { MessageBoxW(P.hwnd, L"Type the word or phrase to correct first.", L"Pronunciation", MB_OK | MB_ICONINFORMATION); return; }
    PronDict *d = P.ui->dict;
    int sel = update ? (int)SendMessageW(P.list, LB_GETCURSEL, 0, 0) : -1;
    if (update) {
        if (sel < 0) { MessageBoxW(P.hwnd, L"Select an entry in the list to update.", L"Pronunciation", MB_OK | MB_ICONINFORMATION); return; }
        if (!pron_set_ex(d, sel, w, r, flags, scope)) {
            MessageBoxW(P.hwnd, L"Cannot update: another entry already uses that word, or the text is invalid.", L"Pronunciation", MB_OK | MB_ICONWARNING);
            return;
        }
    } else {
        int idx = pron_add_ex(d, w, r, flags, scope);
        if (idx == -1) { MessageBoxW(P.hwnd, L"That word is already in the dictionary. Select it and use Update.", L"Pronunciation", MB_OK | MB_ICONINFORMATION); return; }
        if (idx < 0) { MessageBoxW(P.hwnd, L"The word or replacement is not valid (tabs and line breaks are not allowed).", L"Pronunciation", MB_OK | MB_ICONWARNING); return; }
        sel = idx;
    }
    p_changed();
    p_fill_list(sel);
}

static LRESULT CALLBACK p_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        P.hwnd = hwnd;
        pmk(L"STATIC", L"Dictionary entries (applied before the built-in rules):", SS_LEFT, 0, 0, 8, 8, 600, 16);
        P.list = pmk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, P_LIST, 8, 26, 604, 200);
        pmk(L"STATIC", L"&Word:", SS_LEFT, 0, 0, 8, 238, 70, 18);
        P.word = pmk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, P_WORD, 90, 234, 290, 24);
        pmk(L"STATIC", L"&Replacement:", SS_LEFT, 0, 0, 8, 272, 80, 18);
        P.repl = pmk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, P_REPL, 90, 268, 290, 24);
        P.chkCase = pmk(L"BUTTON", L"Match &case", BS_AUTOCHECKBOX | WS_TABSTOP, 0, P_CASE, 396, 234, 200, 22);
        P.chkInside = pmk(L"BUTTON", L"Match &inside words", BS_AUTOCHECKBOX | WS_TABSTOP, 0, P_INSIDE, 396, 258, 210, 22);
        P.chkRegex = pmk(L"BUTTON", L"&Regular expression (replacement may use $1, $2...)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, P_REGEX, 396, 282, 230, 22);
        pmk(L"STATIC", L"Applies to:", SS_LEFT, 0, 0, 8, 302, 70, 18);
        P.rAll = pmk(L"BUTTON", L"Everywhere", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, P_SC_ALL, 90, 300, 100, 20);
        {
            wchar_t lab[200];
            _snwprintf(lab, 200, L"Chapter: %ls", P.ui->chapter_label && P.ui->scope_chapter ? P.ui->chapter_label : L"(no chapter here)");
            lab[199] = 0;
            P.rCh = pmk(L"BUTTON", lab, BS_AUTORADIOBUTTON | WS_TABSTOP, 0, P_SC_CH, 196, 300, 190, 20);
        }
        P.rSel = pmk(L"BUTTON", L"Current selection", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, P_SC_SEL, 392, 304, 130, 20);
        P.rKeep = pmk(L"BUTTON", L"Keep scope", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, P_SC_KEEP, 528, 304, 90, 20);
        EnableWindow(P.rCh, P.ui->scope_chapter != NULL);
        EnableWindow(P.rSel, P.ui->scope_selection != NULL);
        EnableWindow(P.rKeep, FALSE);
        SendMessageW(P.rAll, BM_SETCHECK, BST_CHECKED, 0);
        pmk(L"BUTTON", L"Speak &Original", BS_PUSHBUTTON | WS_TABSTOP, 0, P_SPEAK_O, 8, 334, 140, 28);
        pmk(L"BUTTON", L"Speak Re&placement", BS_PUSHBUTTON | WS_TABSTOP, 0, P_SPEAK_R, 156, 334, 150, 28);
        pmk(L"BUTTON", L"&Add", BS_PUSHBUTTON | WS_TABSTOP, 0, P_ADD, 8, 374, 90, 28);
        pmk(L"BUTTON", L"&Update", BS_PUSHBUTTON | WS_TABSTOP, 0, P_UPD, 106, 374, 90, 28);
        pmk(L"BUTTON", L"&Delete", BS_PUSHBUTTON | WS_TABSTOP, 0, P_DEL, 204, 374, 90, 28);
        pmk(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 522, 374, 90, 28);
        pmk(L"STATIC", L"Tip: select a word in the main editor before opening this dialog to prefill it. The replacement is plain text "
                       L"that is spoken instead - spell it phonetically, e.g. \"Mack ee uh vell ee\". Test it with the Speak buttons. "
                       L"A chapter or selection scope limits the rule to that part of the text.",
            SS_LEFT, 0, 0, 8, 412, 604, 56);
        EnumChildWindows(hwnd, font_cb, (LPARAM)P.ui->font);
        SendMessageW(P.word, EM_SETLIMITTEXT, 200, 0);
        SendMessageW(P.repl, EM_SETLIMITTEXT, 200, 0);
        p_fill_list(-1);
        if (P.initial && P.initial[0]) {
            SetWindowTextW(P.word, P.initial);
            int i = pron_find(P.ui->dict, P.initial);
            if (i >= 0) { SendMessageW(P.list, LB_SETCURSEL, (WPARAM)i, 0); p_load_fields(i); }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case P_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) p_load_fields((int)SendMessageW(P.list, LB_GETCURSEL, 0, 0));
            return 0;
        case P_ADD: p_add_or_update(0); return 0;
        case P_UPD: p_add_or_update(1); return 0;
        case P_DEL: {
            int sel = (int)SendMessageW(P.list, LB_GETCURSEL, 0, 0);
            if (sel < 0) return 0;
            pron_remove(P.ui->dict, sel);
            p_changed();
            p_fill_list(sel < P.ui->dict->n ? sel : P.ui->dict->n - 1);
            return 0;
        }
        case P_SPEAK_O: case P_SPEAK_R: {
            wchar_t t[256];
            p_text(LOWORD(wp) == P_SPEAK_O ? P.word : P.repl, t, 256);
            if (!t[0]) {
                MessageBoxW(hwnd, LOWORD(wp) == P_SPEAK_O ? L"There is no word to speak." : L"There is no replacement to speak.",
                            L"Pronunciation", MB_OK | MB_ICONINFORMATION);
            } else if (P.ui->speak) {
                P.ui->speak(P.ui->speak_ctx, t);
            }
            return 0;
        }
        case IDCANCEL: P.done = 1; return 0;
        }
        break;
    case WM_CLOSE:
        P.done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void pron_dialog(HWND parent, const SpeechUi *ui, const wchar_t *initial_word)
{
    memset(&P, 0, sizeof P);
    P.ui = ui; P.initial = initial_word; P.dpi = ui->dpi;
    register_class(L"MouthpiecePron", p_proc);
    RECT r = { 0, 0, PS(620), PS(474) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpiecePron", L"Pronunciation", style, x, y, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(P.word);
    run_modal(hw, &P.done);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
}

/* ======================================================= speech preview === */

enum { V_CHK0 = 800, V_NCHK = 5, V_LIST = 810, V_SRC, V_OUT, V_SUMMARY, V_LEG0 = 820, V_SPEAK = 830, V_ADDP };
#define MAX_ITEMS 20000

static const COLORREF C_DICT_FG = RGB(0, 90, 30),  C_DICT_BG = RGB(204, 242, 204);
static const COLORREF C_NORM_FG = RGB(0, 50, 130), C_NORM_BG = RGB(196, 222, 255);

static struct {
    HWND hwnd, chk[V_NCHK], list, summary, src, out, leg[2], speak, addp, ok, cancel, lblS, lblO;
    HBRUSH brush[2];
    HMODULE rich;
    const wchar_t *rich_class;
    const SpeechUi *ui;
    SpeechOptions opts;
    int only_changed, dpi;
    volatile int done;
    int result;
    wchar_t *spoken;               /* text of the selected sentence as spoken */
    int cur;
} V;

#define VS(x) MulDiv((x), V.dpi, 96)

static const wchar_t *const V_LABELS[V_NCHK] = {
    L"Expand abbreviations (Dr. Fig. etc.)",
    L"Read numbers, years, currency, %",
    L"Web and e-mail addresses",
    L"Technical terms (C++, C#, .NET)",
    L"Only show sentences that change",
};

static int *v_field(int i)
{
    int *f[V_NCHK] = { &V.opts.abbreviations, &V.opts.numbers, &V.opts.urls, &V.opts.technical, &V.only_changed };
    return f[i];
}

static void v_paint(HWND ed, size_t a, size_t b, COLORREF fg, COLORREF bg)
{
    if (b <= a) return;
    CHARRANGE cr = { (LONG)a, (LONG)b };
    SendMessageW(ed, EM_EXSETSEL, 0, (LPARAM)&cr);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BACKCOLOR;
    cf.crTextColor = fg; cf.crBackColor = bg;
    SendMessageW(ed, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void v_select(int sentence)
{
    free(V.spoken);
    V.spoken = NULL;
    V.cur = sentence;
    if (sentence < 0) { SetWindowTextW(V.src, L""); SetWindowTextW(V.out, L""); return; }
    wchar_t *plain = doc_speech_text(V.ui->doc, sentence);
    if (!plain) return;
    Speech sp;
    memset(&sp, 0, sizeof sp);
    sp.dict = V.ui->dict; sp.opts = V.opts; sp.sentence1 = sentence + 1;
    SpeechText t;
    if (!speech_render(&sp, plain, wcslen(plain), 1, &t)) { free(plain); return; }
    SetWindowTextW(V.src, plain);
    SetWindowTextW(V.out, t.text);
    for (int i = 0; i < t.nspans; i++) {
        const SpeechSpan *s = &t.spans[i];
        COLORREF fg = s->kind == SPAN_DICT ? C_DICT_FG : C_NORM_FG, bg = s->kind == SPAN_DICT ? C_DICT_BG : C_NORM_BG;
        v_paint(V.src, s->in_start, s->in_start + s->in_len, fg, bg);
        v_paint(V.out, s->out_start, s->out_start + s->out_len, fg, bg);
    }
    CHARRANGE z = { 0, 0 };
    SendMessageW(V.src, EM_EXSETSEL, 0, (LPARAM)&z);
    SendMessageW(V.out, EM_EXSETSEL, 0, (LPARAM)&z);
    V.spoken = t.text;
    t.text = NULL;
    speech_text_free(&t);
    free(plain);
}

static void v_fill(void)
{
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    int keep = V.cur;
    SendMessageW(V.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(V.list, LB_RESETCONTENT, 0, 0);
    Speech sp;
    memset(&sp, 0, sizeof sp);
    sp.dict = V.ui->dict; sp.opts = V.opts;
    int shown = 0, changed = 0, sel = -1, nsent = V.ui->doc->nsent;
    for (int i = 0; i < nsent; i++) {
        wchar_t *plain = doc_speech_text(V.ui->doc, i);
        if (!plain) continue;
        SpeechText t;
        int did = 0;
        sp.sentence1 = i + 1;
        if (speech_render(&sp, plain, wcslen(plain), 0, &t)) {
            did = wcscmp(t.text, plain) != 0;
            speech_text_free(&t);
        }
        if (did) changed++;
        if ((did || !V.only_changed) && shown < MAX_ITEMS) {
            wchar_t item[128], snip[90];
            wcsncpy(snip, plain, 80); snip[80] = 0;
            _snwprintf(item, 128, L"S%-5d %ls%ls", i + 1, snip, wcslen(plain) > 80 ? L"..." : L"");
            item[127] = 0;
            int idx = (int)SendMessageW(V.list, LB_ADDSTRING, 0, (LPARAM)item);
            SendMessageW(V.list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
            if (i == keep) sel = idx;
            shown++;
        }
        free(plain);
    }
    SendMessageW(V.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(V.list, NULL, TRUE);
    wchar_t b[200];
    _snwprintf(b, 200, L"%d of %d sentences are changed by the speech rules and your dictionary (%d dictionary entries).%ls",
               changed, nsent, V.ui->dict->n, shown >= MAX_ITEMS ? L"  List limited to the first 20000." : L"");
    b[199] = 0;
    SetWindowTextW(V.summary, b);
    if (sel >= 0) SendMessageW(V.list, LB_SETCURSEL, (WPARAM)sel, 0);
    v_select(sel >= 0 ? keep : -1);
    SetCursor(old);
}

static void v_layout(int w, int h)
{
    int m = VS(8), colw = (w - 2 * m) / 3, rowh = VS(22), y = m;
    for (int i = 0; i < V_NCHK; i++)
        MoveWindow(V.chk[i], m + (i % 3) * colw, y + (i / 3) * rowh, colw - VS(4), rowh, TRUE);
    y += 2 * rowh + VS(4);
    MoveWindow(V.summary, m, y, w - 2 * m, VS(20), TRUE);
    y += VS(26);
    int bottom = h - m - VS(28) - m;
    int listw = (w - 2 * m) * 34 / 100, gap = VS(6);
    MoveWindow(V.list, m, y, listw, bottom - y, TRUE);
    int x = m + listw + gap, rw = w - m - x, ph = (bottom - y - VS(36)) / 2;
    MoveWindow(V.lblS, x, y, rw, VS(16), TRUE);
    MoveWindow(V.src, x, y + VS(18), rw, ph, TRUE);
    MoveWindow(V.lblO, x, y + VS(22) + ph, rw, VS(16), TRUE);
    MoveWindow(V.out, x, y + VS(40) + ph, rw, bottom - (y + VS(40) + ph), TRUE);
    MoveWindow(V.leg[0], m, h - m - VS(26), VS(150), VS(24), TRUE);
    MoveWindow(V.leg[1], m + VS(154), h - m - VS(26), VS(190), VS(24), TRUE);
    int bx = w - m;
    bx -= VS(90);  MoveWindow(V.cancel, bx, h - m - VS(28), VS(90), VS(28), TRUE);
    bx -= VS(150) + VS(8); MoveWindow(V.ok, bx, h - m - VS(28), VS(150), VS(28), TRUE);
    bx -= VS(170) + VS(8); MoveWindow(V.addp, bx, h - m - VS(28), VS(170), VS(28), TRUE);
    bx -= VS(100) + VS(8); MoveWindow(V.speak, bx, h - m - VS(28), VS(100), VS(28), TRUE);
}

static HWND vmk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, V.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static LRESULT CALLBACK v_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        V.hwnd = hwnd;
        for (int i = 0; i < V_NCHK; i++) {
            V.chk[i] = vmk(L"BUTTON", V_LABELS[i], BS_AUTOCHECKBOX | WS_TABSTOP, 0, V_CHK0 + i);
            SendMessageW(V.chk[i], BM_SETCHECK, *v_field(i) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        V.summary = vmk(L"STATIC", L"", SS_LEFT, 0, V_SUMMARY);
        V.list = vmk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, V_LIST);
        V.lblS = vmk(L"STATIC", L"SOURCE (cleaned sentence)", SS_LEFT, 0, 0);
        V.lblO = vmk(L"STATIC", L"SPOKEN (what the voice receives)", SS_LEFT, 0, 0);
        DWORD ro = ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL | WS_TABSTOP;
        V.src = vmk(V.rich_class, L"", ro, WS_EX_CLIENTEDGE, V_SRC);
        V.out = vmk(V.rich_class, L"", ro, WS_EX_CLIENTEDGE, V_OUT);
        V.leg[0] = vmk(L"STATIC", L"Your dictionary", SS_CENTER | SS_CENTERIMAGE, 0, V_LEG0);
        V.leg[1] = vmk(L"STATIC", L"Automatic normalization", SS_CENTER | SS_CENTERIMAGE, 0, V_LEG0 + 1);
        V.speak = vmk(L"BUTTON", L"&Speak this", BS_PUSHBUTTON | WS_TABSTOP, 0, V_SPEAK);
        V.addp = vmk(L"BUTTON", L"Add &pronunciation...", BS_PUSHBUTTON | WS_TABSTOP, 0, V_ADDP);
        V.ok = vmk(L"BUTTON", L"&Use these settings", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK);
        V.cancel = vmk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL);
        EnumChildWindows(hwnd, font_cb, (LPARAM)V.ui->font);
        SendMessageW(V.src, EM_SETUNDOLIMIT, 0, 0);
        SendMessageW(V.out, EM_SETUNDOLIMIT, 0, 0);
        return 0;
    }
    case WM_SIZE:
        if (V.ok) v_layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = VS(760);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = VS(480);
        return 0;
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)lp) - V_LEG0;
        if (id >= 0 && id < 2) {
            static const COLORREF fg[2] = { RGB(0, 90, 30), RGB(0, 50, 130) };
            static const COLORREF bg[2] = { RGB(204, 242, 204), RGB(196, 222, 255) };
            if (!V.brush[id]) V.brush[id] = CreateSolidBrush(bg[id]);
            SetTextColor((HDC)wp, fg[id]);
            SetBkColor((HDC)wp, bg[id]);
            return (LRESULT)V.brush[id];
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= V_CHK0 && id < V_CHK0 + V_NCHK && HIWORD(wp) == BN_CLICKED) {
            int i = id - V_CHK0;
            *v_field(i) = SendMessageW(V.chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            v_fill();
        } else if (id == V_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(V.list, LB_GETCURSEL, 0, 0);
            v_select(sel >= 0 ? (int)SendMessageW(V.list, LB_GETITEMDATA, (WPARAM)sel, 0) : -1);
        } else if (id == V_SPEAK) {
            if (V.spoken && V.ui->speak) V.ui->speak(V.ui->speak_ctx, V.spoken);
            else MessageBoxW(hwnd, L"Select a sentence first.", L"Speech preview", MB_OK | MB_ICONINFORMATION);
        } else if (id == V_ADDP) {
            wchar_t sel[200];
            sel[0] = 0;
            CHARRANGE cr;
            SendMessageW(V.src, EM_EXGETSEL, 0, (LPARAM)&cr);
            if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < 190) SendMessageW(V.src, EM_GETSELTEXT, 0, (LPARAM)sel);
            pron_dialog(hwnd, V.ui, sel);
            v_fill();                                       /* the dictionary may have changed */
        } else if (id == IDOK) {
            V.result = 1; V.done = 1;
        } else if (id == IDCANCEL) {
            V.result = 0; V.done = 1;
        }
        return 0;
    }
    case WM_CLOSE:
        V.result = 0; V.done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int speech_preview_dialog(HWND parent, const SpeechUi *ui)
{
    memset(&V, 0, sizeof V);
    V.ui = ui; V.dpi = ui->dpi; V.opts = *ui->opts; V.only_changed = 1; V.cur = -1;
    V.rich = LoadLibraryW(L"Msftedit.dll");
    V.rich_class = L"RICHEDIT50W";
    if (!V.rich) { V.rich = LoadLibraryW(L"Riched20.dll"); V.rich_class = L"RichEdit20W"; }
    if (!V.rich) {
        MessageBoxW(parent, L"The Windows RichEdit library could not be loaded.", L"Mouthpiece", MB_OK | MB_ICONERROR);
        return 0;
    }
    register_class(L"MouthpieceSpeechPreview", v_proc);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = VS(1000), h = VS(680);
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceSpeechPreview", L"Speech text preview",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                              x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); FreeLibrary(V.rich); return 0; }
    RECT cr;
    GetClientRect(hw, &cr);
    v_layout(cr.right, cr.bottom);
    ShowWindow(hw, SW_SHOW);
    v_fill();
    SetFocus(V.list);
    run_modal(hw, &V.done);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    for (int i = 0; i < 2; i++) if (V.brush[i]) DeleteObject(V.brush[i]);
    free(V.spoken);
    FreeLibrary(V.rich);
    if (V.result) {
        *ui->opts = V.opts;
        if (ui->changed) ui->changed(ui->changed_ctx);
    }
    return V.result;
}
