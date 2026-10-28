#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "ui_caption.h"
#include "navmath.h"
#include "ini.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CLASS_NAME L"MouthpieceCaption"
#define T_PAGE 1
#define T_HIDE 2
#define T_NOTICE 3
#define MAX_CHUNKS 64

static struct {
    HWND hwnd;
    CaptionCfg cfg;
    wchar_t *text; size_t n;
    size_t st[MAX_CHUNKS], ln[MAX_CHUNKS]; int nchunks, chunk;
    int synced;           /* pages follow the voice's real position (caption_progress), not the timer */
    double seconds;
    wchar_t notice[300]; int has_notice;
    int moving, preview;
    void (*moved)(const CaptionCfg *, void *); void *moved_ctx;
} C;

/* ---------------------------------------------------------------- settings */

static const CaptionPreset PRESETS[] = {
    { L"White on black (classic captions)", RGB(255, 255, 255), RGB(0, 0, 0), 85, 0 },
    { L"Yellow on black (high contrast)", RGB(255, 235, 59), RGB(0, 0, 0), 90, 0 },
    { L"Black on white", RGB(0, 0, 0), RGB(255, 255, 255), 90, 0 },
    { L"Black on yellow", RGB(0, 0, 0), RGB(255, 235, 59), 90, 0 },
    { L"White on dark blue", RGB(255, 255, 255), RGB(16, 32, 80), 85, 0 },
    { L"White with outline, no background", RGB(255, 255, 255), RGB(0, 0, 0), 0, 1 },
    { L"Soft gray on dark, low glare", RGB(210, 210, 210), RGB(30, 30, 30), 70, 0 },
};
const CaptionPreset *caption_presets(int *n) { *n = (int)(sizeof PRESETS / sizeof PRESETS[0]); return PRESETS; }

int caption_preset_of(const CaptionCfg *c)
{
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++)
        if (PRESETS[i].text == c->text_color && PRESETS[i].bg == c->bg_color && PRESETS[i].bg_opacity == c->bg_opacity && PRESETS[i].outline == c->outline) return (int)i;
    return -1;
}

void caption_apply_preset(CaptionCfg *c, int i)
{
    if (i < 0 || i >= (int)(sizeof PRESETS / sizeof PRESETS[0])) return;
    c->text_color = PRESETS[i].text; c->bg_color = PRESETS[i].bg; c->bg_opacity = PRESETS[i].bg_opacity; c->outline = PRESETS[i].outline;
}

void caption_default(CaptionCfg *c)
{
    memset(c, 0, sizeof *c);
    wcscpy(c->face, L"Segoe UI");
    c->font_pt = 24; c->bold = 1;
    c->text_color = RGB(255, 255, 255); c->bg_color = RGB(0, 0, 0);
    c->text_opacity = 100; c->bg_opacity = 85; c->outline = 0;
    c->position = 0; c->width_pct = 70; c->hide_when_paused = 1;
}

static int clampi(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

void caption_cfg_from_ini(CaptionCfg *c, const void *ini)
{
    const Ini *d = (const Ini *)ini;
    caption_default(c);
    c->enabled = ini_get_int(d, L"Captions", L"Enabled", 0) != 0;
    if (ini_get_int(d, L"Captions", L"FontPt", 24) < 12) return;      /* a size that cannot be chosen: an empty save, use the defaults */
    const wchar_t *f = ini_get(d, L"Captions", L"Font");
    if (f && *f) { wcsncpy(c->face, f, 31); c->face[31] = 0; }
    c->font_pt = clampi(ini_get_int(d, L"Captions", L"FontPt", 24), 12, 72);
    c->bold = ini_get_int(d, L"Captions", L"Bold", 1) != 0;
    c->text_color = (unsigned)ini_get_int(d, L"Captions", L"TextColor", (long)RGB(255, 255, 255)) & 0xFFFFFF;
    c->bg_color = (unsigned)ini_get_int(d, L"Captions", L"BackColor", 0) & 0xFFFFFF;
    c->text_opacity = clampi(ini_get_int(d, L"Captions", L"TextOpacity", 100), 20, 100);
    c->bg_opacity = clampi(ini_get_int(d, L"Captions", L"Opacity", 85), 0, 100);
    c->outline = ini_get_int(d, L"Captions", L"Outline", 0) != 0;
    c->position = clampi(ini_get_int(d, L"Captions", L"Position", ini_get_int(d, L"Captions", L"Top", 0) ? 1 : 0), 0, 2);
    c->cx = (int)ini_get_int(d, L"Captions", L"CenterX", 0);
    c->cy = (int)ini_get_int(d, L"Captions", L"CenterY", 0);
    c->width_pct = clampi(ini_get_int(d, L"Captions", L"WidthPercent", 70), 30, 95);
    c->hide_when_paused = ini_get_int(d, L"Captions", L"HideWhenPaused", 1) != 0;
}

void caption_cfg_to_ini(const CaptionCfg *c, void *ini)
{
    Ini *d = (Ini *)ini;
    ini_set_int(d, L"Captions", L"Enabled", c->enabled);
    ini_set(d, L"Captions", L"Font", c->face);
    ini_set_int(d, L"Captions", L"FontPt", c->font_pt);
    ini_set_int(d, L"Captions", L"Bold", c->bold);
    ini_set_int(d, L"Captions", L"TextColor", (long)c->text_color);
    ini_set_int(d, L"Captions", L"BackColor", (long)c->bg_color);
    ini_set_int(d, L"Captions", L"TextOpacity", c->text_opacity);
    ini_set_int(d, L"Captions", L"Opacity", c->bg_opacity);
    ini_set_int(d, L"Captions", L"Outline", c->outline);
    ini_set_int(d, L"Captions", L"Position", c->position);
    ini_set_int(d, L"Captions", L"CenterX", c->cx);
    ini_set_int(d, L"Captions", L"CenterY", c->cy);
    ini_set_int(d, L"Captions", L"WidthPercent", c->width_pct);
    ini_set_int(d, L"Captions", L"HideWhenPaused", c->hide_when_paused);
}

/* ---------------------------------------------------------------- drawing */

static const wchar_t *current_text(size_t *n)
{
    if (C.has_notice) { *n = wcslen(C.notice); return C.notice; }
    if (C.preview || (C.moving && (!C.text || !C.nchunks))) {
        static const wchar_t *sample = L"This is how the captions look. Drag or double-click to finish.";
        const wchar_t *s = C.moving ? L"Drag me anywhere on the screen. Double-click to lock the position." : L"This is how the captions will look while Mouthpiece reads to you.";
        (void)sample;
        *n = wcslen(s);
        return s;
    }
    if (!C.text || !C.nchunks) { *n = 0; return L""; }
    *n = C.ln[C.chunk];
    return C.text + C.st[C.chunk];
}

static HFONT make_font(HDC dc)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(C.cfg.font_pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfWeight = C.cfg.bold ? FW_BOLD : FW_NORMAL;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, C.cfg.face[0] ? C.cfg.face : L"Segoe UI", 31);
    return CreateFontIndirectW(&lf);
}

static unsigned char mul255(int a, int b) { return (unsigned char)((a * b + 127) / 255); }

/* Render the bar into a premultiplied 32-bit bitmap and put it on the screen at (x, y). */
static void render(int x, int y, int w, int h, const wchar_t *t, size_t n, HFONT font, int pad)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(NULL);
    HDC cov_dc = CreateCompatibleDC(sdc), out_dc = CreateCompatibleDC(sdc);
    unsigned char *cov = NULL, *px = NULL;
    HBITMAP cov_bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&cov, NULL, 0);
    HBITMAP out_bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!cov_bmp || !out_bmp || !cov || !px) {
        if (cov_bmp) DeleteObject(cov_bmp);
        if (out_bmp) DeleteObject(out_bmp);
        DeleteDC(cov_dc); DeleteDC(out_dc); ReleaseDC(NULL, sdc);
        return;
    }
    HGDIOBJ oc = SelectObject(cov_dc, cov_bmp), oo = SelectObject(out_dc, out_bmp);
    memset(cov, 0, (size_t)w * h * 4);
    /* coverage of the text: white on black */
    RECT r = { pad, pad, w - pad, h - pad };
    SetBkMode(cov_dc, TRANSPARENT);
    SetTextColor(cov_dc, RGB(255, 255, 255));
    HGDIOBJ of = SelectObject(cov_dc, font);
    DrawTextW(cov_dc, t, (int)n, &r, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(cov_dc, of);
    GdiFlush();

    int bgA = C.cfg.bg_opacity * 255 / 100;
    if (C.moving && bgA < 110) bgA = 110;                         /* something to grab while positioning */
    int br = GetRValue(C.cfg.bg_color), bgc = GetGValue(C.cfg.bg_color), bb = GetBValue(C.cfg.bg_color);
    int tr = GetRValue(C.cfg.text_color), tg = GetGValue(C.cfg.text_color), tb = GetBValue(C.cfg.text_color);
    int ta = C.cfg.text_opacity * 255 / 100;
    int lum = (tr * 30 + tg * 59 + tb * 11) / 100;
    int orr = lum > 128 ? 0 : 255, og = orr, ob = orr;               /* outline contrasts with the text */
    int rad = pad * 3 / 4;
    int orad = C.cfg.font_pt >= 30 ? 2 : 1;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            unsigned char *d = px + ((size_t)yy * w + xx) * 4;
            /* rounded rectangle background */
            int inside = 1;
            int cx_ = xx < rad ? rad - xx : xx >= w - rad ? xx - (w - rad - 1) : 0;
            int cy_ = yy < rad ? rad - yy : yy >= h - rad ? yy - (h - rad - 1) : 0;
            if (cx_ && cy_ && cx_ * cx_ + cy_ * cy_ > rad * rad) inside = 0;
            int A = inside ? bgA : 0;
            int R = mul255(br, A), G = mul255(bgc, A), B = mul255(bb, A);
            if (C.cfg.outline) {
                int oc2 = 0;
                for (int dy = -orad; dy <= orad && oc2 < 255; dy++) {
                    int y2 = yy + dy;
                    if (y2 < 0 || y2 >= h) continue;
                    for (int dx = -orad; dx <= orad; dx++) {
                        int x2 = xx + dx;
                        if (x2 < 0 || x2 >= w) continue;
                        int v = cov[((size_t)y2 * w + x2) * 4 + 1];
                        if (v > oc2) oc2 = v;
                    }
                }
                int sa = mul255(oc2, ta);
                R = mul255(orr, sa) + mul255(R, 255 - sa); G = mul255(og, sa) + mul255(G, 255 - sa); B = mul255(ob, sa) + mul255(B, 255 - sa);
                A = sa + mul255(A, 255 - sa);
            }
            int c = cov[((size_t)yy * w + xx) * 4 + 1];
            if (c) {
                int sa = mul255(c, ta);
                R = mul255(tr, sa) + mul255(R, 255 - sa); G = mul255(tg, sa) + mul255(G, 255 - sa); B = mul255(tb, sa) + mul255(B, 255 - sa);
                A = sa + mul255(A, 255 - sa);
            }
            d[0] = (unsigned char)B; d[1] = (unsigned char)G; d[2] = (unsigned char)R; d[3] = (unsigned char)A;
        }
    }
    if (C.moving) {                                                  /* dashed border while positioning */
        for (int xx = 0; xx < w; xx++) for (int k = 0; k < 2; k++) {
            if ((xx / 6) % 2) continue;
            unsigned char *a = px + ((size_t)k * w + xx) * 4, *b = px + ((size_t)(h - 1 - k) * w + xx) * 4;
            a[0] = a[1] = a[2] = a[3] = 255; b[0] = b[1] = b[2] = b[3] = 255;
        }
    }
    POINT dst = { x, y }, src = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(C.hwnd, sdc, &dst, &sz, out_dc, &src, 0, &bf, ULW_ALPHA);
    SelectObject(cov_dc, oc); SelectObject(out_dc, oo);
    DeleteObject(cov_bmp); DeleteObject(out_bmp);
    DeleteDC(cov_dc); DeleteDC(out_dc);
    ReleaseDC(NULL, sdc);
}

static void relayout(void)
{
    if (!C.hwnd) return;
    size_t n;
    const wchar_t *t = current_text(&n);
    if (!n) { ShowWindow(C.hwnd, SW_HIDE); return; }
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    HMONITOR mon;
    if (C.cfg.position == 2 && (C.cfg.cx || C.cfg.cy)) { POINT pt = { C.cfg.cx, C.cfg.cy }; mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); }
    else { HWND fg = GetForegroundWindow(); mon = MonitorFromWindow(fg ? fg : C.hwnd, MONITOR_DEFAULTTOPRIMARY); }
    GetMonitorInfoW(mon, &mi);
    int sw = mi.rcWork.right - mi.rcWork.left;
    int w = sw * C.cfg.width_pct / 100;
    HDC dc = GetDC(NULL);
    HFONT font = make_font(dc);
    HGDIOBJ old = SelectObject(dc, font);
    int pad = MulDiv(14, GetDeviceCaps(dc, LOGPIXELSX), 96) + C.cfg.font_pt / 4;
    RECT r = { 0, 0, w - 2 * pad, 0 };
    DrawTextW(dc, t, (int)n, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    int margin = MulDiv(36, GetDeviceCaps(dc, LOGPIXELSY), 96);
    ReleaseDC(NULL, dc);
    int h = (r.bottom - r.top) + 2 * pad;
    int x, y;
    if (C.cfg.position == 2 && (C.cfg.cx || C.cfg.cy)) { x = C.cfg.cx - w / 2; y = C.cfg.cy - h / 2; }
    else { x = mi.rcWork.left + (sw - w) / 2; y = C.cfg.position == 1 ? mi.rcWork.top + margin : mi.rcWork.bottom - h - margin; }
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (!IsWindowVisible(C.hwnd)) ShowWindow(C.hwnd, SW_SHOWNA);
    SetWindowPos(C.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    render(x, y, w, h, t, n, font, pad);
    DeleteObject(font);
}

static void start_paging(void)
{
    KillTimer(C.hwnd, T_PAGE);
    if (C.synced) return;
    if (C.nchunks > 1 && C.seconds > 0) {
        size_t total = 0;
        for (int i = 0; i < C.nchunks; i++) total += C.ln[i];
        int ms = (int)(0.88 * C.seconds * 1000.0 * (double)C.ln[C.chunk] / (double)(total ? total : 1));   /* a little fast: late pages are worse than early ones */
        SetTimer(C.hwnd, T_PAGE, (UINT)(ms < 400 ? 400 : ms), NULL);
    }
}

static void notify_moved(void)
{
    if (C.moved) C.moved(&C.cfg, C.moved_ctx);
}

static void set_click_through(int on)
{
    LONG_PTR ex = GetWindowLongPtrW(C.hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT; else ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    SetWindowLongPtrW(C.hwnd, GWL_EXSTYLE, ex);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: return C.moving ? HTCAPTION : HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCLBUTTONDBLCLK: if (C.moving) { caption_move_mode(0); return 0; } break;
    case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(h, &r);
        C.cfg.position = 2;
        C.cfg.cx = (r.left + r.right) / 2;
        C.cfg.cy = (r.top + r.bottom) / 2;
        notify_moved();
        return 0;
    }
    case WM_TIMER:
        if (wp == T_PAGE) {
            KillTimer(h, T_PAGE);
            if (C.chunk + 1 < C.nchunks) { C.chunk++; if (!C.has_notice) relayout(); start_paging(); }
        } else if (wp == T_HIDE) {
            KillTimer(h, T_HIDE);
            if (!C.has_notice && !C.moving && !C.preview) ShowWindow(h, SW_HIDE);
        } else if (wp == T_NOTICE) {
            KillTimer(h, T_NOTICE);
            C.has_notice = 0;
            if (C.moving || C.preview || (C.cfg.enabled && C.text)) relayout(); else ShowWindow(h, SW_HIDE);
        }
        return 0;
    case WM_DISPLAYCHANGE: relayout(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------------------------------------------------------- public API */

int caption_init(HINSTANCE inst)
{
    caption_default(&C.cfg);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
    C.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             CLASS_NAME, L"Mouthpiece caption", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    return C.hwnd != NULL;
}

void caption_shutdown(void)
{
    if (C.hwnd) DestroyWindow(C.hwnd);
    free(C.text);
    memset(&C, 0, sizeof C);
}

void caption_on_moved(void (*fn)(const CaptionCfg *, void *), void *ctx) { C.moved = fn; C.moved_ctx = ctx; }

void caption_configure(const CaptionCfg *c)
{
    C.cfg = *c;
    if (!C.hwnd) return;
    if (!C.cfg.enabled && !C.has_notice && !C.moving && !C.preview) ShowWindow(C.hwnd, SW_HIDE);
    else if (C.text || C.has_notice || C.moving || C.preview) relayout();
}

void caption_move_mode(int on)
{
    if (!C.hwnd || C.moving == (on != 0)) return;
    C.moving = on != 0;
    set_click_through(!C.moving);
    relayout();
    if (!C.moving) {
        if (!C.cfg.enabled && !C.preview && !C.has_notice) ShowWindow(C.hwnd, SW_HIDE);
        notify_moved();
    }
}

int caption_moving(void) { return C.moving; }

void caption_preview(int on)
{
    if (!C.hwnd) return;
    C.preview = on != 0;
    if (C.preview) relayout();
    else if (!C.moving && !C.has_notice && !(C.cfg.enabled && C.text)) ShowWindow(C.hwnd, SW_HIDE);
    else relayout();
}

void caption_set(const wchar_t *text, size_t n, double seconds)
{
    if (!C.hwnd) return;
    free(C.text);
    C.text = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!C.text) { C.n = 0; C.nchunks = 0; return; }
    memcpy(C.text, text, n * sizeof(wchar_t));
    C.text[n] = 0;
    for (size_t i = 0; i < n; i++) if (C.text[i] < L' ') C.text[i] = L' ';
    C.n = n;
    /* about two lines of text per page at the configured size and width */
    HDC dc = GetDC(NULL);
    int px = GetDeviceCaps(dc, HORZRES);
    ReleaseDC(NULL, dc);
    int per_line = (int)((double)px * C.cfg.width_pct / 100.0 / (C.cfg.font_pt * 0.95));
    if (per_line < 20) per_line = 20;
    C.nchunks = caption_chunks(C.text, n, (size_t)per_line * 2, C.st, C.ln, MAX_CHUNKS);
    C.chunk = 0;
    C.synced = 0;
    C.seconds = seconds;
    KillTimer(C.hwnd, T_HIDE);
    if (!C.cfg.enabled) return;
    if (!C.has_notice) relayout();
    start_paging();
}

void caption_progress(double frac)
{
    if (!C.hwnd || !C.cfg.enabled || !C.text || C.nchunks < 2) return;
    C.synced = 1;
    KillTimer(C.hwnd, T_PAGE);                           /* the real position replaces the timed estimate */
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    size_t idx = (size_t)(frac * (double)C.n) + 14;      /* a few words ahead: the page is up before its words are spoken */
    int ch = 0;
    for (int i = 0; i < C.nchunks; i++) if (C.st[i] <= idx) ch = i;
    if (ch > C.chunk) { C.chunk = ch; if (!C.has_notice) relayout(); }
}

void caption_idle(void)
{
    if (!C.hwnd || !C.cfg.enabled) return;
    KillTimer(C.hwnd, T_PAGE);
    if (C.cfg.hide_when_paused) SetTimer(C.hwnd, T_HIDE, 4000, NULL);
}

void caption_notice(const wchar_t *text, int ms)
{
    if (!C.hwnd) return;
    wcsncpy(C.notice, text, 299);
    C.notice[299] = 0;
    C.has_notice = 1;
    relayout();
    KillTimer(C.hwnd, T_NOTICE);
    SetTimer(C.hwnd, T_NOTICE, (UINT)(ms < 500 ? 500 : ms), NULL);
}

void caption_hide(void)
{
    if (!C.hwnd) return;
    C.has_notice = 0;
    if (C.moving || C.preview || (C.cfg.enabled && C.text && IsWindowVisible(C.hwnd))) relayout();
    else ShowWindow(C.hwnd, SW_HIDE);
}

int caption_visible(void) { return C.hwnd && IsWindowVisible(C.hwnd); }
