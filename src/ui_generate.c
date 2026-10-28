/*
 * ui_generate.c - Generate audio dialog.
 *
 * Shows, before anything is synthesized, how many segments exist, how many are already in the
 * audio cache and how many must be synthesized (the analysis is just IR building + cache
 * lookups: no speech engine involved). Pause/trim/normalize settings only affect the final
 * assembly; only the chunk size (and voice/rate/volume/dictionary) can cause re-synthesis.
 * Output: one WAV, or one WAV per chapter ("01 - Title.wav") into a folder.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_generate.h"
#include "gen.h"
#include "util.h"

#define WM_GEN_DONE (WM_APP + 21)
#define POLL_ID 1

enum { G_CHUNK = 900, G_SENT_MS, G_PARA_MS, G_CHAP_MS, G_SCALE, G_TRIM, G_NORM, G_ANALYSIS, G_PATH, G_BROWSE,
       G_CACHEINFO, G_OPENDIR, G_CLEAR, G_PROGRESS, G_STATUS, G_GO, G_STOP, G_RANGE, G_ONEFILE, G_PERCHAP, G_PATHLBL };

static struct {
    HWND hwnd, chunk, sentms, paraMs, chapms, scale, trim, norm, analysis, path, pathlbl, browse, cacheinfo, opendir, clear;
    HWND progress, status, go, stop, close, range, onefile, perchap;
    GenUi *ui;
    GenJob *job;
    int dpi, done;
    wchar_t confirmed[MAX_PATH];        /* output target the user already agreed to overwrite in this session */
} G;

#define GS(x) MulDiv((x), G.dpi, 96)

static HWND gmk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, GS(x), GS(y), GS(w), GS(h),
                           G.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}
static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }


static void fmt_time(double sec, wchar_t *b, size_t n)
{
    long t = (long)(sec + 0.5);
    _snwprintf(b, n, L"%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    b[n - 1] = 0;
}

static int read_int(HWND ed, int lo, int hi, int def)
{
    wchar_t b[32];
    GetWindowTextW(ed, b, 32);
    wchar_t *end;
    long v = wcstol(b, &end, 10);
    if (end == b) return def;
    return v < lo ? lo : (v > hi ? hi : (int)v);
}

static int per_chapter_now(void) { return SendMessageW(G.perchap, BM_GETCHECK, 0, 0) == BST_CHECKED; }

/* controls -> settings struct */
static void pull_settings(void)
{
    GenSettings *s = G.ui->settings;
    s->chunk_sentences = (int)SendMessageW(G.chunk, CB_GETCURSEL, 0, 0) + 1;
    if (s->chunk_sentences < 1) s->chunk_sentences = 1;
    s->sentence_ms = read_int(G.sentms, 0, 10000, s->sentence_ms);
    s->paragraph_ms = read_int(G.paraMs, 0, 20000, s->paragraph_ms);
    s->chapter_ms = read_int(G.chapms, 0, 60000, s->chapter_ms);
    s->scale_pauses = SendMessageW(G.scale, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->trim_silence = SendMessageW(G.trim, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->normalize = SendMessageW(G.norm, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->per_chapter = per_chapter_now();
}


static void cache_line(void)
{
    unsigned long files = 0;
    unsigned long long bytes = 0;
    cache_stats(G.ui->cache, &files, &bytes);
    wchar_t b[700];
    _snwprintf(b, 700, L"Audio cache: %lu segment files, %.1f MB   (%ls)", files, (double)bytes / 1048576.0, G.ui->cache->root);
    b[699] = 0;
    SetWindowTextW(G.cacheinfo, b);
}

static void sync_mode_labels(void)
{
    int pc = per_chapter_now();
    SetWindowTextW(G.pathlbl, pc ? L"Output folder:" : L"Output file:");
    SetWindowTextW(G.path, pc ? G.ui->out_dir : G.ui->out_path);
}

static void analyze(void)
{
    GenUi *ui = G.ui;
    pull_settings();
    SynthParams sp = ui->synth;
    sp.chunk_sentences = ui->settings->chunk_sentences;
    sp.chunk_max_chars = 700;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    IR ir;
    wchar_t msg[800];
    if (!ir_build(&ir, ui->doc, ui->speech, ui->first_sent, ui->last_sent, &sp, ui->chapters)) {
        SetWindowTextW(G.analysis, L"Out of memory while analysing.");
        SetCursor(old);
        return;
    }
    int hits = 0, miss = 0, files = ir.n ? 1 : 0;
    double cached_sec = 0, est_sec = 0;
    double cps = 15.0 * pow(3.0, ui->synth.rate / 10.0);        /* rough speaking speed in characters/second */
    for (int i = 0; i < ir.n; i++) {
        CacheEntry e;
        if (cache_lookup(ui->cache, &ir.seg[i].key, &e)) { hits++; cached_sec += (double)e.samples / e.rate; }
        else { miss++; est_sec += (double)wcslen(ir.seg[i].text) / cps; }
        if (i > 0 && ir.seg[i].chapter != ir.seg[i - 1].chapter) files++;
    }
    double ps = config_pause_scale(ui->settings, ui->synth.rate), pauses = 0;
    int pc = ui->settings->per_chapter;
    for (int i = 0; i + 1 < ir.n; i++) {
        if (ir.seg[i].chapter_end) { if (!pc) pauses += ui->settings->chapter_ms * ps / 1000.0; }
        else pauses += (ir.seg[i].para_end ? ui->settings->paragraph_ms : ui->settings->sentence_ms) * ps / 1000.0;
    }
    double total = cached_sec + est_sec + pauses;
    wchar_t tt[32], fl[96];
    fmt_time(total, tt, 32);
    double mb = total * TTS_WAV_RATE * 2 / 1048576.0;
    if (pc) _snwprintf(fl, 96, L"     Files: %d (one per chapter, largest well under 4 GB)", files);
    else _snwprintf(fl, 96, L"%ls", mb > 4000 ? L"   - TOO LARGE for one WAV file: use one file per chapter" : L"");
    fl[95] = 0;
    _snwprintf(msg, 800,
        L"Segments: %d     already in cache: %d     to synthesize: %d\r\n"
        L"Duration: %ls%ls     WAV size: %.0f MB%ls\r\n"
        L"%ls",
        ir.n, hits, miss, tt, miss ? L" (estimated for new segments)" : L"", mb, fl,
        miss == 0 && ir.n > 0 ? L"Everything is cached: exporting only assembles audio (fast)."
                              : L"Only the segments to synthesize use the speech engine; everything else is reused.");
    msg[799] = 0;
    SetWindowTextW(G.analysis, msg);
    ir_free(&ir);
    cache_line();
    SetCursor(old);
}

static void set_running(int running)
{
    EnableWindow(G.go, !running);
    EnableWindow(G.stop, running);
    EnableWindow(G.close, !running);
    HWND ctl[] = { G.chunk, G.sentms, G.paraMs, G.chapms, G.scale, G.trim, G.norm, G.path, G.browse, G.clear, G.opendir, G.onefile, G.perchap };
    for (size_t i = 0; i < sizeof ctl / sizeof ctl[0]; i++) EnableWindow(ctl[i], !running);
}

static void browse(void)
{
    wchar_t path[MAX_PATH];
    GetWindowTextW(G.path, path, MAX_PATH);
    if (per_chapter_now()) {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = G.hwnd;
        bi.lpszTitle = L"Choose the folder for the chapter files";
        bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(G.path, path);
            CoTaskMemFree(pidl);
        }
        return;
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = G.hwnd;
    ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) { SetWindowTextW(G.path, path); wcscpy(G.confirmed, path); }   /* the dialog already asked */
}

static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void start(void)
{
    GenUi *ui = G.ui;
    pull_settings();
    int pc = ui->settings->per_chapter;
    wchar_t path[MAX_PATH], err[TTS_ERR_MAX];
    GetWindowTextW(G.path, path, MAX_PATH);
    if (!path[0]) { browse(); GetWindowTextW(G.path, path, MAX_PATH); if (!path[0]) return; }
    if (pc) wcscpy(ui->out_dir, path); else wcscpy(ui->out_path, path);

    SynthParams sp = ui->synth;
    sp.chunk_sentences = ui->settings->chunk_sentences;
    sp.chunk_max_chars = 700;
    IR ir;
    if (!ir_build(&ir, ui->doc, ui->speech, ui->first_sent, ui->last_sent, &sp, ui->chapters)) { MessageBoxW(G.hwnd, L"Out of memory.", L"Generate audio", MB_OK | MB_ICONERROR); return; }
    if (ir.n == 0) { ir_free(&ir); MessageBoxW(G.hwnd, L"There is nothing to speak in this range.", L"Generate audio", MB_OK | MB_ICONINFORMATION); return; }

    if (pc) {
        if (!dir_exists(path) && !CreateDirectoryW(path, NULL)) {
            wchar_t why[256];
            util_format_win32(why, 256, L"", GetLastError());
            _snwprintf(err, TTS_ERR_MAX, L"Unable to create the output folder:\n%ls\n\nReason:\n%ls", path, why);
            ir_free(&ir);
            MessageBoxW(G.hwnd, err, L"Generate audio", MB_OK | MB_ICONERROR);
            return;
        }
        int existing = 0, ordinal = 0;
        for (int i = 0; i < ir.n; i++) {
            if (i > 0 && ir.seg[i].chapter == ir.seg[i - 1].chapter) continue;
            int c = ir.seg[i].chapter;
            wchar_t fn[200], full[MAX_PATH];
            chapter_filename(fn, 200, ++ordinal, (ui->chapters && c >= 0 && c < ui->chapters->n) ? ui->chapters->c[c].title : L"Beginning");
            _snwprintf(full, MAX_PATH, L"%ls\\%ls", path, fn);
            full[MAX_PATH - 1] = 0;
            if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) existing++;
        }
        if (existing && _wcsicmp(G.confirmed, path) != 0) {
            wchar_t q[200];
            _snwprintf(q, 200, L"%d chapter file(s) already exist in this folder and will be replaced.\n\nContinue?", existing);
            q[199] = 0;
            if (MessageBoxW(G.hwnd, q, L"Generate audio", MB_YESNO | MB_ICONQUESTION) != IDYES) { ir_free(&ir); return; }
        }
        wcscpy(G.confirmed, path);
    } else if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && _wcsicmp(G.confirmed, path) != 0) {
        wchar_t q[MAX_PATH + 80];
        _snwprintf(q, MAX_PATH + 80, L"%ls\n\nalready exists. Replace it?", path);
        q[MAX_PATH + 79] = 0;
        if (MessageBoxW(G.hwnd, q, L"Generate audio", MB_YESNO | MB_ICONQUESTION) != IDYES) { ir_free(&ir); return; }
        wcscpy(G.confirmed, path);
    }

    AsmOptions ao;
    memset(&ao, 0, sizeof ao);
    double ps = config_pause_scale(ui->settings, ui->synth.rate);
    ao.rate = sp.wav_rate;
    ao.sentence_ms = (int)(ui->settings->sentence_ms * ps + 0.5);
    ao.paragraph_ms = (int)(ui->settings->paragraph_ms * ps + 0.5);
    ao.chapter_ms = (int)(ui->settings->chapter_ms * ps + 0.5);
    ao.trim_silence = ui->settings->trim_silence;
    ao.trim_threshold = 300;
    ao.normalize = ui->settings->normalize;

    if (ui->changed) ui->changed(ui->ctx);
    G.job = gen_start(ui->cache, &ir, &sp, &ao, pc ? NULL : path, pc, pc ? path : NULL, ui->chapters,
                      G.hwnd, WM_GEN_DONE, err, TTS_ERR_MAX);
    if (!G.job) { ir_free(&ir); MessageBoxW(G.hwnd, err, L"Generate audio", MB_OK | MB_ICONERROR); return; }
    SendMessageW(G.progress, PBM_SETRANGE32, 0, 1);
    SendMessageW(G.progress, PBM_SETPOS, 0, 0);
    set_running(1);
    SetTimer(G.hwnd, POLL_ID, 200, NULL);
    SetWindowTextW(G.status, L"Starting...");
}

static void poll(void)
{
    GenJob *j = G.job;
    if (!j) return;
    LONG total = j->total > 0 ? j->total : 1, done = j->done;
    SendMessageW(G.progress, PBM_SETRANGE32, 0, total);
    SendMessageW(G.progress, PBM_SETPOS, (WPARAM)done, 0);
    wchar_t b[220];
    if (j->phase == 0)
        _snwprintf(b, 220, L"Synthesizing segment %ld of %ld   (%ld new, %ld reused from cache)%ls", done, total,
                   (long)j->synthesized, (long)j->hits, j->repaired ? L"   - repairing a corrupt cache entry" : L"");
    else
        _snwprintf(b, 220, L"Assembling audio: %ld of %ld   (%ld file(s) written)", done, total, (long)j->files);
    b[219] = 0;
    SetWindowTextW(G.status, b);
}

static void finish(void)
{
    GenJob *j = G.job;
    KillTimer(G.hwnd, POLL_ID);
    WaitForSingleObject(j->thread, INFINITE);
    wchar_t msg[900];
    switch (j->state) {
    case GEN_OK: {
        wchar_t tt[32];
        fmt_time((double)j->samples / TTS_WAV_RATE, tt, 32);
        _snwprintf(msg, 900, L"Done: %ls  (%.0f MB)  -  %ld synthesized, %ld reused from cache%ls.\r\n%ls",
                   tt, (double)j->samples * 2 / 1048576.0, (long)j->synthesized, (long)j->hits,
                   j->repaired ? L", repaired a corrupt cache entry" : L"",
                   j->per_chapter ? (wchar_t *)j->out_dir : (wchar_t *)j->out_path);
        if (j->per_chapter) {
            wchar_t extra[64];
            _snwprintf(extra, 64, L"  (%ld files)", (long)j->files);
            extra[63] = 0;
            wcsncat(msg, extra, 899 - wcslen(msg));
        }
        msg[899] = 0;
        SetWindowTextW(G.status, msg);
        SendMessageW(G.progress, PBM_SETPOS, (WPARAM)(j->total > 0 ? j->total : 1), 0);
        break;
    }
    case GEN_CANCELLED:
        _snwprintf(msg, 900, L"Cancelled. %ld segments were synthesized and stay in the cache; run it again to continue.", (long)j->synthesized);
        msg[899] = 0;
        SetWindowTextW(G.status, msg);
        break;
    default:
        SetWindowTextW(G.status, L"Failed.");
        MessageBoxW(G.hwnd, j->err, L"Generate audio", MB_OK | MB_ICONERROR);
        break;
    }
    gen_free(j);
    G.job = NULL;
    set_running(0);
    analyze();
}

static LRESULT CALLBACK g_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    GenUi *ui = G.ui;
    switch (msg) {
    case WM_CREATE: {
        G.hwnd = hwnd;
        wchar_t b[256];
        G.range = gmk(L"STATIC", L"", SS_LEFT, 0, G_RANGE, 8, 8, 644, 18);
        gmk(L"STATIC", L"Sentences per synthesis chunk:", SS_LEFT, 0, 0, 8, 36, 230, 18);
        G.chunk = gmk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, G_CHUNK, 240, 32, 70, 200);
        gmk(L"STATIC", L"(more = smoother prosody, less = finer re-use after edits)", SS_LEFT, 0, 0, 320, 36, 330, 18);
        gmk(L"BUTTON", L"Pauses and audio - assembly only: changing these never re-synthesizes", BS_GROUPBOX, 0, 0, 8, 62, 644, 112);
        gmk(L"STATIC", L"Sentence (ms):", SS_LEFT, 0, 0, 20, 88, 100, 18);
        G.sentms = gmk(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, G_SENT_MS, 124, 84, 56, 22);
        gmk(L"STATIC", L"Paragraph (ms):", SS_LEFT, 0, 0, 200, 88, 100, 18);
        G.paraMs = gmk(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, G_PARA_MS, 304, 84, 56, 22);
        gmk(L"STATIC", L"Chapter (ms):", SS_LEFT, 0, 0, 380, 88, 90, 18);
        G.chapms = gmk(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, G_CHAP_MS, 474, 84, 56, 22);
        G.scale = gmk(L"BUTTON", L"Scale pauses with speech rate", BS_AUTOCHECKBOX | WS_TABSTOP, 0, G_SCALE, 20, 112, 300, 22);
        G.trim = gmk(L"BUTTON", L"Trim silence at segment edges", BS_AUTOCHECKBOX | WS_TABSTOP, 0, G_TRIM, 20, 138, 300, 22);
        G.norm = gmk(L"BUTTON", L"Normalize volume (peak to about -1 dB)", BS_AUTOCHECKBOX | WS_TABSTOP, 0, G_NORM, 330, 138, 310, 22);
        gmk(L"BUTTON", L"Analysis", BS_GROUPBOX, 0, 0, 8, 182, 644, 96);
        G.analysis = gmk(L"STATIC", L"", SS_LEFT, 0, G_ANALYSIS, 20, 202, 620, 70);
        G.onefile = gmk(L"BUTTON", L"One &WAV file", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, G_ONEFILE, 8, 288, 300, 20);
        G.perchap = gmk(L"BUTTON", L"One WAV per &chapter (\"01 - Title.wav\") into a folder", BS_AUTORADIOBUTTON, 0, G_PERCHAP, 8, 310, 500, 20);
        G.pathlbl = gmk(L"STATIC", L"Output file:", SS_LEFT, 0, G_PATHLBL, 8, 340, 100, 18);
        G.path = gmk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, G_PATH, 112, 336, 446, 22);
        G.browse = gmk(L"BUTTON", L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, 0, G_BROWSE, 566, 335, 86, 24);
        G.cacheinfo = gmk(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 0, G_CACHEINFO, 8, 368, 644, 18);
        G.opendir = gmk(L"BUTTON", L"Open cache &folder", BS_PUSHBUTTON | WS_TABSTOP, 0, G_OPENDIR, 8, 390, 140, 26);
        G.clear = gmk(L"BUTTON", L"&Clear cache...", BS_PUSHBUTTON | WS_TABSTOP, 0, G_CLEAR, 156, 390, 130, 26);
        G.progress = gmk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, G_PROGRESS, 8, 428, 644, 20);
        G.status = gmk(L"STATIC", L"", SS_LEFT, 0, G_STATUS, 8, 454, 644, 36);
        G.go = gmk(L"BUTTON", L"&Generate and export", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, G_GO, 8, 498, 200, 32);
        G.stop = gmk(L"BUTTON", L"&Stop", BS_PUSHBUTTON | WS_TABSTOP, 0, G_STOP, 216, 498, 90, 32);
        G.close = gmk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 562, 498, 90, 32);
        EnumChildWindows(hwnd, font_cb, (LPARAM)ui->font);

        _snwprintf(b, 256, L"Range: %ls  (sentences %d - %d)", ui->range_label, ui->first_sent + 1, ui->last_sent + 1);
        b[255] = 0;
        SetWindowTextW(G.range, b);
        for (int i = 1; i <= 8; i++) { wchar_t n[4]; _snwprintf(n, 4, L"%d", i); SendMessageW(G.chunk, CB_ADDSTRING, 0, (LPARAM)n); }
        const GenSettings *s = ui->settings;
        SendMessageW(G.chunk, CB_SETCURSEL, (WPARAM)(s->chunk_sentences - 1), 0);
        _snwprintf(b, 256, L"%d", s->sentence_ms); SetWindowTextW(G.sentms, b);
        _snwprintf(b, 256, L"%d", s->paragraph_ms); SetWindowTextW(G.paraMs, b);
        _snwprintf(b, 256, L"%d", s->chapter_ms); SetWindowTextW(G.chapms, b);
        SendMessageW(G.scale, BM_SETCHECK, s->scale_pauses ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(G.trim, BM_SETCHECK, s->trim_silence ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(G.norm, BM_SETCHECK, s->normalize ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(s->per_chapter ? G.perchap : G.onefile, BM_SETCHECK, BST_CHECKED, 0);
        sync_mode_labels();
        EnableWindow(G.stop, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == POLL_ID) poll();
        return 0;
    case WM_GEN_DONE:
        finish();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case G_CHUNK: if (HIWORD(wp) == CBN_SELCHANGE) analyze(); return 0;
        case G_SENT_MS: case G_PARA_MS: case G_CHAP_MS: if (HIWORD(wp) == EN_KILLFOCUS) analyze(); return 0;
        case G_SCALE: case G_TRIM: case G_NORM: if (HIWORD(wp) == BN_CLICKED) analyze(); return 0;
        case G_ONEFILE: case G_PERCHAP:
            if (HIWORD(wp) == BN_CLICKED) {
                wchar_t cur[MAX_PATH];                          /* remember what was typed for the mode we are leaving */
                GetWindowTextW(G.path, cur, MAX_PATH);
                if (LOWORD(wp) == G_PERCHAP) wcscpy(ui->out_path, cur); else wcscpy(ui->out_dir, cur);
                sync_mode_labels();
                analyze();
            }
            return 0;
        case G_BROWSE: browse(); return 0;
        case G_GO: start(); return 0;
        case G_STOP: if (G.job) { gen_cancel(G.job); SetWindowTextW(G.status, L"Stopping (finished segments stay in the cache)..."); } return 0;
        case G_OPENDIR: ShellExecuteW(hwnd, L"open", ui->cache->root, NULL, NULL, SW_SHOWNORMAL); return 0;
        case G_CLEAR:
            if (MessageBoxW(hwnd, L"Delete ALL cached audio segments?\n\nThey will be synthesized again the next time you generate audio.",
                            L"Clear audio cache", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                wchar_t err[256];
                if (!cache_clear(ui->cache, err, 256)) MessageBoxW(hwnd, err, L"Clear audio cache", MB_OK | MB_ICONERROR);
                analyze();
            }
            return 0;
        case IDCANCEL: if (!G.job) G.done = 1; return 0;
        }
        break;
    case WM_CLOSE:
        if (!G.job) G.done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void generate_dialog(HWND parent, GenUi *ui)
{
    memset(&G, 0, sizeof G);
    G.ui = ui; G.dpi = ui->dpi;
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceGenerate", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = g_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceGenerate";
        if (!RegisterClassExW(&wc)) return;
    }
    RECT r = { 0, 0, GS(660), GS(540) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceGenerate", L"Generate audio", style, x, y, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return; }
    ShowWindow(hw, SW_SHOW);
    analyze();
    SetFocus(G.go);

    MSG m;
    while (!G.done) {
        int rr = GetMessageW(&m, NULL, 0, 0);
        if (rr <= 0) {
            if (rr == 0) PostQuitMessage((int)m.wParam);
            if (G.job) { gen_cancel(G.job); gen_free(G.job); G.job = NULL; }
            break;
        }
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    {   /* remember the path typed for the current mode */
        wchar_t cur[MAX_PATH];
        GetWindowTextW(G.path, cur, MAX_PATH);
        pull_settings();
        if (ui->settings->per_chapter) wcscpy(ui->out_dir, cur); else wcscpy(ui->out_path, cur);
    }
    if (ui->changed) ui->changed(ui->ctx);
    EnableWindow(parent, TRUE);
    DestroyWindow(hw);
    SetForegroundWindow(parent);
}
