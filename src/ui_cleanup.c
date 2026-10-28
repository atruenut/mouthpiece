/*
 * ui_cleanup.c - "Cleanup options and preview" dialog.
 *
 * Both panes are RichEdit controls (read-only) so that individual changes can
 * be coloured and so that selection changes are reported (EN_SELCHANGE).
 *
 * Offsets: RichEdit counts a paragraph break as ONE character. The ORIGINAL
 * text (CRLF) is therefore shown with "\r\n" -> "\r"; the positions of the
 * dropped '\n' are kept in D.rm[] so source offsets <-> pane offsets can be
 * converted exactly. The PROCESSED text only has '\n' (-> '\r', 1:1).
 *
 * Following: a caret/selection change in one pane is mapped through the
 * preprocessor's offset map to the other pane (EN_SELCHANGE). Scrolling is
 * mirrored by a light 60 ms timer that only exists while this dialog is open:
 * whichever pane has keyboard focus leads.
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
#include "ui_cleanup.h"

#define MAX_LISTED   20000
#define MAX_COLORED  4000          /* per group: removals/hyphens vs joins/unicode (RichEdit gets slow beyond this) */
#define SYNC_TIMER   1
enum { ID_CHK0 = 600, NCHK = 13, ID_LIST = 650, ID_ED_ORIG, ID_ED_PROC, ID_SUMMARY, ID_LEGEND0 = 660 };

/* Colours: foreground, background (same hue family in both panes). */
static const COLORREF C_REMOVED_FG = RGB(170, 0, 0),   C_REMOVED_BG = RGB(255, 222, 222);
static const COLORREF C_HYPHEN_FG  = RGB(110, 70, 0),  C_HYPHEN_BG  = RGB(255, 232, 170);
static const COLORREF C_JOIN_FG    = RGB(0, 50, 130),  C_JOIN_BG    = RGB(196, 222, 255);
static const COLORREF C_UNI_FG     = RGB(80, 0, 120),  C_UNI_BG     = RGB(232, 214, 252);

static struct {
    HWND hwnd, chk[NCHK], list, summary, lblO, lblP, edO, edP, legend[4], ok, cancel;
    HBRUSH legend_brush[4];
    HFONT font;
    HMODULE rich_dll;
    const wchar_t *rich_class;
    int dpi, done, result;
    int sync, syncing, in_refresh;
    int last_top[2];
    const wchar_t *src;
    size_t srclen;
    size_t *rm; int nrm;            /* source indices of '\n' that followed '\r' (dropped in the ORIGINAL pane) */
    PreprocOptions opts;
    PreprocResult res;
} D;

#define S(x) MulDiv((x), D.dpi, 96)

static const wchar_t *const LABELS[NCHK] = {
    L"Enable cleanup (off = speak the text as-is)",
    L"Remove repeated headers/footers",
    L"Remove page numbers (arabic and roman)",
    L"Join hard-wrapped lines",
    L"Fix hyphenated line breaks",
    L"Normalize whitespace",
    L"Normalize Unicode (ligatures, NBSP, zero-width)",
    L"Remove URLs",
    L"Remove [numeric] citations",
    L"Remove footnote markers and notes",
    L"Remove tables",
    L"Read tables row by row",
    L"Panes follow each other (cursor and scroll)",
};
static const wchar_t *const LEGEND[4] = { L"Removed", L"Hyphen repaired", L"Line joined", L"Unicode fix" };

static int *field(int i)
{
    int *f[NCHK] = { &D.opts.enabled, &D.opts.repeated_lines, &D.opts.page_numbers, &D.opts.join_lines,
                     &D.opts.fix_hyphens, &D.opts.normalize_ws, &D.opts.normalize_unicode,
                     &D.opts.remove_urls, &D.opts.remove_citations,
                     &D.opts.remove_footnotes, &D.opts.remove_tables, &D.opts.table_rows, &D.sync };
    return f[i];
}

/* ------------------------------------------------------------ offset helpers */

static size_t rich_from_src(size_t s)                     /* source offset -> ORIGINAL pane offset */
{
    int lo = 0, hi = D.nrm;                               /* count of rm[j] < s */
    while (lo < hi) { int mid = (lo + hi) / 2; if (D.rm[mid] < s) lo = mid + 1; else hi = mid; }
    return s - (size_t)lo;
}

static size_t src_from_rich(size_t r)                     /* ORIGINAL pane offset -> source offset */
{
    int lo = 0, hi = D.nrm;                               /* count of j with rm[j]-j <= r */
    while (lo < hi) { int mid = (lo + hi) / 2; if (D.rm[mid] - (size_t)mid <= r) lo = mid + 1; else hi = mid; }
    return r + (size_t)lo;
}

/* ORIGINAL pane text (and the rm[] table). Caller frees. */
static wchar_t *build_original(void)
{
    wchar_t *o = (wchar_t *)malloc((D.srclen + 1) * sizeof(wchar_t));
    D.rm = (size_t *)malloc((D.srclen / 2 + 1) * sizeof(size_t));
    if (!o || !D.rm) { free(o); free(D.rm); D.rm = NULL; return NULL; }
    size_t j = 0;
    D.nrm = 0;
    for (size_t i = 0; i < D.srclen; i++) {
        wchar_t c = D.src[i];
        if (c == L'\n') {
            if (i > 0 && D.src[i - 1] == L'\r') { D.rm[D.nrm++] = i; continue; }
            c = L'\r';
        }
        o[j++] = c;
    }
    o[j] = 0;
    return o;
}

static wchar_t *build_processed(void)
{
    wchar_t *o = (wchar_t *)malloc((D.res.len + 1) * sizeof(wchar_t));
    if (!o) return NULL;
    for (size_t i = 0; i < D.res.len; i++) o[i] = D.res.text[i] == L'\n' ? L'\r' : D.res.text[i];
    o[D.res.len] = 0;
    return o;
}

/* ------------------------------------------------------------ rich helpers */

static void set_sel(HWND ed, size_t a, size_t b)
{
    CHARRANGE cr = { (LONG)a, (LONG)b };
    SendMessageW(ed, EM_EXSETSEL, 0, (LPARAM)&cr);
}

static void paint(HWND ed, size_t a, size_t b, COLORREF fg, COLORREF bg, int strike)
{
    if (b <= a) return;
    set_sel(ed, a, b);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_STRIKEOUT;
    cf.dwEffects = strike ? CFE_STRIKEOUT : 0;
    cf.crTextColor = fg;
    cf.crBackColor = bg;
    SendMessageW(ed, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void reset_format(HWND ed)
{
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_STRIKEOUT;
    cf.dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
    SendMessageW(ed, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

/* Put `off` about 4 lines below the top of the pane and select [a,b). */
static void show_at(HWND ed, size_t a, size_t b)
{
    set_sel(ed, a, b);
    int line = (int)SendMessageW(ed, EM_LINEFROMCHAR, (WPARAM)a, 0);
    int top = (int)SendMessageW(ed, EM_GETFIRSTVISIBLELINE, 0, 0);
    int want = line > 4 ? line - 4 : 0;
    if (want != top) SendMessageW(ed, EM_LINESCROLL, 0, (LPARAM)(want - top));
}

/* ------------------------------------------------------------ coloring */

static void color_changes(void)
{
    reset_format(D.edO);
    reset_format(D.edP);
    int shown = 0, capped = 0;
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        for (int i = 0; i < D.res.nchanges; i++) {
            const Change *c = &D.res.changes[i];
            int minor = (c->kind == CH_JOIN || c->kind == CH_UNICODE);
            if (minor != pass) continue;
            if (n >= MAX_COLORED) { capped = 1; break; }
            n++; shown++;
            size_t a = rich_from_src(c->src_start), b = rich_from_src(c->src_start + c->src_len);
            size_t pa = preproc_src_to_norm(&D.res, c->src_start);
            size_t pb = preproc_src_to_norm(&D.res, c->src_start + c->src_len);
            switch (c->kind) {
            case CH_HEADER: case CH_PAGENUM: case CH_URL: case CH_CITATION: case CH_FOOTNOTE: case CH_TABLE:
                paint(D.edO, a, b, C_REMOVED_FG, C_REMOVED_BG, 1);
                break;
            case CH_HYPHEN:
                paint(D.edO, a, b, C_HYPHEN_FG, C_HYPHEN_BG, 0);
                paint(D.edP, pa, pb, C_HYPHEN_FG, C_HYPHEN_BG, 0);
                break;
            case CH_JOIN:
                paint(D.edO, a, b, C_JOIN_FG, C_JOIN_BG, 0);
                paint(D.edP, pa, pb, C_JOIN_FG, C_JOIN_BG, 0);
                break;
            case CH_UNICODE:
                paint(D.edO, a, b, C_UNI_FG, C_UNI_BG, 0);
                paint(D.edP, pa, pb, C_UNI_FG, C_UNI_BG, 0);
                break;
            }
        }
    }
    (void)shown;
    D.last_top[0] = D.last_top[1] = -1;
    if (capped) {
        wchar_t b[480];
        GetWindowTextW(D.summary, b, 400);
        wcsncat(b, L"   (Colouring is limited to the first 4000 changes per group.)", 479 - wcslen(b));
        SetWindowTextW(D.summary, b);
    }
}

/* ------------------------------------------------------------ list + summary */

static const wchar_t *kind_name(int k)
{
    switch (k) {
    case CH_HEADER:   return L"Header ";
    case CH_PAGENUM:  return L"Page # ";
    case CH_HYPHEN:   return L"Hyphen ";
    case CH_URL:      return L"URL    ";
    case CH_CITATION: return L"Cite   ";
    case CH_FOOTNOTE: return L"Footnt ";
    case CH_TABLE:    return L"Table  ";
    case CH_UNICODE:  return L"Unicode";
    }
    return L"?";
}

static void fill_list(void)
{
    SendMessageW(D.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(D.list, LB_RESETCONTENT, 0, 0);
    size_t line = 1, pos = 0;
    int shown = 0, total = 0;
    for (int i = 0; i < D.res.nchanges; i++) {
        const Change *c = &D.res.changes[i];
        if (c->kind == CH_JOIN) continue;                         /* thousands of these: shown in colour only */
        total++;
        if (shown >= MAX_LISTED) continue;
        for (; pos < c->src_start && pos < D.srclen; pos++)
            if (D.src[pos] == L'\n') line++;
        wchar_t item[160], snip[72];
        size_t n = c->src_len < 60 ? c->src_len : 60, k = 0;
        for (size_t j = 0; j < n; j++) {
            wchar_t ch = D.src[c->src_start + j];
            if (ch == L'\r') continue;
            snip[k++] = (ch == L'\n') ? L'/' : ch;
        }
        snip[k] = 0;
        _snwprintf(item, 160, L"%ls L%-5u %ls", kind_name(c->kind), (unsigned)line, snip);
        item[159] = 0;
        int idx = (int)SendMessageW(D.list, LB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(D.list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        shown++;
    }
    if (total > shown) {
        wchar_t more[64];
        _snwprintf(more, 64, L"... and %d more", total - shown);
        more[63] = 0;
        int idx = (int)SendMessageW(D.list, LB_ADDSTRING, 0, (LPARAM)more);
        SendMessageW(D.list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
    }
    SendMessageW(D.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(D.list, NULL, TRUE);
}

static void refresh(void)
{
    D.in_refresh = 1;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    preproc_free(&D.res);
    if (!preproc_run(D.src, D.srclen, &D.opts, 1, &D.res)) {
        SetWindowTextW(D.summary, L"Out of memory.");
        D.in_refresh = 0;
        SetCursor(old);
        return;
    }
    SendMessageW(D.edO, WM_SETREDRAW, FALSE, 0);
    SendMessageW(D.edP, WM_SETREDRAW, FALSE, 0);
    wchar_t *pt = build_processed();
    SetWindowTextW(D.edP, pt ? pt : L"");
    free(pt);

    wchar_t b[560];
    _snwprintf(b, 560,
        L"Removed: %u headers/footers, %u page numbers, %u URLs, %u citations, %u footnote parts, %u table lines.   Hyphens repaired: %u.   "
        L"Line breaks joined: %u.   Unicode fixes: %u.   Hard-wrapped text: %ls",
        (unsigned)D.res.n_headers, (unsigned)D.res.n_pagenums, (unsigned)D.res.n_urls,
        (unsigned)D.res.n_citations, (unsigned)D.res.n_footnotes, (unsigned)D.res.n_tables, (unsigned)D.res.n_hyphens, (unsigned)D.res.n_joined,
        (unsigned)D.res.n_unicode, D.res.hard_wrapped ? L"yes" : L"no");
    b[559] = 0;
    SetWindowTextW(D.summary, b);
    fill_list();
    color_changes();
    set_sel(D.edO, 0, 0);
    set_sel(D.edP, 0, 0);
    SendMessageW(D.edO, WM_SETREDRAW, TRUE, 0);
    SendMessageW(D.edP, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(D.edO, NULL, TRUE);
    InvalidateRect(D.edP, NULL, TRUE);
    SetCursor(old);
    D.in_refresh = 0;
}

/* ------------------------------------------------------------ following */

static void sync_selection(HWND from, CHARRANGE cr)
{
    if (!D.sync || D.syncing || D.in_refresh) return;
    D.syncing = 1;
    if (from == D.edO) {
        size_t s0 = src_from_rich((size_t)cr.cpMin), s1 = src_from_rich((size_t)cr.cpMax);
        size_t n0 = preproc_src_to_norm(&D.res, s0);
        size_t n1 = cr.cpMax > cr.cpMin ? preproc_src_to_norm(&D.res, s1) : n0;
        set_sel(D.edP, n0, n1);
        SendMessageW(D.edP, EM_SCROLLCARET, 0, 0);
    } else if (from == D.edP) {
        size_t n0 = (size_t)cr.cpMin, n1 = (size_t)cr.cpMax;
        size_t s0 = preproc_norm_to_src(&D.res, n0);
        size_t s1 = n1 > n0 ? preproc_norm_end_to_src(&D.res, n1) : s0;
        set_sel(D.edO, rich_from_src(s0), rich_from_src(s1));
        SendMessageW(D.edO, EM_SCROLLCARET, 0, 0);
    }
    D.syncing = 0;
}

static void sync_scroll_from(HWND from)
{
    HWND to = (from == D.edO) ? D.edP : D.edO;
    int top = (int)SendMessageW(from, EM_GETFIRSTVISIBLELINE, 0, 0);
    size_t ch = (size_t)SendMessageW(from, EM_LINEINDEX, (WPARAM)top, 0);
    size_t target;
    if (from == D.edO) target = preproc_src_to_norm(&D.res, src_from_rich(ch));
    else target = rich_from_src(preproc_norm_to_src(&D.res, ch));
    int line = (int)SendMessageW(to, EM_LINEFROMCHAR, (WPARAM)target, 0);
    int cur = (int)SendMessageW(to, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (line != cur) SendMessageW(to, EM_LINESCROLL, 0, (LPARAM)(line - cur));
}

static void on_sync_timer(void)
{
    if (!D.sync || D.in_refresh || D.syncing) return;
    HWND f = GetFocus();
    int idx = (f == D.edO) ? 0 : (f == D.edP) ? 1 : -1;
    if (idx < 0) return;
    int top = (int)SendMessageW(f, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (top == D.last_top[idx]) return;
    D.syncing = 1;
    sync_scroll_from(f);
    D.syncing = 0;
    D.last_top[idx] = top;
    D.last_top[1 - idx] = (int)SendMessageW(idx == 0 ? D.edP : D.edO, EM_GETFIRSTVISIBLELINE, 0, 0);
}

static void on_pick(void)
{
    int sel = (int)SendMessageW(D.list, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    int ci = (int)SendMessageW(D.list, LB_GETITEMDATA, (WPARAM)sel, 0);
    if (ci < 0 || ci >= D.res.nchanges) return;
    const Change *c = &D.res.changes[ci];
    D.syncing = 1;
    show_at(D.edO, rich_from_src(c->src_start), rich_from_src(c->src_start + c->src_len));
    size_t n0 = preproc_src_to_norm(&D.res, c->src_start);
    size_t n1 = preproc_src_to_norm(&D.res, c->src_start + c->src_len);
    show_at(D.edP, n0, n1);
    D.syncing = 0;
    D.last_top[0] = D.last_top[1] = -1;
}

/* ------------------------------------------------------------ window */

static void layout(int w, int h)
{
    int m = S(8), colw = (w - 2 * m) / 3, rowh = S(22), y = m;
    for (int i = 0; i < NCHK; i++)
        MoveWindow(D.chk[i], m + (i % 3) * colw, y + (i / 3) * rowh, colw - S(4), rowh, TRUE);
    y += 5 * rowh + S(4);
    MoveWindow(D.summary, m, y, w - 2 * m, S(34), TRUE);
    y += S(38);
    int bottom = h - m - S(28) - m;
    int listw = (w - 2 * m) * 25 / 100, gap = S(6);
    int paneW = (w - 2 * m - listw - 2 * gap) / 2;
    MoveWindow(D.list, m, y, listw, bottom - y, TRUE);
    int x = m + listw + gap;
    MoveWindow(D.lblO, x, y, paneW, S(16), TRUE);
    MoveWindow(D.edO, x, y + S(18), paneW, bottom - y - S(18), TRUE);
    x += paneW + gap;
    MoveWindow(D.lblP, x, y, paneW, S(16), TRUE);
    MoveWindow(D.edP, x, y + S(18), paneW, bottom - y - S(18), TRUE);
    int lx = m;
    for (int i = 0; i < 4; i++) { MoveWindow(D.legend[i], lx, h - m - S(26), S(112), S(24), TRUE); lx += S(116); }
    MoveWindow(D.ok, w - m - S(150) - S(90) - S(8), h - m - S(28), S(150), S(28), TRUE);
    MoveWindow(D.cancel, w - m - S(90), h - m - S(28), S(90), S(28), TRUE);
}

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, D.hwnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        D.hwnd = hwnd;
        for (int i = 0; i < NCHK; i++) {
            D.chk[i] = mk(L"BUTTON", LABELS[i], BS_AUTOCHECKBOX | WS_TABSTOP, 0, ID_CHK0 + i);
            SendMessageW(D.chk[i], BM_SETCHECK, *field(i) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        D.summary = mk(L"STATIC", L"", SS_LEFT, 0, ID_SUMMARY);
        D.list = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_LIST);
        D.lblO = mk(L"STATIC", L"ORIGINAL (never modified)", SS_LEFT, 0, 0);
        D.lblP = mk(L"STATIC", L"PROCESSED (what is spoken)", SS_LEFT, 0, 0);
        DWORD ro = ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL | WS_TABSTOP;
        D.edO = mk(D.rich_class, L"", ro, WS_EX_CLIENTEDGE, ID_ED_ORIG);
        D.edP = mk(D.rich_class, L"", ro, WS_EX_CLIENTEDGE, ID_ED_PROC);
        for (int i = 0; i < 4; i++) D.legend[i] = mk(L"STATIC", LEGEND[i], SS_CENTER | SS_CENTERIMAGE, 0, ID_LEGEND0 + i);
        D.ok = mk(L"BUTTON", L"&Use these settings", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK);
        D.cancel = mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL);
        EnumChildWindows(hwnd, font_cb, (LPARAM)D.font);
        HWND eds[2] = { D.edO, D.edP };
        for (int i = 0; i < 2; i++) {
            SendMessageW(eds[i], EM_SETEVENTMASK, 0, ENM_SELCHANGE);
            SendMessageW(eds[i], EM_SETUNDOLIMIT, 0, 0);
            SendMessageW(eds[i], EM_EXLIMITTEXT, 0, 0x7FFFFFF0);
        }
        wchar_t *ot = build_original();
        D.in_refresh = 1;
        SetWindowTextW(D.edO, ot ? ot : L"");
        free(ot);
        D.in_refresh = 0;
        SetTimer(hwnd, SYNC_TIMER, 60, NULL);
        return 0;
    }
    case WM_SIZE:
        if (D.ok) layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(820);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(500);
        return 0;
    case WM_TIMER:
        if (wp == SYNC_TIMER) on_sync_timer();
        return 0;
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)lp) - ID_LEGEND0;
        if (id >= 0 && id < 4) {
            static const COLORREF fg[4] = { RGB(170, 0, 0), RGB(110, 70, 0), RGB(0, 50, 130), RGB(80, 0, 120) };
            static const COLORREF bg[4] = { RGB(255, 222, 222), RGB(255, 232, 170), RGB(196, 222, 255), RGB(232, 214, 252) };
            if (!D.legend_brush[id]) D.legend_brush[id] = CreateSolidBrush(bg[id]);
            SetTextColor((HDC)wp, fg[id]);
            SetBkColor((HDC)wp, bg[id]);
            return (LRESULT)D.legend_brush[id];
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->code == EN_SELCHANGE && (h->hwndFrom == D.edO || h->hwndFrom == D.edP))
            sync_selection(h->hwndFrom, ((SELCHANGE *)lp)->chrg);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_CHK0 && id < ID_CHK0 + NCHK && HIWORD(wp) == BN_CLICKED) {
            int i = id - ID_CHK0;
            *field(i) = SendMessageW(D.chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (i != NCHK - 1) refresh();                 /* the "follow" switch needs no re-run */
        } else if (id == ID_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            on_pick();
        } else if (id == IDOK) {
            D.result = 1; D.done = 1;
        } else if (id == IDCANCEL) {
            D.result = 0; D.done = 1;
        }
        return 0;
    }
    case WM_CLOSE:
        D.result = 0; D.done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int cleanup_dialog(HWND parent, HFONT font, int dpi, const wchar_t *src, size_t srclen, PreprocOptions *opts)
{
    static int registered;
    memset(&D, 0, sizeof D);
    D.font = font; D.dpi = dpi; D.src = src; D.srclen = srclen; D.opts = *opts;
    D.sync = 1;

    D.rich_dll = LoadLibraryW(L"Msftedit.dll");
    D.rich_class = L"RICHEDIT50W";
    if (!D.rich_dll) {
        D.rich_dll = LoadLibraryW(L"Riched20.dll");
        D.rich_class = L"RichEdit20W";
    }
    if (!D.rich_dll) {
        MessageBoxW(parent, L"The Windows RichEdit library could not be loaded.", L"Mouthpiece", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceCleanup";
        if (!RegisterClassExW(&wc)) { FreeLibrary(D.rich_dll); return 0; }
        registered = 1;
    }
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = S(1100), h = S(700);
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceCleanup", L"Cleanup options and preview",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                              x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); FreeLibrary(D.rich_dll); return 0; }
    RECT cr;
    GetClientRect(hw, &cr);
    layout(cr.right, cr.bottom);
    ShowWindow(hw, SW_SHOW);
    refresh();
    SetFocus(D.chk[0]);

    MSG m;
    while (!D.done) {
        int r = GetMessageW(&m, NULL, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)m.wParam); break; }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    KillTimer(hw, SYNC_TIMER);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
    for (int i = 0; i < 4; i++) if (D.legend_brush[i]) DeleteObject(D.legend_brush[i]);
    preproc_free(&D.res);
    free(D.rm);
    FreeLibrary(D.rich_dll);
    if (D.result) *opts = D.opts;
    return D.result;
}
