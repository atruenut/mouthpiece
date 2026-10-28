#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_caption_dlg.h"
#include "ui_nav.h"

enum { CD_ENABLE = 980, CD_PRESET, CD_FONT, CD_SIZE, CD_SIZEVAL, CD_TEXTCOL, CD_BGCOL, CD_TOP, CD_BGOP, CD_WIDTH, CD_OUTLINE,
       CD_POS_BOTTOM, CD_POS_TOP, CD_POS_CUSTOM, CD_MOVE, CD_RESETPOS, CD_HIDE, CD_DEFAULTS, CD_BOLD, CD_FONTNAME,
       CD_TOPVAL, CD_BGOPVAL, CD_WIDTHVAL, CD_SAVELOOK, CD_DELLOOK };

static struct {
    HWND hwnd;
    CaptionCfg *cfg;
    void (*apply)(const CaptionCfg *, void *);
    void *ctx;
    CaptionLooks *looks;
    void (*looks_changed)(void *);
    int npresets;
    HFONT font;
    int dpi, changed;
    volatile int done;
    COLORREF custom[16];
} D;
#define PS(x) MulDiv((x), D.dpi, 96)

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, PS(x), PS(y), PS(w), PS(h), D.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static HWND slider(int id, int lo, int hi, int val, int x, int y, int w)
{
    HWND t = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, id, x, y, w, 24);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
    SendMessageW(t, TBM_SETPOS, TRUE, val);
    return t;
}

static void changed(void)
{
    D.changed = 1;
    if (D.apply) D.apply(D.cfg, D.ctx);
}

static void fill_looks(const wchar_t *select_name);

static void sync_labels(void)
{
    wchar_t b[80];
    _snwprintf(b, 80, L"%ls, %d pt%ls", D.cfg->face, D.cfg->font_pt, D.cfg->bold ? L", bold" : L"");
    SetDlgItemTextW(D.hwnd, CD_FONTNAME, b);
    _snwprintf(b, 80, L"%d pt", D.cfg->font_pt);   SetDlgItemTextW(D.hwnd, CD_SIZEVAL, b);
    _snwprintf(b, 80, L"%d%%", D.cfg->text_opacity); SetDlgItemTextW(D.hwnd, CD_TOPVAL, b);
    _snwprintf(b, 80, L"%d%%", D.cfg->bg_opacity);   SetDlgItemTextW(D.hwnd, CD_BGOPVAL, b);
    _snwprintf(b, 80, L"%d%%", D.cfg->width_pct);    SetDlgItemTextW(D.hwnd, CD_WIDTHVAL, b);
    if (D.looks) fill_looks(NULL);
    CheckRadioButton(D.hwnd, CD_POS_BOTTOM, CD_POS_CUSTOM, CD_POS_BOTTOM + D.cfg->position);
    SetDlgItemTextW(D.hwnd, CD_MOVE, caption_moving() ? L"&Done moving" : L"&Move by dragging...");
    InvalidateRect(GetDlgItem(D.hwnd, CD_TEXTCOL), NULL, TRUE);
    InvalidateRect(GetDlgItem(D.hwnd, CD_BGCOL), NULL, TRUE);
}

static int pick_color(unsigned *col)
{
    CHOOSECOLORW cc;
    memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = D.hwnd;
    cc.rgbResult = *col;
    cc.lpCustColors = D.custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return 0;
    *col = cc.rgbResult;
    return 1;
}

static void pick_font(void)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    HDC dc = GetDC(NULL);
    lf.lfHeight = -MulDiv(D.cfg->font_pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    lf.lfWeight = D.cfg->bold ? FW_BOLD : FW_NORMAL;
    wcsncpy(lf.lfFaceName, D.cfg->face, 31);
    CHOOSEFONTW cf;
    memset(&cf, 0, sizeof cf);
    cf.lStructSize = sizeof cf;
    cf.hwndOwner = D.hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_TTONLY;
    if (!ChooseFontW(&cf)) return;
    wcsncpy(D.cfg->face, lf.lfFaceName, 31);
    D.cfg->face[31] = 0;
    D.cfg->bold = lf.lfWeight >= FW_SEMIBOLD;
    int pt = cf.iPointSize / 10;
    if (pt < 12) pt = 12;
    if (pt > 72) pt = 72;
    D.cfg->font_pt = pt;
    SendDlgItemMessageW(D.hwnd, CD_SIZE, TBM_SETPOS, TRUE, pt);
    sync_labels();
    changed();
}

static CaptionLook look_of_cfg(const CaptionCfg *c, const wchar_t *name)
{
    CaptionLook l;
    memset(&l, 0, sizeof l);
    if (name) wcsncpy(l.name, name, LOOK_NAME - 1);
    wcsncpy(l.face, c->face, 31);
    l.font_pt = c->font_pt; l.bold = c->bold; l.text_color = c->text_color; l.bg_color = c->bg_color;
    l.text_opacity = c->text_opacity; l.bg_opacity = c->bg_opacity; l.outline = c->outline; l.width_pct = c->width_pct;
    return l;
}

/* a look changes how the caption appears, never where it is, nor whether it is on */
static void apply_look(const CaptionLook *l)
{
    wcsncpy(D.cfg->face, l->face, 31);
    D.cfg->font_pt = l->font_pt; D.cfg->bold = l->bold; D.cfg->text_color = l->text_color; D.cfg->bg_color = l->bg_color;
    D.cfg->text_opacity = l->text_opacity; D.cfg->bg_opacity = l->bg_opacity; D.cfg->outline = l->outline; D.cfg->width_pct = l->width_pct;
}

static void sync_controls(void)
{
    SendDlgItemMessageW(D.hwnd, CD_SIZE, TBM_SETPOS, TRUE, D.cfg->font_pt);
    SendDlgItemMessageW(D.hwnd, CD_TOP, TBM_SETPOS, TRUE, D.cfg->text_opacity);
    SendDlgItemMessageW(D.hwnd, CD_BGOP, TBM_SETPOS, TRUE, D.cfg->bg_opacity);
    SendDlgItemMessageW(D.hwnd, CD_WIDTH, TBM_SETPOS, TRUE, D.cfg->width_pct);
    SendDlgItemMessageW(D.hwnd, CD_OUTLINE, BM_SETCHECK, D.cfg->outline ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* (custom), the built-in looks, then the user's own */
static void fill_looks(const wchar_t *select_name)
{
    HWND pc = GetDlgItem(D.hwnd, CD_PRESET);
    SendMessageW(pc, CB_RESETCONTENT, 0, 0);
    SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)L"(custom)");
    int np;
    const CaptionPreset *pr = caption_presets(&np);
    D.npresets = np;
    for (int i = 0; i < np; i++) SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)pr[i].name);
    int sel = 0;
    for (int i = 0; i < D.looks->n; i++) SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)D.looks->l[i].name);
    CaptionLook cur = look_of_cfg(D.cfg, NULL);
    int li = select_name ? looks_find(D.looks, select_name) : -1;
    if (li < 0) for (int i = 0; i < D.looks->n; i++) if (looks_same(&D.looks->l[i], &cur)) { li = i; break; }
    if (li >= 0) sel = 1 + np + li;
    else { int p = caption_preset_of(D.cfg); if (p >= 0) sel = p + 1; }
    SendMessageW(pc, CB_SETCURSEL, (WPARAM)sel, 0);
    EnableWindow(GetDlgItem(D.hwnd, CD_DELLOOK), sel > np);
}

static void save_look(void)
{
    wchar_t raw[80], name[LOOK_NAME];
    int cur = (int)SendDlgItemMessageW(D.hwnd, CD_PRESET, CB_GETCURSEL, 0, 0);
    raw[0] = 0;
    if (cur > D.npresets) wcsncpy(raw, D.looks->l[cur - 1 - D.npresets].name, 79);
    wchar_t init[80];
    wcscpy(init, raw);
    if (!input_box(D.hwnd, D.font, D.dpi, L"Save look", L"Name for this look (the font, size, colors, opacity and outline):", init, raw, 80)) return;
    if (!looks_clean_name(raw, name, LOOK_NAME)) {
        MessageBoxW(D.hwnd, L"That name cannot be used. Use 1 to 39 characters, without = signs.", L"Save look", MB_OK | MB_ICONWARNING);
        return;
    }
    int np;
    const CaptionPreset *pr = caption_presets(&np);
    for (int i = 0; i < np; i++)
        if (!_wcsicmp(pr[i].name, name)) {
            MessageBoxW(D.hwnd, L"That is the name of a built-in look. Choose another name.", L"Save look", MB_OK | MB_ICONWARNING);
            return;
        }
    int ex = looks_find(D.looks, name);
    if (ex >= 0 && MessageBoxW(D.hwnd, L"A look with this name exists. Replace it?", L"Save look", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (ex < 0 && D.looks->n >= LOOK_MAX) {
        MessageBoxW(D.hwnd, L"There is room for 24 saved looks. Delete one first.", L"Save look", MB_OK | MB_ICONWARNING);
        return;
    }
    CaptionLook l = look_of_cfg(D.cfg, name);
    looks_put(D.looks, &l);
    if (D.looks_changed) D.looks_changed(D.ctx);
    fill_looks(name);
}

static void delete_look(void)
{
    int cur = (int)SendDlgItemMessageW(D.hwnd, CD_PRESET, CB_GETCURSEL, 0, 0);
    int li = cur - 1 - D.npresets;
    if (li < 0 || li >= D.looks->n) return;
    wchar_t q[140];
    _snwprintf(q, 140, L"Delete the look \"%ls\"? The caption on the screen keeps its current appearance.", D.looks->l[li].name);
    q[139] = 0;
    if (MessageBoxW(D.hwnd, q, L"Delete look", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    looks_remove(D.looks, li);
    if (D.looks_changed) D.looks_changed(D.ctx);
    fill_looks(NULL);
}

static void draw_swatch(const DRAWITEMSTRUCT *di, unsigned col, const wchar_t *label)
{
    RECT r = di->rcItem;
    FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
    RECT sw = { r.left + 4, r.top + 4, r.left + 4 + (r.bottom - r.top - 8) * 2, r.bottom - 4 };
    HBRUSH b = CreateSolidBrush(col);
    FillRect(di->hDC, &sw, b);
    DeleteObject(b);
    FrameRect(di->hDC, &sw, (HBRUSH)GetStockObject(BLACK_BRUSH));
    RECT tr = { sw.right + 8, r.top, r.right - 4, r.bottom };
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(di->hDC, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &r);
    DrawEdge(di->hDC, &r, (di->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        D.hwnd = h;
        CaptionCfg *c = D.cfg;
        mk(L"BUTTON", L"&Show captions while reading", BS_AUTOCHECKBOX | WS_TABSTOP, 0, CD_ENABLE, 10, 8, 300, 22);
        SendDlgItemMessageW(h, CD_ENABLE, BM_SETCHECK, c->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        mk(L"STATIC", L"Look:", SS_LEFT, 0, 0, 10, 40, 60, 18);
        mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, CD_PRESET, 74, 36, 170, 240);
        mk(L"BUTTON", L"Sa&ve look...", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_SAVELOOK, 250, 34, 90, 28);
        mk(L"BUTTON", L"Dele&te look", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_DELLOOK, 344, 34, 66, 28);
        mk(L"STATIC", L"Font:", SS_LEFT, 0, 0, 10, 72, 60, 18);
        mk(L"STATIC", L"", SS_LEFT | SS_SUNKEN | SS_CENTERIMAGE, 0, CD_FONTNAME, 74, 68, 236, 24);
        mk(L"BUTTON", L"F&ont...", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_FONT, 318, 66, 92, 28);
        mk(L"STATIC", L"Text size:", SS_LEFT, 0, 0, 10, 106, 60, 18);
        slider(CD_SIZE, 12, 72, c->font_pt, 74, 102, 270);
        mk(L"STATIC", L"", SS_LEFT, 0, CD_SIZEVAL, 352, 106, 60, 18);
        mk(L"BUTTON", L"Text c&olor...", BS_OWNERDRAW | WS_TABSTOP, 0, CD_TEXTCOL, 10, 138, 196, 28);
        mk(L"BUTTON", L"&Background color...", BS_OWNERDRAW | WS_TABSTOP, 0, CD_BGCOL, 214, 138, 196, 28);
        mk(L"STATIC", L"Text opacity:", SS_LEFT, 0, 0, 10, 178, 80, 18);
        slider(CD_TOP, 20, 100, c->text_opacity, 94, 174, 250);
        mk(L"STATIC", L"", SS_LEFT, 0, CD_TOPVAL, 352, 178, 60, 18);
        mk(L"STATIC", L"Background:", SS_LEFT, 0, 0, 10, 208, 80, 18);
        slider(CD_BGOP, 0, 100, c->bg_opacity, 94, 204, 250);
        mk(L"STATIC", L"", SS_LEFT, 0, CD_BGOPVAL, 352, 208, 60, 18);
        mk(L"STATIC", L"Bar width:", SS_LEFT, 0, 0, 10, 238, 80, 18);
        slider(CD_WIDTH, 30, 95, c->width_pct, 94, 234, 250);
        mk(L"STATIC", L"", SS_LEFT, 0, CD_WIDTHVAL, 352, 238, 60, 18);
        mk(L"BUTTON", L"Outline the letters (helps with no background)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, CD_OUTLINE, 10, 268, 400, 22);
        SendDlgItemMessageW(h, CD_OUTLINE, BM_SETCHECK, c->outline ? BST_CHECKED : BST_UNCHECKED, 0);
        mk(L"BUTTON", L"Position", BS_GROUPBOX, 0, 0, 10, 296, 400, 90);
        mk(L"BUTTON", L"Bottom of the screen", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, CD_POS_BOTTOM, 22, 316, 170, 20);
        mk(L"BUTTON", L"Top of the screen", BS_AUTORADIOBUTTON, 0, CD_POS_TOP, 200, 316, 170, 20);
        mk(L"BUTTON", L"Where I put it", BS_AUTORADIOBUTTON, 0, CD_POS_CUSTOM, 22, 340, 170, 20);
        mk(L"BUTTON", L"&Move by dragging...", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_MOVE, 200, 336, 130, 28);
        mk(L"BUTTON", L"&Reset", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_RESETPOS, 336, 336, 64, 28);
        mk(L"BUTTON", L"Hide a few seconds after speech pauses or stops", BS_AUTOCHECKBOX | WS_TABSTOP, 0, CD_HIDE, 10, 394, 400, 22);
        SendDlgItemMessageW(h, CD_HIDE, BM_SETCHECK, c->hide_when_paused ? BST_CHECKED : BST_UNCHECKED, 0);
        mk(L"STATIC", L"The caption bar on screen changes as you adjust these. Click \"Move by dragging\", drag the bar anywhere "
                      L"(any monitor), then double-click it to lock it in place.", SS_LEFT, 0, 0, 10, 424, 400, 34);
        mk(L"BUTTON", L"&Defaults", BS_PUSHBUTTON | WS_TABSTOP, 0, CD_DEFAULTS, 10, 466, 90, 28);
        mk(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 320, 466, 90, 28);
        EnumChildWindows(h, font_cb, (LPARAM)D.font);
        fill_looks(NULL);
        sync_labels();
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlID == CD_TEXTCOL) { draw_swatch(di, D.cfg->text_color, L"Text color..."); return TRUE; }
        if (di->CtlID == CD_BGCOL) { draw_swatch(di, D.cfg->bg_color, L"Background color..."); return TRUE; }
        break;
    }
    case WM_HSCROLL: {
        HWND t = (HWND)lp;
        int v = (int)SendMessageW(t, TBM_GETPOS, 0, 0);
        int id = GetDlgCtrlID(t);
        if (id == CD_SIZE) D.cfg->font_pt = v;
        else if (id == CD_TOP) D.cfg->text_opacity = v;
        else if (id == CD_BGOP) D.cfg->bg_opacity = v;
        else if (id == CD_WIDTH) D.cfg->width_pct = v;
        sync_labels();
        changed();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CD_ENABLE: D.cfg->enabled = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; changed(); return 0;
        case CD_OUTLINE: D.cfg->outline = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; sync_labels(); changed(); return 0;
        case CD_HIDE: D.cfg->hide_when_paused = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; changed(); return 0;
        case CD_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (i > 0 && i <= D.npresets) { caption_apply_preset(D.cfg, i - 1); sync_controls(); sync_labels(); changed(); }
                else if (i > D.npresets && i - 1 - D.npresets < D.looks->n) {
                    CaptionLook l = D.looks->l[i - 1 - D.npresets];
                    apply_look(&l); sync_controls(); sync_labels(); changed();
                    SendMessageW((HWND)lp, CB_SETCURSEL, (WPARAM)i, 0);
                    EnableWindow(GetDlgItem(h, CD_DELLOOK), TRUE);
                }
                else EnableWindow(GetDlgItem(h, CD_DELLOOK), FALSE);
            }
            return 0;
        case CD_SAVELOOK: save_look(); return 0;
        case CD_DELLOOK: delete_look(); return 0;
        case CD_FONT: pick_font(); return 0;
        case CD_TEXTCOL: if (pick_color(&D.cfg->text_color)) { sync_labels(); changed(); } return 0;
        case CD_BGCOL: if (pick_color(&D.cfg->bg_color)) { sync_labels(); changed(); } return 0;
        case CD_POS_BOTTOM: case CD_POS_TOP: case CD_POS_CUSTOM:
            D.cfg->position = LOWORD(wp) - CD_POS_BOTTOM;
            if (D.cfg->position == 2 && !D.cfg->cx && !D.cfg->cy) {          /* nothing chosen yet: start from the middle of the screen */
                D.cfg->cx = GetSystemMetrics(SM_CXSCREEN) / 2; D.cfg->cy = GetSystemMetrics(SM_CYSCREEN) * 3 / 4;
            }
            changed();
            return 0;
        case CD_MOVE:
            if (!caption_moving()) { if (D.cfg->position != 2 && !D.cfg->cx && !D.cfg->cy) { D.cfg->cx = GetSystemMetrics(SM_CXSCREEN) / 2; D.cfg->cy = GetSystemMetrics(SM_CYSCREEN) * 3 / 4; } D.cfg->position = 2; changed(); caption_move_mode(1); }
            else caption_move_mode(0);
            sync_labels();
            return 0;
        case CD_RESETPOS: D.cfg->position = 0; D.cfg->cx = D.cfg->cy = 0; caption_move_mode(0); sync_labels(); changed(); return 0;
        case CD_DEFAULTS: {
            int en = D.cfg->enabled;
            caption_default(D.cfg);
            D.cfg->enabled = en;
            sync_controls();
            SendDlgItemMessageW(h, CD_HIDE, BM_SETCHECK, BST_CHECKED, 0);
            caption_move_mode(0);
            sync_labels();
            changed();
            return 0;
        }
        case IDCANCEL: D.done = 1; return 0;
        }
        break;
    case WM_CLOSE: D.done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int caption_dialog(HWND parent, HFONT font, int dpi, CaptionCfg *cfg, void (*apply)(const CaptionCfg *, void *), void *ctx,
                   CaptionLooks *looks, void (*looks_changed)(void *))
{
    memset(&D, 0, sizeof D);
    D.cfg = cfg; D.apply = apply; D.ctx = ctx; D.looks = looks; D.looks_changed = looks_changed; D.font = font; D.dpi = dpi;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceCaptionSettings", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceCaptionSettings";
        RegisterClassExW(&wc);
    }
    RECT r = { 0, 0, PS(420), PS(504) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    HWND hw;
    EnableWindow(parent, FALSE);
    caption_preview(1);
    hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceCaptionSettings", L"On-screen captions", style, x, y, w, h,
                         parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { caption_preview(0); EnableWindow(parent, TRUE); return 0; }
    ShowWindow(hw, SW_SHOW);
    MSG m;
    while (!D.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) { if (rr == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    caption_move_mode(0);
    caption_preview(0);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    return D.changed;
}
