/*
 * app.c - Win32 front end ("Mouthpiece.exe"), Milestone 2.
 *
 * Text flow: EDIT control (SOURCE, never modified by the pipeline)
 *   --(debounced, 300 ms)--> preproc.c (cleanup, NORMALIZED text + offset map)
 *   --> Document (doc.c) --> Player (player.c) --> TTSEngine.
 * Sentence spans live in normalized space; g.pre maps them back to source
 * offsets for highlighting and maps the caret/selection the other way. The Document is rebuilt from the whole edit text
 * (O(n), a few ms per MB); incremental rebuild of dirty paragraphs is a later
 * optimisation. Editing while speaking stops playback, because sentence
 * indices would no longer match the text.
 *
 * Audio generation (Milestone 5): Document -> Speech IR segments (ir.c) -> per-segment
 * cache (cache.c) -> background synthesis of missing segments (gen.c) -> assembly (assemble.c).
 * It lives in the Generate dialog (ui_generate.c); this file only gathers its inputs.
 *
 * UI THREAD owns every window and the playback TTSEngine. A generation job runs on its own
 * thread with its own TTSEngine + COM apartment and never touches a control.
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
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "tts_engine.h"
#include "util.h"
#include "doc.h"
#include "player.h"
#include "preproc.h"
#include "ui_cleanup.h"
#include "ui_speech.h"
#include "ui_generate.h"
#include "cache.h"
#include "ini.h"
#include "chapters.h"
#include "ui_nav.h"
#include "ui_gutter.h"
#include "ui_about.h"
#include "ui_find.h"
#include "ui_profiles.h"
#include "ui_analyze.h"
#include "ui_caption.h"
#include "ui_hotkeys.h"
#include "ui_goto.h"
#include "ui_caption_dlg.h"
#include "hotkeys.h"
#include "navmath.h"
#include "pronscope.h"
#include "config.h"
#include <shlobj.h>
#include <shellapi.h>
#include "utf8.h"
#include "pron.h"
#include "speech.h"

#define APP_TITLE L"Mouthpiece"
#define POLL_TIMER_ID 1
#define DOC_TIMER_ID  2
#define DOC_DEBOUNCE_MS 300

enum {
    CMD_PLAY = 100, CMD_PLAY_TRAY, CMD_STOP, CMD_EXPORT, CMD_EXPORT_SEL, CMD_OPEN, CMD_EXIT, CMD_SELALL, CMD_ABOUT,
    CMD_PREV, CMD_NEXT, CMD_PREV_PARA, CMD_NEXT_PARA,
    CMD_PLAY_SENT, CMD_PLAY_PARA, CMD_PLAY_SEL, CMD_REPLAY, CMD_CLEANUP, CMD_PRON, CMD_SPEECH_PREVIEW, CMD_HOMEEND,
    CMD_PREV_CH, CMD_NEXT_CH, CMD_PLAY_CH, CMD_EXPORT_CH, CMD_NAV, CMD_BM_TOGGLE, CMD_BM_NEXT, CMD_BM_PREV,
    CMD_PROJ_NEW, CMD_PROJ_OPEN, CMD_PROJ_SAVE, CMD_PROJ_CLOSE, CMD_NEW, CMD_VOICES_LIST, CMD_VOICES_RESCAN,
    CMD_DATA, CMD_DATA_RESET, CMD_FIND, CMD_FIND_NEXT, CMD_FIND_PREV, CMD_PROFILES, CMD_ANALYZE,
    ID_VOICE, ID_RATE, ID_VOLUME, ID_EDIT, ID_STATUS, ID_GUTTER
};

typedef enum { ST_IDLE, ST_PLAYING, ST_PAUSED } PlayState;



static struct {
    HWND hwnd, lblVoice, voice, lblRate, rate, rateVal, lblVol, vol, volVal;
    HWND prev, play, next, stop, exportBtn, edit, status;
    HFONT font;
    HWND gutter;
    int data_how;          /* 0 = %APPDATA%\Mouthpiece, 1 = chosen folder (datadir.txt), 2 = portable MouthpieceData next to the exe */
    int dpi;
    PlayState state;
    TTSEngine *engine;
    TTSVoiceList voices;
    Cache cache;           /* persistent per-segment audio cache */
    int cache_ok;
    wchar_t cache_root[MAX_PATH];
    GenSettings gen;       /* pauses, trim, normalize, chunk size */
    wchar_t last_out[MAX_PATH];
    wchar_t app_dir[MAX_PATH], settings_path[MAX_PATH], scratch_dict[MAX_PATH], scratch_cache[MAX_PATH];
    wchar_t last_dir[MAX_PATH];        /* last per-chapter export folder */
    ChapterList chapters;  /* detected + corrected chapters of the current document */
    Overrides ov;          /* the user's chapter corrections */
    BookmarkList bm;
    ProfileList profiles;  /* voice profiles (global, in settings.txt) */
    wchar_t project_path[MAX_PATH], project_dir[MAX_PATH], project_name[128];   /* empty path = scratch mode */
    int dirty;             /* project text changed since the last save */
    Document doc;
    PreprocOptions opts;
    int home_end_doc;      /* Home/End jump to the start/end of the document (not the line) */
    HMENU menu_tools;
    PronDict dict;         /* user pronunciation dictionary (persisted in %APPDATA%\\Mouthpiece\\pronunciations.txt) */
    Speech speech;         /* dictionary + built-in speech normalization, applied per sentence */
    wchar_t dict_path[MAX_PATH];
    PreprocResult pre;     /* offset map for the current doc (text buffer is owned by doc) */
    int doc_dirty;
    Player player;
    int cur_sent;          /* sentence highlighted / being spoken, -1 if none */
    int play_last;         /* last sentence of the current play range */
} g;

#define S(x) MulDiv((x), g.dpi, 96)


/* Dialogs opened while the main window is hidden in the tray (or minimized) need a visible owner, otherwise they never
 * appear. A one-pixel, nearly transparent tool window in the middle of the screen stands in for it; it is hidden again by
 * a timer once no dialog is using it (a dialog disables its owner while it runs). */
static HWND g_anchor;
#define TIMER_ANCHOR 6
static LRESULT CALLBACK anchor_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static HWND dlg_parent(void)
{
    if (g.hwnd && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) return g.hwnd;
    if (!g_anchor) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = anchor_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"MouthpieceAnchor";
        RegisterClassW(&wc);
        g_anchor = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, L"MouthpieceAnchor", L"Mouthpiece", WS_POPUP,
                                   0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_anchor) return g.hwnd;
        SetLayeredWindowAttributes(g_anchor, 0, 1, LWA_ALPHA);
    }
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int cx = (mi.rcWork.left + mi.rcWork.right) / 2, cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    EnableWindow(g_anchor, TRUE);
    SetWindowPos(g_anchor, HWND_TOPMOST, cx, cy, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_anchor);
    SetTimer(g.hwnd, TIMER_ANCHOR, 1000, NULL);
    return g_anchor;
}

static const COLORREF mark_colors[MK_COUNT] = { RGB(30, 110, 200), RGB(0, 140, 130), RGB(200, 40, 40), RGB(130, 80, 170), RGB(40, 150, 60), RGB(190, 50, 120) };
static void flag_mark(int kind);
static void ext_to_ini(Ini *a);
static void sync_menu_checks(void);
static void jump_to(int target);
static void goto_any_mark(int dir);
static int shown_rate_value;      /* the rate shown by the slider at the last label update */
static void on_sentence_shown(int sent);
static void caption_follow(void);
static void on_state_changed(void);
static void sleep_check(void);
static void update_value_labels(void);
static void update_controls(void);

/* ------------------------------------------------------------ small helpers */

static void set_status(const wchar_t *s)
{
    SendMessageW(g.status, SB_SETTEXTW, 0, (LPARAM)s);
}

static void error_box(const wchar_t *msg)
{
    MessageBoxW(dlg_parent(), msg, APP_TITLE, MB_OK | MB_ICONERROR);
}

static void update_char_count(void)
{
    wchar_t b[64];
    _snwprintf(b, 64, L"%d chars", GetWindowTextLengthW(g.edit));
    b[63] = 0;
    SendMessageW(g.status, SB_SETTEXTW, 2, (LPARAM)b);
}

static TTSSpeechSettings current_settings(void)
{
    TTSSpeechSettings s;
    s.rate   = (int)SendMessageW(g.rate, TBM_GETPOS, 0, 0);
    s.volume = (int)SendMessageW(g.vol, TBM_GETPOS, 0, 0);
    return s;
}

static void update_value_labels(void)
{
    wchar_t b[16];
    TTSSpeechSettings s = current_settings();
    _snwprintf(b, 16, L"%+d", s.rate);   b[15] = 0;
    if (s.rate == 0) wcscpy(b, L"0");
    SetWindowTextW(g.rateVal, b);
    _snwprintf(b, 16, L"%d%%", s.volume); b[15] = 0;
    SetWindowTextW(g.volVal, b);
    shown_rate_value = s.rate;
}

static void update_controls(void)
{
    BOOL idle = (g.state == ST_IDLE);
    BOOL haveVoices = g.voices.count > 0;
    const wchar_t *label = L"&Play";
    if (g.state == ST_PLAYING) label = L"&Pause";
    else if (g.state == ST_PAUSED) label = L"&Resume";
    SetWindowTextW(g.play, label);
    EnableWindow(g.play, haveVoices);
    EnableWindow(g.prev, TRUE);
    EnableWindow(g.next, TRUE);
    EnableWindow(g.stop, !idle);
    EnableWindow(g.exportBtn, haveVoices && idle);
    EnableWindow(g.voice, haveVoices && idle);
    EnableWindow(g.rate, TRUE);
    EnableWindow(g.vol, TRUE);
    on_state_changed();
}

/* ---------------------------------------------------------- document model */

static void refresh_gutter(void);

static void rebuild_doc(void)
{
    int len = GetWindowTextLengthW(g.edit);
    wchar_t *buf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!buf) { set_status(L"Out of memory building document."); return; }
    buf[0] = 0;
    if (len > 0) GetWindowTextW(g.edit, buf, len + 1);
    preproc_free(&g.pre);
    if (!preproc_run(buf, (size_t)len, &g.opts, 0, &g.pre)) {
        doc_free(&g.doc);
        preproc_free(&g.pre);
        set_status(L"Out of memory preparing text.");
    } else {
        wchar_t *norm = g.pre.text;            /* the document takes the buffer; the map stays here */
        size_t nlen = g.pre.len;
        g.pre.text = NULL;
        if (!doc_build_take(&g.doc, norm, nlen)) set_status(L"Out of memory building document.");
    }
    free(buf);
    g.doc_dirty = 0;
    chapters_free(&g.chapters);
    if (!chapters_detect(&g.chapters, &g.doc, &g.ov)) memset(&g.chapters, 0, sizeof g.chapters);
    pron_resolve_scopes(&g.dict, &g.doc, &g.chapters);   /* chapter/selection pronunciation scopes follow the text */
    refresh_gutter();
}

static void ensure_doc(void)
{
    if (g.doc_dirty) {
        KillTimer(g.hwnd, DOC_TIMER_ID);
        rebuild_doc();
    }
}

static void update_position(int sent)
{
    wchar_t b[160];
    if (sent < 0 || sent >= g.doc.nsent) {
        b[0] = 0;
    } else {
        const Sentence *sn = &g.doc.sents[sent];
        int pct = g.doc.len ? (int)((sn->start * 100) / g.doc.len) : 0;
        wchar_t chp[40];
        chp[0] = 0;
        if (g.chapters.n > 0) {
            int ch = chapter_of_sentence(&g.chapters, sent);
            if (ch >= 0) _snwprintf(chp, 40, L"Chapter %d/%d   ", ch + 1, g.chapters.n);
            else _snwprintf(chp, 40, L"Chapter -/%d   ", g.chapters.n);
            chp[39] = 0;
        }
        _snwprintf(b, 160, L"%lsPara %d/%d   Sentence %d/%d   %d%%", chp,
                   sn->para + 1, g.doc.npara, sent + 1, g.doc.nsent, pct);
        b[159] = 0;
    }
    SendMessageW(g.status, SB_SETTEXTW, 1, (LPARAM)b);
}

/* Highlight a sentence in the editor (selection) and report progress. */
static void show_sentence(int sent)
{
    if (sent < 0 || sent >= g.doc.nsent) return;
    g.cur_sent = sent;
    const Sentence *sn = &g.doc.sents[sent];
    size_t a = preproc_norm_to_src(&g.pre, sn->start);
    size_t b = preproc_norm_end_to_src(&g.pre, sn->start + sn->len);
    SendMessageW(g.edit, EM_SETSEL, (WPARAM)a, (LPARAM)b);     /* highlight in the untouched SOURCE */
    SendMessageW(g.edit, EM_SCROLLCARET, 0, 0);
    update_position(sent);
    on_sentence_shown(sent);
}

static int caret_sentence(void)
{
    ensure_doc();
    DWORD sa = 0, sb = 0;
    SendMessageW(g.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    return doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sa));
}

static const wchar_t *selected_voice_id(void)
{
    int i = (int)SendMessageW(g.voice, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= g.voices.count) return NULL;
    return g.voices.items[i].id;
}

static void populate_voices(const wchar_t *keep_id)
{
    int sel = 0;
    SendMessageW(g.voice, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g.voices.count; i++) {
        SendMessageW(g.voice, CB_ADDSTRING, 0, (LPARAM)g.voices.items[i].name);
        if (keep_id && wcscmp(g.voices.items[i].id, keep_id) == 0) sel = i;
    }
    if (g.voices.count) SendMessageW(g.voice, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void rescan_voices(void)
{
    wchar_t keep[TTS_ID_MAX], err[TTS_ERR_MAX], m[200];
    const wchar_t *cur = selected_voice_id();
    keep[0] = 0;
    if (cur) wcsncpy(keep, cur, TTS_ID_MAX - 1);
    TTSVoiceList nl;
    if (tts_voices_enumerate(&nl, err, TTS_ERR_MAX) != TTS_SUCCESS) { set_status(err); return; }
    tts_voices_free(&g.voices);
    g.voices = nl;
    populate_voices(keep);
    _snwprintf(m, 200, L"Found %d installed voice%ls (%d-bit program).", g.voices.count,
               g.voices.count == 1 ? L"" : L"s", (int)(sizeof(void *) * 8));
    m[199] = 0;
    set_status(m);
}

static void show_voices(void)
{
    size_t cap = 4096 + (size_t)g.voices.count * 700;
    wchar_t *t = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!t) return;
    int n = _snwprintf(t, cap - 1, L"%d voice%ls found.\n\n", g.voices.count,
                       g.voices.count == 1 ? L"" : L"s");
    for (int i = 0; i < g.voices.count && n > 0 && (size_t)n < cap - 800; i++)
        n += _snwprintf(t + n, cap - (size_t)n - 1, L"%ls\n", g.voices.items[i].name);
    if (n > 0 && (size_t)n < cap - 700)
        _snwprintf(t + n, cap - (size_t)n - 1,
            L"\nVoices marked (32-bit) run in the helper MouthpieceVoiceHost32.exe, which must be in the same folder as "
            L"the program. If a voice you installed is missing, run Tools > Rescan voices.\n"
            L"Voices marked [OneCore] are the newer Windows voices; not every one can be used through SAPI.");
    t[cap - 1] = 0;
    MessageBoxW(dlg_parent(), t, L"Installed voices", MB_OK | MB_ICONINFORMATION);
    free(t);
}

static void refresh_gutter(void)
{
    if (!g.gutter) return;
    GutterMark *m = NULL;
    int n = 0;
    if (!g.doc_dirty && g.doc.nsent > 0) {
        m = (GutterMark *)calloc((size_t)g.chapters.n + (size_t)g.bm.n + 1, sizeof(GutterMark));
        for (int i = 0; m && i < g.chapters.n; i++) {
            int p = g.chapters.c[i].para;
            if (p < 0 || p >= g.doc.npara) continue;
            m[n].offset = preproc_norm_to_src(&g.pre, g.doc.paras[p].start);
            m[n].kind = GM_CHAPTER;
            m[n].tag = 100000 + i;
            wcsncpy(m[n].label, g.chapters.c[i].title ? g.chapters.c[i].title : L"", 47);
            n++;
        }
        for (int i = 0; m && i < g.bm.n; i++) {
            int sent = bm_resolve(&g.bm.b[i], &g.doc, &g.chapters);
            if (sent < 0 || sent >= g.doc.nsent) continue;
            int kind = g.bm.b[i].kind;
            m[n].offset = preproc_norm_to_src(&g.pre, g.doc.sents[sent].start);
            m[n].kind = kind == MK_BOOKMARK ? GM_BOOKMARK : GM_FLAG;
            m[n].glyph = mark_kind_glyph(kind)[0];
            m[n].color = (unsigned)mark_colors[kind];
            m[n].tag = i;
            _snwprintf(m[n].label, 48, L"%ls", g.bm.b[i].note ? g.bm.b[i].note : g.bm.b[i].label);
            n++;
        }
    }
    gutter_set_marks(g.gutter, m, n);
    free(m);
}

/* ----------------------------------------------------- settings and projects */
/*
 * Two storage modes share one UTF-8 key/value format (ini.c):
 *   scratch mode : settings.txt, pronunciations.txt, cache\  in %APPDATA%\Mouthpiece
 *   project mode : <dir>\book.ttsp (all settings + chapter fixes + bookmarks + position),
 *                  <dir>\source.txt, <dir>\pronunciations.txt, <dir>\cache\, <dir>\export\
 * settings.txt always keeps the scratch settings, the Home/End preference and the last project.
 */


static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void appdata_dir(wchar_t *out)
{
    out[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"APPDATA", out, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) { out[0] = 0; return; }
    wcscat(out, L"\\Mouthpiece");
    if (!dir_exists(out)) {                       /* the old TTSForge folder is renamed to Mouthpiece (falls back to using it in place if it cannot be renamed) */
        wchar_t old[MAX_PATH];
        wcscpy(old, out);
        wchar_t *k = wcsrchr(old, L'\\');
        if (k) { wcscpy(k, L"\\TTSForge"); if (dir_exists(old)) { if (!MoveFileExW(old, out, 0)) wcscpy(out, old); } }
    }
}

/* Where settings, the scratch dictionary and the scratch cache live. Priority:
 *   1. a folder named MouthpieceData next to the program (portable mode)
 *   2. the folder named in %APPDATA%\Mouthpiece\datadir.txt (chosen with Tools > Data location)
 *   3. %APPDATA%\Mouthpiece
 * Projects (.ttsp) keep their own files in their own folder regardless of this. */
static void init_paths(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH], pointer[MAX_PATH];
    g.app_dir[0] = 0;
    g.data_how = 0;
    dir[0] = 0;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *sl = wcsrchr(exe, L'\\');
        if (sl) {
            *sl = 0;
            _snwprintf(dir, MAX_PATH, L"%ls\\MouthpieceData", exe);
            dir[MAX_PATH - 1] = 0;
            if (dir_exists(dir)) g.data_how = 2; else dir[0] = 0;
        }
    }
    wchar_t ad[MAX_PATH];
    appdata_dir(ad);
    if (!dir[0] && ad[0]) {
        _snwprintf(pointer, MAX_PATH, L"%ls\\datadir.txt", ad);
        pointer[MAX_PATH - 1] = 0;
        unsigned char *buf = NULL;
        size_t len = 0;
        int missing = 0;
        wchar_t err[TTS_ERR_MAX];
        if (util_read_file(pointer, &buf, &len, &missing, err, TTS_ERR_MAX) && buf) {
            wchar_t w[MAX_PATH];
            size_t k = 0;
            size_t off = (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
            int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)buf + off, (int)(len - off), w, MAX_PATH - 1);
            if (wn > 0) {
                k = (size_t)wn;
                while (k && (w[k - 1] == L'\r' || w[k - 1] == L'\n' || w[k - 1] == L' ')) k--;
                w[k] = 0;
                if (k && (dir_exists(w) || CreateDirectoryW(w, NULL))) { wcscpy(dir, w); g.data_how = 1; }
            }
            free(buf);
        }
    }
    if (!dir[0]) {
        if (!ad[0]) return;
        wcscpy(dir, ad);
        g.data_how = 0;
    }
    CreateDirectoryW(ad[0] ? ad : dir, NULL);
    CreateDirectoryW(dir, NULL);
    wcscpy(g.app_dir, dir);
    _snwprintf(g.settings_path, MAX_PATH, L"%ls\\settings.txt", dir);
    _snwprintf(g.scratch_dict, MAX_PATH, L"%ls\\pronunciations.txt", dir);
    _snwprintf(g.scratch_cache, MAX_PATH, L"%ls\\cache", dir);
    g.settings_path[MAX_PATH - 1] = g.scratch_dict[MAX_PATH - 1] = g.scratch_cache[MAX_PATH - 1] = 0;
    wcscpy(g.dict_path, g.scratch_dict);
    wcscpy(g.cache_root, g.scratch_cache);
}

static int read_ini_file(const wchar_t *path, Ini *d)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    int missing = 0;
    wchar_t err[TTS_ERR_MAX];
    ini_init(d);
    if (!path[0] || !util_read_file(path, &buf, &len, &missing, err, TTS_ERR_MAX)) return 0;
    ini_from_utf8(d, (const char *)buf, len);
    free(buf);
    return 1;
}

static int write_ini_file(const wchar_t *path, const Ini *d, int report)
{
    wchar_t err[TTS_ERR_MAX];
    size_t len = 0;
    char *b = ini_to_utf8(d, &len);
    if (!b) return 0;
    int ok = util_write_file(path, b, len, err, TTS_ERR_MAX);
    free(b);
    if (!ok && report) error_box(err);
    return ok;
}

static void settings_to_ini(Ini *d, int project)
{
    const wchar_t *id = selected_voice_id();
    if (id) ini_set(d, L"Speech", L"VoiceId", id);
    TTSSpeechSettings st = current_settings();
    ini_set_int(d, L"Speech", L"Rate", st.rate);
    ini_set_int(d, L"Speech", L"Volume", st.volume);
    config_options_to_ini(d, &g.opts, &g.speech.opts, &g.gen);
    ini_set(d, L"Audio", L"LastOutput", g.last_out);
    ini_set(d, L"Audio", L"LastOutputDir", g.last_dir);
    if (!project) ini_set_int(d, L"Editor", L"HomeEndJumpsToDocument", g.home_end_doc ? 1 : 0);
}

/* Returns 1 if the saved voice is no longer installed. Absent keys keep their defaults. */
static int settings_from_ini(const Ini *d, int project)
{
    int missing = 0, sel = 0;
    config_options_from_ini(d, &g.opts, &g.speech.opts, &g.gen);
    g.last_out[0] = g.last_dir[0] = 0;
    const wchar_t *v = ini_get(d, L"Audio", L"LastOutput");
    if (v) wcsncpy(g.last_out, v, MAX_PATH - 1);
    v = ini_get(d, L"Audio", L"LastOutputDir");
    if (v) wcsncpy(g.last_dir, v, MAX_PATH - 1);
    if (!project) {
        g.home_end_doc = ini_get_int(d, L"Editor", L"HomeEndJumpsToDocument", 1) != 0;   /* on by default */
        if (g.menu_tools) CheckMenuItem(g.menu_tools, CMD_HOMEEND, MF_BYCOMMAND | (g.home_end_doc ? MF_CHECKED : MF_UNCHECKED));
    }
    int rate = (int)ini_get_int(d, L"Speech", L"Rate", 0), vol = (int)ini_get_int(d, L"Speech", L"Volume", 100);
    const wchar_t *id = ini_get(d, L"Speech", L"VoiceId");
    if (id && *id) {
        missing = 1;
        for (int i = 0; i < g.voices.count; i++)
            if (wcscmp(g.voices.items[i].id, id) == 0) { sel = i; missing = 0; break; }
    }
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    SendMessageW(g.rate, TBM_SETPOS, TRUE, rate);
    SendMessageW(g.vol, TBM_SETPOS, TRUE, vol);
    if (g.voices.count) SendMessageW(g.voice, CB_SETCURSEL, (WPARAM)sel, 0);
    update_value_labels();
    return missing;
}

static void project_to_ini(Ini *d)
{
    ini_set_int(d, L"Project", L"Version", 1);
    ini_set(d, L"Project", L"Name", g.project_name);
    ini_set(d, L"Project", L"Source", L"source.txt");
    overrides_to_ini(&g.ov, d);
    bm_to_ini(&g.bm, d);
    int s = g.doc.nsent ? caret_sentence() : -1;                /* resume position */
    Bookmark pos;
    if (s >= 0 && bm_make(&pos, &g.doc, &g.chapters, s, L"")) { bm_anchor_to_ini(&pos, d, L"Position"); bm_destroy(&pos); }
}

/* Persist everything that should survive a restart. */
static void save_settings(void)
{
    Ini a;
    read_ini_file(g.settings_path, &a);                        /* keep the scratch settings already stored there */
    if (g.project_path[0]) {
        Ini p;
        ini_init(&p);
        settings_to_ini(&p, 1);
        project_to_ini(&p);
        write_ini_file(g.project_path, &p, 1);
        ini_free(&p);
        ini_set(&a, L"App", L"LastProject", g.project_path);
        ini_set_int(&a, L"Editor", L"HomeEndJumpsToDocument", g.home_end_doc ? 1 : 0);
    } else {
        settings_to_ini(&a, 0);
        ini_set(&a, L"App", L"LastProject", L"");
    }
    profiles_to_ini(&g.profiles, &a);                           /* profiles are global, never per project */
    ext_to_ini(&a);                                             /* so are tray, shortcut and caption settings */
    write_ini_file(g.settings_path, &a, 0);
    ini_free(&a);
}

/* Scratch settings from settings.txt. Returns 1 if the saved voice is gone. */
static int load_settings(void)
{
    Ini a;
    read_ini_file(g.settings_path, &a);
    int missing = settings_from_ini(&a, 0);
    ini_free(&a);
    return missing;
}

/* ---------------------------------------------------------------- playback */

static void stop_playback(const wchar_t *why)
{
    player_stop(&g.player);
    KillTimer(g.hwnd, POLL_TIMER_ID);
    g.state = ST_IDLE;
    update_controls();
    set_status(why ? why : L"Stopped.");
}

/* Speak sentences first..last, highlighting as it goes. Works from idle,
 * paused or playing (used for navigation too: it replaces the queue). */
static void begin_playback_at(int first, int last, double frac)
{
    wchar_t err[TTS_ERR_MAX];
    ensure_doc();
    if (g.doc.nsent == 0) { set_status(L"Nothing to speak."); return; }
    if (first < 0) first = 0;
    if (first >= g.doc.nsent) first = g.doc.nsent - 1;
    if (last < first) last = first;
    if (last >= g.doc.nsent) last = g.doc.nsent - 1;

    TTSSpeechSettings s = current_settings();
    if (!tts_select_voice(g.engine, selected_voice_id(), err, TTS_ERR_MAX) ||
        !tts_apply_settings(g.engine, &s, err, TTS_ERR_MAX) ||
        !player_start_at(&g.player, &g.doc, &g.speech, first, last, frac, err, TTS_ERR_MAX)) {
        stop_playback(L"Stopped.");
        error_box(err);
        return;
    }
    g.play_last = last;
    g.state = ST_PLAYING;
    show_sentence(first);
    SetTimer(g.hwnd, POLL_TIMER_ID, 40, NULL);   /* only runs while speaking; short, so sentence changes and captions are not late */
    update_controls();
    set_status(L"Speaking...");
}

static void begin_playback(int first, int last) { begin_playback_at(first, last, 0.0); }

static void rate_dirty_resume(void);
static void X_note_pause(int paused);
static int X_rate_dirty(void);

static void toggle_play(void)
{
    switch (g.state) {
    case ST_IDLE: {
        int i = caret_sentence();
        if (i < 0) { set_status(L"Nothing to speak."); return; }
        begin_playback(i, g.doc.nsent - 1);
        break;
    }
    case ST_PLAYING: tts_pause(g.engine);  X_note_pause(1); g.state = ST_PAUSED;  update_controls(); set_status(L"Paused."); break;
    case ST_PAUSED:
        if (X_rate_dirty()) { rate_dirty_resume(); break; }
        tts_resume(g.engine); X_note_pause(0); g.state = ST_PLAYING; update_controls(); set_status(L"Speaking..."); break;
    default: break;
    }
}

/* F5 and the Play button: while reading, a cursor that was moved to another sentence restarts reading there; otherwise pause/resume. */
static void play_from_cursor(void)
{
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) {
        int i = caret_sentence();
        if (i >= 0 && i != g.cur_sent) { begin_playback(i, g.doc.nsent - 1); return; }
    }
    toggle_play();
}

/* The sentence "we are at": the spoken one while playing, else the caret's. */
static int here(void)
{
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) return g.cur_sent;
    return caret_sentence();
}

/* Jump to a sentence: restarts playback there if playing, else just selects it. */
static void goto_sentence(int target)
{
    ensure_doc();
    if (g.doc.nsent == 0) return;
    if (target < 0) target = 0;
    if (target >= g.doc.nsent) target = g.doc.nsent - 1;
    if (g.state == ST_PLAYING || g.state == ST_PAUSED)
        begin_playback(target, g.play_last > target ? g.play_last : g.doc.nsent - 1);
    else
        show_sentence(target);
}

static void nav_sentence(int delta)
{
    int i = here();
    if (i < 0) return;
    goto_sentence(i + delta);
}

static void nav_paragraph(int delta)
{
    int i = here();
    if (i < 0) return;
    const Paragraph *pg = &g.doc.paras[g.doc.sents[i].para];
    int p = g.doc.sents[i].para;
    int target;
    if (delta < 0) {
        if (i > pg->first_sentence) target = pg->first_sentence;       /* start of this paragraph */
        else if (p > 0) target = g.doc.paras[p - 1].first_sentence;
        else target = 0;
    } else {
        target = (p + 1 < g.doc.npara) ? g.doc.paras[p + 1].first_sentence : g.doc.nsent - 1;
    }
    goto_sentence(target);
}

static void play_sentence_only(void)
{
    int i = here();
    if (i >= 0) begin_playback(i, i);
}

static void play_paragraph(void)
{
    int i = here();
    if (i < 0) return;
    const Paragraph *pg = &g.doc.paras[g.doc.sents[i].para];
    begin_playback(pg->first_sentence, pg->first_sentence + pg->sentence_count - 1);
}

static void play_selection(void)
{
    ensure_doc();
    DWORD sa = 0, sb = 0;
    SendMessageW(g.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    if (sb <= sa) { set_status(L"No selection to play."); return; }
    int a = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sa));
    int b = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sb - 1));
    if (a < 0) { set_status(L"Nothing to speak."); return; }
    begin_playback(a, b < a ? a : b);
}

static void replay_sentence(void)
{
    int i = here();
    if (i < 0) return;
    begin_playback(i, g.state == ST_IDLE ? g.doc.nsent - 1 : g.play_last);
}

static void on_poll_timer(void)
{
    wchar_t err[TTS_ERR_MAX];
    int cur = g.cur_sent;
    if (g.state != ST_PLAYING && g.state != ST_PAUSED) { KillTimer(g.hwnd, POLL_TIMER_ID); return; }
    sleep_check();
    if (g.state != ST_PLAYING && g.state != ST_PAUSED) return;
    PlayerState ps = player_poll(&g.player, &cur, err, TTS_ERR_MAX);
    if (ps == PLAYER_RUNNING) {
        if (cur != g.cur_sent) show_sentence(cur);
        caption_follow();
    } else if (ps == PLAYER_FINISHED) {
        show_sentence(cur);
        stop_playback(L"Finished.");
    } else if (ps == PLAYER_ERROR) {
        stop_playback(L"Speech error.");
        error_box(err);
    }
}

/* ------------------------------------------------------------- generate audio */

static void gen_changed(void *ctx)
{
    (void)ctx;
    save_settings();
}

/* mode 0: whole document, 1: the selection, 2: the chapter at the cursor. */
static void open_generate(int mode)
{
    if (g.state != ST_IDLE) stop_playback(NULL);
    ensure_doc();
    if (!g.cache_ok) { error_box(L"The audio cache could not be opened, so audio cannot be generated.\nCheck that the project folder or %APPDATA%\\Mouthpiece is writable."); return; }
    if (g.doc.nsent == 0) { set_status(L"Nothing to export."); return; }
    int first = 0, last = g.doc.nsent - 1;
    static wchar_t label[300];
    const wchar_t *range_label = L"the whole document";
    if (mode == 1) {
        DWORD sa = 0, sb = 0;
        SendMessageW(g.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
        if (sb <= sa) { set_status(L"No selection to export."); return; }
        first = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sa));
        last = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sb - 1));
        if (first < 0) return;
        if (last < first) last = first;
        range_label = L"the selection";
    } else if (mode == 2) {
        if (g.chapters.n == 0) { set_status(L"No chapters were detected. Tools > Navigator can add headings."); return; }
        int ch = chapter_of_sentence(&g.chapters, caret_sentence());
        first = chapter_first_sentence(&g.chapters, &g.doc, ch);
        last = ch < 0 ? g.chapters.c[0].first_sent - 1 : chapter_last_sentence(&g.chapters, &g.doc, ch);
        if (first < 0 || last < first) { set_status(L"That chapter has nothing to speak."); return; }
        _snwprintf(label, 300, L"the chapter \"%ls\"", ch < 0 ? L"(before the first chapter)" : g.chapters.c[ch].title);
        label[299] = 0;
        range_label = label;
    }
    static GenUi ui;
    memset(&ui, 0, sizeof ui);
    ui.font = g.font; ui.dpi = g.dpi;
    ui.cache = &g.cache; ui.doc = &g.doc; ui.speech = &g.speech; ui.chapters = &g.chapters;
    const wchar_t *vid = selected_voice_id();
    if (vid) wcsncpy(ui.synth.voice_id, vid, TTS_ID_MAX - 1);
    TTSSpeechSettings st = current_settings();
    ui.synth.rate = st.rate; ui.synth.volume = st.volume; ui.synth.pitch = 0;
    ui.synth.engine_id = L"sapi5";
    ui.synth.wav_rate = TTS_WAV_RATE;
    ui.first_sent = first; ui.last_sent = last;
    ui.range_label = range_label;
    ui.settings = &g.gen;
    if (g.last_out[0]) wcsncpy(ui.out_path, g.last_out, MAX_PATH - 1);
    else if (g.project_path[0]) _snwprintf(ui.out_path, MAX_PATH, L"%ls\\export\\%ls.wav", g.project_dir, g.project_name);
    else wcscpy(ui.out_path, L"audiobook.wav");
    if (g.last_dir[0]) wcsncpy(ui.out_dir, g.last_dir, MAX_PATH - 1);
    else if (g.project_path[0]) _snwprintf(ui.out_dir, MAX_PATH, L"%ls\\export", g.project_dir);
    ui.changed = gen_changed;
    generate_dialog(dlg_parent(), &ui);
    wcsncpy(g.last_out, ui.out_path, MAX_PATH - 1);
    wcsncpy(g.last_dir, ui.out_dir, MAX_PATH - 1);
    save_settings();
    set_status(L"Ready.");
}

/* ------------------------------------------------------ pronunciation / speech UI */

static void ui_speak(void *ctx, const wchar_t *text)
{
    (void)ctx;
    wchar_t err[TTS_ERR_MAX];
    TTSSpeechSettings st = current_settings();
    unsigned long id = 0;
    tts_stop(g.engine);
    if (!tts_select_voice(g.engine, selected_voice_id(), err, TTS_ERR_MAX) ||
        !tts_apply_settings(g.engine, &st, err, TTS_ERR_MAX) ||
        !tts_enqueue(g.engine, text, &id, err, TTS_ERR_MAX))
        error_box(err);
}

static void save_dictionary(void)
{
    wchar_t err[TTS_ERR_MAX];
    size_t len = 0;
    char *bytes = pron_to_utf8(&g.dict, &len);
    if (!bytes) { error_box(L"Out of memory saving the pronunciation dictionary."); return; }
    if (g.dict_path[0] && !util_write_file(g.dict_path, bytes, len, err, TTS_ERR_MAX)) error_box(err);
    free(bytes);
}

static void ui_changed(void *ctx)
{
    (void)ctx;
    pron_resolve_scopes(&g.dict, &g.doc, &g.chapters);
    pron_prepare(&g.dict);
    save_dictionary();
    save_settings();
}

static void load_dictionary(void)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    int missing = 0;
    wchar_t err[TTS_ERR_MAX];
    pron_init(&g.dict);
    g.speech.dict = &g.dict;
    if (!g.dict_path[0]) return;
    if (!util_read_file(g.dict_path, &buf, &len, &missing, err, TTS_ERR_MAX)) {
        if (!missing) error_box(err);                      /* a missing file is normal on first run */
        return;
    }
    int bad = 0;
    pron_from_utf8(&g.dict, (const char *)buf, len, &bad);
    free(buf);
    if (bad) {
        wchar_t m[160];
        _snwprintf(m, 160, L"%d line(s) of the pronunciation dictionary could not be read and were skipped.", bad);
        m[159] = 0;
        set_status(m);
    }
}

static void make_speech_ui(SpeechUi *u)
{
    memset(u, 0, sizeof *u);
    u->font = g.font; u->dpi = g.dpi;
    u->dict = &g.dict; u->opts = &g.speech.opts; u->doc = &g.doc;
    u->speak = ui_speak;
    u->changed = ui_changed;
}

static int begin_modal_dialog(void)
{
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) stop_playback(NULL);
    ensure_doc();
    return 1;
}

static void status_cb(const wchar_t *m) { set_status(m); }

static void prof_current(void *ctx, VoiceProfile *o)
{
    (void)ctx;
    const wchar_t *id = selected_voice_id();
    wcsncpy(o->voice_id, id ? id : L"", 255);
    TTSSpeechSettings st = current_settings();
    o->rate = st.rate;
    o->volume = st.volume;
}

static int prof_apply(void *ctx, const VoiceProfile *p)
{
    (void)ctx;
    int found = 0;
    for (int i = 0; i < g.voices.count; i++)
        if (wcscmp(g.voices.items[i].id, p->voice_id) == 0) { SendMessageW(g.voice, CB_SETCURSEL, (WPARAM)i, 0); found = 1; break; }
    SendMessageW(g.rate, TBM_SETPOS, TRUE, p->rate);
    SendMessageW(g.vol, TBM_SETPOS, TRUE, p->volume);
    update_value_labels();
    save_settings();
    return found;
}

static void prof_changed(void *ctx) { (void)ctx; save_settings(); }

static void open_profiles(void)
{
    if (g.state != ST_IDLE) stop_playback(NULL);
    ProfileUi u;
    memset(&u, 0, sizeof u);
    u.font = g.font; u.dpi = g.dpi; u.list = &g.profiles;
    u.current = prof_current; u.apply = prof_apply; u.changed = prof_changed;
    profiles_dialog(dlg_parent(), &u);
    set_status(L"Ready.");
}

static const wchar_t *pron_prefill = NULL;       /* set by the problem-word report */
static void open_pron(void);

static void open_analyze(void)
{
    if (g.state != ST_IDLE) stop_playback(NULL);
    ensure_doc();
    if (g.doc.nsent == 0) { MessageBoxW(dlg_parent(), L"There is no text to analyze.", L"Problem words", MB_OK | MB_ICONINFORMATION); return; }
    Analysis a;
    if (!analyze_document(&a, &g.doc, &g.chapters, &g.dict, &g.speech.opts)) { MessageBoxW(dlg_parent(), L"Out of memory.", L"Problem words", MB_OK | MB_ICONERROR); return; }
    int idx = -1;
    int act = analyze_dialog(dlg_parent(), g.font, g.dpi, &a, &idx);
    if (act == ANALYZE_GOTO && idx >= 0) goto_sentence(a.issues[idx].first_sent);
    else if (act == ANALYZE_PRON && idx >= 0) {
        wchar_t tok[48];
        wcsncpy(tok, a.issues[idx].token, 47); tok[47] = 0;
        pron_prefill = tok;
        open_pron();
        pron_prefill = NULL;
    }
    analysis_free(&a);
    set_status(L"Ready.");
}

static void open_find(void)
{
    FindUi f;
    f.edit = g.edit; f.font = g.font; f.dpi = g.dpi; f.status = status_cb;
    wchar_t pre[200];
    pre[0] = 0;
    DWORD sa = 0, sb = 0;
    SendMessageW(g.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    if (sb > sa && sb - sa < 150) {
        int len = GetWindowTextLengthW(g.edit);
        wchar_t *all = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
        if (all) {
            GetWindowTextW(g.edit, all, len + 1);
            if (sb <= (DWORD)len) { memcpy(pre, all + sa, (sb - sa) * sizeof(wchar_t)); pre[sb - sa] = 0; }
            free(all);
        }
    }
    for (wchar_t *p = pre; *p; p++) if (*p == L'\r' || *p == L'\n') { *p = 0; break; }
    find_dialog_open(g.hwnd, &f, pre);
}

static void open_pron(void)
{
    if (!begin_modal_dialog()) return;
    wchar_t word[200];
    word[0] = 0;
    DWORD sa = 0, sb = 0;
    SendMessageW(g.edit, EM_GETSEL, (WPARAM)&sa, (LPARAM)&sb);
    if (sb > sa && sb - sa < 150) {                            /* prefill with a short selection */
        int len = GetWindowTextLengthW(g.edit);
        wchar_t *all = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
        if (all) {
            GetWindowTextW(g.edit, all, len + 1);
            if (sb <= (DWORD)len) {
                size_t k = 0;
                for (DWORD i = sa; i < sb && k < 199; i++) word[k++] = (all[i] == L'\r' || all[i] == L'\n' || all[i] == L'\t') ? L' ' : all[i];
                word[k] = 0;
            }
            free(all);
        }
    }
    if (pron_prefill) { wcsncpy(word, pron_prefill, 199); word[199] = 0; }
    SpeechUi ui;
    make_speech_ui(&ui);
    /* scopes: the chapter at the cursor, and the selected sentences */
    wchar_t *sc_sel = NULL, *sc_ch = NULL;
    int cs = caret_sentence();
    int chi = cs >= 0 ? chapter_of_sentence(&g.chapters, cs) : -1;
    if (chi >= 0 && g.chapters.c[chi].title) { sc_ch = pronscope_chapter(g.chapters.c[chi].title); ui.chapter_label = g.chapters.c[chi].title; }
    if (sb > sa && g.doc.nsent > 0) {
        int a = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sa));
        int b = doc_sentence_at(&g.doc, preproc_src_to_norm(&g.pre, sb > sa ? sb - 1 : sb));
        if (a >= 0 && b >= a) sc_sel = pronscope_range(&g.doc, a, b);
    }
    ui.scope_chapter = sc_ch;
    ui.scope_selection = sc_sel;
    pron_dialog(dlg_parent(), &ui, word);
    free(sc_ch);
    free(sc_sel);
    tts_stop(g.engine);                                        /* cut any test speech */
    set_status(L"Pronunciation dictionary saved.");
}

static void open_speech_preview(void)
{
    if (!begin_modal_dialog()) return;
    SpeechUi ui;
    make_speech_ui(&ui);
    speech_preview_dialog(dlg_parent(), &ui);
    tts_stop(g.engine);
    set_status(L"Ready.");
}

/* ----------------------------------------------------------------- cleanup UI */

static void open_cleanup(void)
{
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) stop_playback(NULL);
    int len = GetWindowTextLengthW(g.edit);
    wchar_t *buf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!buf) { error_box(L"Out of memory."); return; }
    buf[0] = 0;
    if (len > 0) GetWindowTextW(g.edit, buf, len + 1);
    PreprocOptions o = g.opts;
    if (cleanup_dialog(dlg_parent(), g.font, g.dpi, buf, (size_t)len, &o)) {
        g.opts = o;
        save_settings();
        g.doc_dirty = 1;
        ensure_doc();
        update_position(caret_sentence());
        set_status(L"Cleanup options applied.");
    }
    free(buf);
}

/* ---------------------------------------------------------------- file open */

static void load_text_path(const wchar_t *path);

static void open_file(void)
{
    if (g.state != ST_IDLE) return;
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    load_text_path(path);
}

static void load_text_path(const wchar_t *path)
{
    wchar_t err[TTS_ERR_MAX];
    wchar_t *text = NULL;
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    if (!util_load_text_file(path, &text, err, TTS_ERR_MAX)) { error_box(err); return; }
    SetWindowTextW(g.edit, text);
    free(text);
    update_char_count();
    set_status(L"File loaded.");
}

/* ----------------------------------------------------- projects, chapters, bookmarks */

static void project_save(void);

static void update_title(void)
{
    wchar_t t[300];
    if (g.project_path[0]) _snwprintf(t, 300, L"%ls - %ls%ls", APP_TITLE, g.project_name, g.dirty ? L" *" : L"");
    else _snwprintf(t, 300, L"%ls", APP_TITLE);
    t[299] = 0;
    SetWindowTextW(g.hwnd, t);
}

static int open_cache(void)
{
    wchar_t cerr[256];
    if (g.cache_ok) { cache_close(&g.cache); g.cache_ok = 0; }
    g.cache_ok = g.cache_root[0] && cache_open(&g.cache, g.cache_root, cerr, 256);
    if (!g.cache_ok && g.cache_root[0]) set_status(cerr);
    return g.cache_ok;
}

/* Point the pronunciation dictionary and the audio cache at another location. */
static void switch_storage(const wchar_t *dict, const wchar_t *cache)
{
    pron_free(&g.dict);
    wcsncpy(g.dict_path, dict, MAX_PATH - 1);
    wcsncpy(g.cache_root, cache, MAX_PATH - 1);
    load_dictionary();
    open_cache();
}

static void path_join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", dir, name);
    out[MAX_PATH - 1] = 0;
}

static void split_project_path(const wchar_t *path, wchar_t *dir, wchar_t *name, size_t namecap)
{
    wcsncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    const wchar_t *file = path;
    if (slash) { file = path + (slash - dir) + 1; *slash = 0; } else wcscpy(dir, L".");
    wcsncpy(name, file, namecap - 1);
    name[namecap - 1] = 0;
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot && dot != name) *dot = 0;
}

/* Editor text -> UTF-8 file with LF line ends (what the project stores as the source). */
static int save_source(const wchar_t *path)
{
    int len = GetWindowTextLengthW(g.edit);
    wchar_t *w = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!w) { error_box(L"Out of memory saving the text."); return 0; }
    w[0] = 0;
    if (len > 0) GetWindowTextW(g.edit, w, len + 1);
    size_t j = 0;
    for (int i = 0; i < len; i++) { if (w[i] == L'\r' && i + 1 < len && w[i + 1] == L'\n') continue; w[j++] = w[i]; }
    size_t bytes = utf8_from_wide(w, j, NULL);
    char *u = (char *)malloc(bytes + 1);
    if (!u) { free(w); error_box(L"Out of memory saving the text."); return 0; }
    utf8_from_wide(w, j, u);
    wchar_t err[TTS_ERR_MAX];
    int ok = util_write_file(path, u, bytes, err, TTS_ERR_MAX);
    if (!ok) error_box(err);
    free(u); free(w);
    return ok;
}

static int confirm_discard(void)
{
    if (!g.project_path[0] || !g.dirty) return 1;
    wchar_t m[300];
    _snwprintf(m, 300, L"Save the changes to the text of project \"%ls\"?", g.project_name);
    m[299] = 0;
    int r = MessageBoxW(dlg_parent(), m, APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) project_save();
    return 1;
}

static void project_new(void)
{
    if (g.state != ST_IDLE) stop_playback(NULL);
    wchar_t path[MAX_PATH] = L"book.ttsp";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Mouthpiece project (*.ttsp)\0*.ttsp\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"ttsp";
    ofn.lpstrTitle = L"New project - choose a (preferably empty) folder and a name";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    wchar_t dir[MAX_PATH], name[128], p[MAX_PATH];
    split_project_path(path, dir, name, 128);
    path_join(p, dir, L"export");
    CreateDirectoryW(p, NULL);
    wchar_t dict[MAX_PATH], cache[MAX_PATH], src[MAX_PATH];
    path_join(dict, dir, L"pronunciations.txt");
    path_join(cache, dir, L"cache");
    path_join(src, dir, L"source.txt");
    if (!save_source(src)) return;
    wcsncpy(g.project_path, path, MAX_PATH - 1);
    wcsncpy(g.project_dir, dir, MAX_PATH - 1);
    wcsncpy(g.project_name, name, 127);
    wcsncpy(g.dict_path, dict, MAX_PATH - 1);            /* the current dictionary moves into the project */
    save_dictionary();
    wcsncpy(g.cache_root, cache, MAX_PATH - 1);
    open_cache();
    g.last_out[0] = g.last_dir[0] = 0;
    g.dirty = 0;
    save_settings();
    update_title();
    set_status(L"Project created. Its text, dictionary, audio cache and settings now live in its folder.");
}

static int project_open_path(const wchar_t *path)
{
    Ini ini;
    if (!read_ini_file(path, &ini) || !ini_get(&ini, L"Project", L"Version")) {
        wchar_t m[MAX_PATH + 80];
        _snwprintf(m, MAX_PATH + 80, L"This is not a readable Mouthpiece project:\n%ls", path);
        m[MAX_PATH + 79] = 0;
        error_box(m);
        ini_free(&ini);
        return 0;
    }
    wchar_t dir[MAX_PATH], name[128], src[MAX_PATH], err[TTS_ERR_MAX], *text = NULL;
    split_project_path(path, dir, name, 128);
    const wchar_t *sf = ini_get(&ini, L"Project", L"Source");
    path_join(src, dir, sf && *sf ? sf : L"source.txt");
    if (!util_load_text_file(src, &text, err, TTS_ERR_MAX)) { error_box(err); ini_free(&ini); return 0; }

    if (g.state != ST_IDLE) stop_playback(NULL);
    wcsncpy(g.project_path, path, MAX_PATH - 1);
    wcsncpy(g.project_dir, dir, MAX_PATH - 1);
    wcsncpy(g.project_name, name, 127);
    wchar_t dict[MAX_PATH], cache[MAX_PATH], exp[MAX_PATH];
    path_join(dict, dir, L"pronunciations.txt");
    path_join(cache, dir, L"cache");
    path_join(exp, dir, L"export");
    CreateDirectoryW(exp, NULL);
    switch_storage(dict, cache);
    int missing = settings_from_ini(&ini, 1);
    overrides_from_ini(&g.ov, &ini);
    bm_from_ini(&g.bm, &ini);
    SetWindowTextW(g.edit, text);
    free(text);
    g.dirty = 0;
    g.doc_dirty = 1;
    ensure_doc();
    Bookmark pos;
    int restored = 0;
    if (bm_anchor_from_ini(&pos, &ini, L"Position")) {       /* resume where the user left off */
        int sent = bm_resolve(&pos, &g.doc, &g.chapters);
        bm_destroy(&pos);
        if (sent >= 0) { show_sentence(sent); restored = 1; }
    }
    ini_free(&ini);
    update_controls();
    update_char_count();
    update_title();
    save_settings();
    set_status(missing ? L"Project opened. Its voice is no longer installed; using the first available voice."
                       : (restored ? L"Project opened at the position where you left off." : L"Project opened."));
    return 1;
}

static void project_open_dialog(void)
{
    if (!confirm_discard()) return;
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Mouthpiece project (*.ttsp)\0*.ttsp\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) project_open_path(path);
}

static void project_save(void)
{
    if (!g.project_path[0]) { project_new(); return; }
    wchar_t src[MAX_PATH];
    path_join(src, g.project_dir, L"source.txt");
    if (!save_source(src)) return;
    g.dirty = 0;
    save_settings();
    update_title();
    set_status(L"Project saved.");
}

/* Reset to an empty scratch document: no project, no text, no chapters/bookmarks/corrections. */
static void reset_to_blank(void)
{
    if (g.state != ST_IDLE) stop_playback(NULL);
    g.project_path[0] = g.project_dir[0] = g.project_name[0] = 0;
    g.dirty = 0;
    overrides_free(&g.ov);
    bm_clear(&g.bm);
    load_settings();                                      /* back to the scratch settings */
    switch_storage(g.scratch_dict, g.scratch_cache);
    SetWindowTextW(g.edit, L"");
    g.cur_sent = -1;
    g.doc_dirty = 1;
    ensure_doc();
    update_position(-1);
    update_char_count();
    update_controls();
    update_title();
    save_settings();
    refresh_gutter();
}

static void project_close(void)
{
    if (!g.project_path[0]) return;
    if (!confirm_discard()) return;
    reset_to_blank();
    set_status(L"Project closed. You have a new blank document.");
}

/* File > New: a blank scratch document (closes the current project if there is one). */
static void new_document(void)
{
    if (g.project_path[0]) { project_close(); return; }
    if (GetWindowTextLengthW(g.edit) > 0 &&
        MessageBoxW(dlg_parent(), L"Discard the current text and start a new blank document?\n\n"
                            L"(The text is not saved unless you made a project from it or saved it yourself.)",
                    APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    reset_to_blank();
    set_status(L"New blank document.");
}

/* Tools > Data location: where settings, the scratch dictionary and cache are kept. */
static int pick_folder(wchar_t *out)
{
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g.hwnd;
    bi.lpszTitle = L"Choose the folder for Mouthpiece settings, pronunciations and cache";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;
    int ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok;
}

static void write_pointer(const wchar_t *folder)
{
    wchar_t ad[MAX_PATH], ptr[MAX_PATH], err[TTS_ERR_MAX];
    appdata_dir(ad);
    CreateDirectoryW(ad, NULL);
    _snwprintf(ptr, MAX_PATH, L"%ls\\datadir.txt", ad);
    ptr[MAX_PATH - 1] = 0;
    if (!folder) { DeleteFileW(ptr); return; }
    char u[MAX_PATH * 4];
    int n = WideCharToMultiByte(CP_UTF8, 0, folder, -1, u, (int)sizeof u, NULL, NULL);
    if (n > 1 && !util_write_file(ptr, u, (size_t)n - 1, err, TTS_ERR_MAX)) error_box(err);
}

static void data_location(void)
{
    wchar_t m[1400];
    const wchar_t *how = g.data_how == 2 ? L"portable mode (a MouthpieceData folder next to the program)"
                       : g.data_how == 1 ? L"a folder you chose" : L"the default (%APPDATA%\\Mouthpiece)";
    _snwprintf(m, 1400,
        L"Settings, the global pronunciation dictionary and the audio cache for unsaved text are kept in:\n\n%ls\n\n"
        L"This is %ls.\n\n"
        L"Projects (.ttsp) keep their own text, settings, dictionary and cache inside the project folder; "
        L"this location is only for everything outside a project.\n\n"
        L"Yes = choose another folder (applies after you restart; existing files are copied there)\n"
        L"No = open this folder in Explorer\n"
        L"Cancel = close\n\n"
        L"Portable mode: create a folder named MouthpieceData next to the program and it is used automatically.",
        g.app_dir[0] ? g.app_dir : L"(unavailable)", how);
    m[1399] = 0;
    int r = MessageBoxW(dlg_parent(), m, L"Data location", MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (r == IDNO && g.app_dir[0]) {
        ShellExecuteW(g.hwnd, L"open", g.app_dir, NULL, NULL, SW_SHOWNORMAL);
    } else if (r == IDYES) {
        wchar_t f[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
        if (!pick_folder(f)) return;
        save_settings();
        static const wchar_t *const files[] = { L"settings.txt", L"pronunciations.txt" };
        for (int i = 0; i < 2 && g.app_dir[0]; i++) {
            _snwprintf(from, MAX_PATH, L"%ls\\%ls", g.app_dir, files[i]);
            _snwprintf(to, MAX_PATH, L"%ls\\%ls", f, files[i]);
            from[MAX_PATH - 1] = to[MAX_PATH - 1] = 0;
            CopyFileW(from, to, TRUE);                  /* never overwrite what is already there */
        }
        write_pointer(f);
        MessageBoxW(dlg_parent(), L"Saved. The new location is used the next time you start the program.\n"
                            L"(Cached audio is not moved; it is simply regenerated when needed.)",
                    L"Data location", MB_OK | MB_ICONINFORMATION);
    }
}

static void data_location_reset(void)
{
    if (g.data_how == 2) {
        MessageBoxW(dlg_parent(), L"Portable mode is active (a MouthpieceData folder next to the program). "
                            L"Rename or remove that folder to stop using it.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    write_pointer(NULL);
    MessageBoxW(dlg_parent(), L"The default location (%APPDATA%\\Mouthpiece) will be used the next time you start the program.",
                L"Data location", MB_OK | MB_ICONINFORMATION);
}

/* ---- chapters ---- */

static void redetect_cb(void *ctx)
{
    (void)ctx;
    chapters_free(&g.chapters);
    if (!chapters_detect(&g.chapters, &g.doc, &g.ov)) memset(&g.chapters, 0, sizeof g.chapters);
    save_settings();
    update_position(g.cur_sent >= 0 ? g.cur_sent : caret_sentence());
}

static void nav_chapter(int delta)
{
    ensure_doc();
    int i = here();
    if (i < 0) return;
    if (g.chapters.n == 0) { set_status(L"No chapters were detected. Tools > Navigator can add headings."); return; }
    int ch = chapter_of_sentence(&g.chapters, i), target;
    if (delta < 0) {
        int start = chapter_first_sentence(&g.chapters, &g.doc, ch);
        if (i > start) target = start;
        else if (ch > 0) target = g.chapters.c[ch - 1].first_sent;
        else target = 0;
    } else {
        target = (ch + 1 < g.chapters.n) ? g.chapters.c[ch + 1].first_sent : g.doc.nsent - 1;
    }
    jump_to(target);
}

static void play_chapter(void)
{
    ensure_doc();
    int i = here();
    if (i < 0) return;
    if (g.chapters.n == 0) { set_status(L"No chapters were detected. Tools > Navigator can add headings."); return; }
    int ch = chapter_of_sentence(&g.chapters, i);
    int first = chapter_first_sentence(&g.chapters, &g.doc, ch);
    int last = ch < 0 ? g.chapters.c[0].first_sent - 1 : chapter_last_sentence(&g.chapters, &g.doc, ch);
    if (first >= 0 && last >= first) begin_playback(first, last);
}

/* ---- bookmarks ---- */

static void toggle_bookmark(void) { flag_mark(MK_BOOKMARK); }

static void goto_bookmark(int dir) { goto_any_mark(dir); }

static void open_nav(void)
{
    if (!begin_modal_dialog()) return;
    NavUi ui;
    memset(&ui, 0, sizeof ui);
    ui.font = g.font; ui.dpi = g.dpi;
    ui.doc = &g.doc; ui.chapters = &g.chapters; ui.overrides = &g.ov; ui.bookmarks = &g.bm;
    ui.cursor_sent = caret_sentence();
    ui.redetect = redetect_cb;
    nav_dialog(dlg_parent(), &ui);
    refresh_gutter();
    if (ui.goto_sent >= 0) jump_to(ui.goto_sent);
    set_status(L"Ready.");
}


/* every popup menu, so that check marks can be set by command id wherever the item lives */
static HMENU g_popups[24];
static int g_npop;
static HMENU newpop(void)
{
    HMENU m = CreatePopupMenu();
    if (g_npop < 24) g_popups[g_npop++] = m;
    return m;
}
static void menu_check(int id, int on)
{
    for (int i = 0; i < g_npop; i++) CheckMenuItem(g_popups[i], (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

/* ============================================ marks, jumping, tray, global shortcuts, captions */

enum {
    CMD_FLAG_BASE = 1100,                       /* + MarkKind (1..5) */
    CMD_JUMP_BACK = 1120, CMD_JUMP_FWD, CMD_SKIP_BACK, CMD_SKIP_FWD, CMD_GOTO, CMD_MARK_NOTE,
    CMD_RATE_UP, CMD_RATE_DOWN, CMD_CAP_TOGGLE, CMD_CAP_BIGGER, CMD_CAP_SMALLER, CMD_CAP_POS, CMD_CAP_MOREOP, CMD_CAP_LESSOP,
    CMD_TRAY_MIN, CMD_TRAY_CLOSE, CMD_HK_DIALOG, CMD_HK_ENABLE, CMD_HK_HELP, CMD_SHOW, CMD_SLEEP_STOP_CH, CMD_CAP_DIALOG, CMD_CAP_MOVE,
    CMD_CHAPTERJUMP_BASE_UNUSED,
    CMD_SLEEP_BASE = 1200,                      /* + index into sleep_minutes */
    CMD_PROFILE_BASE = 1300,                    /* + profile index */
    CMD_MARKJUMP_BASE = 1500,                   /* + mark index */
    CMD_CHJUMP_BASE = 1700                      /* + chapter index */
};
static const int sleep_minutes[] = { 0, 15, 30, 45, 60, 90 };
#define TRAY_MSG (WM_APP + 1)
#define TIMER_ARM 3
#define TIMER_RATE 5
#define HK_ID_LEADER 1
#define HK_ID_ESC 2
#define HK_ID_DIRECT 100
#define HK_ID_MEDIA 300
#define HK_ID_ARMED 400


static struct {
    int ready;
    PosHist hist;
    HotkeyCfg hk;
    CaptionCfg cap;
    int tray_added, min_to_tray, close_to_tray;
    CaptionLooks looks;
    int armed;
    wchar_t hk_report[600];
    ULONGLONG sent_tick, sleep_deadline, pause_tick;
    int rate_dirty, meas_sent, meas_natural, pause_used;
    double cps_meas;                 /* measured speaking speed, characters per second (0 = unknown) */
    UINT msg_taskbar;
    HICON icons[3];
} X;

static int play_rate(void) { return current_settings().rate; }

static void X_note_pause(int paused)
{
    if (paused) { X.pause_tick = GetTickCount64(); X.pause_used = 1; }
    else if (X.pause_tick) { X.sent_tick += GetTickCount64() - X.pause_tick; X.pause_tick = 0; }
}
static int X_rate_dirty(void) { return X.rate_dirty; }

/* How far into the sentence being spoken we are (0..1), judged from time and the measured speaking speed. */
static double cur_frac(void)
{
    if (g.cur_sent < 0 || g.cur_sent >= g.doc.nsent) return 0;
    double cps = X.cps_meas > 0 ? X.cps_meas : nav_chars_per_sec(play_rate());
    double total = (double)g.doc.sents[g.cur_sent].len / cps;
    ULONGLONG ref = (g.state == ST_PAUSED && X.pause_tick) ? X.pause_tick : GetTickCount64();
    double el = (double)(ref - X.sent_tick) / 1000.0;
    if (total <= 0.2 || el <= 0) return 0;
    double f = el / total;
    return f > 1.0 ? 1.0 : f;
}

/* Restart the current sentence with the current speed, continuing from about where it was. */
static void restart_here(void)
{
    if (g.cur_sent < 0) return;
    double f = cur_frac();
    int s = g.cur_sent, last = g.play_last;
    double cps = X.cps_meas > 0 ? X.cps_meas : nav_chars_per_sec(play_rate());
    double total = (double)g.doc.sents[s].len / cps;
    X.rate_dirty = 0;
    begin_playback_at(s, last, f);
    X.meas_natural = 0;
    if (f < 0.96) X.sent_tick -= (ULONGLONG)(f * total * 1000.0);
}

static void rate_dirty_resume(void) { restart_here(); }

/* The speed changed while speaking: SAPI only applies a new speed to sentences it starts later, and a few are already
 * queued, so the change would arrive several sentences late. Restart from about the current word instead. */
static void rate_changed_live(int old_rate)
{
    TTSSpeechSettings st = current_settings();
    tts_apply_settings(g.engine, &st, NULL, 0);
    if (X.cps_meas > 0 && old_rate != st.rate) {
        double fo = 1.0 + old_rate * 0.07, fn = 1.0 + st.rate * 0.07;
        if (fo < 0.3) fo = 0.3;
        if (fn < 0.3) fn = 0.3;
        X.cps_meas *= fn / fo;
    }
    if (g.state == ST_PLAYING) SetTimer(g.hwnd, TIMER_RATE, 150, NULL);        /* wait until the slider stops moving */
    else if (g.state == ST_PAUSED) X.rate_dirty = 1;
}


/* ---- which sentence a flag applies to ---- */
static int flag_target(void)
{
    ensure_doc();
    if (g.doc.nsent == 0) return -1;
    if ((g.state == ST_PLAYING || g.state == ST_PAUSED) && g.cur_sent >= 0) {
        int s = g.cur_sent;
        /* you react to what you just heard: if this sentence only just started, the previous one is the one you meant */
        if (g.state == ST_PLAYING && s > 0 && GetTickCount64() - X.sent_tick < 1500) s--;
        return s;
    }
    return caret_sentence();
}

static void say(const wchar_t *m, int popup_ms)
{
    set_status(m);
    if (popup_ms > 0) caption_notice(m, popup_ms);
}

static void flag_mark(int kind)
{
    int s = flag_target();
    if (s < 0) { set_status(L"Nothing to mark."); return; }
    wchar_t msg[220];
    int i = bm_find_kind_at(&g.bm, &g.doc, &g.chapters, s, kind);
    if (i >= 0) {
        bm_remove(&g.bm, i);
        _snwprintf(msg, 220, L"%ls mark removed.", mark_kind_name(kind));
    } else {
        int n = bm_add_kind(&g.bm, &g.doc, &g.chapters, s, kind, NULL);
        if (n >= 0) _snwprintf(msg, 220, L"%ls: \"%.60ls\"", kind == MK_BOOKMARK ? L"Bookmarked" : mark_kind_name(kind), g.bm.b[n].snippet);
        else _snwprintf(msg, 220, L"Could not add the mark.");
    }
    msg[219] = 0;
    say(msg, 1800);
    save_settings();
    refresh_gutter();
}

static int mark_at_sentence(int s)
{
    int best = -1;
    for (int i = 0; i < g.bm.n; i++) if (bm_resolve(&g.bm.b[i], &g.doc, &g.chapters) == s) best = i;
    return best;
}

/* returns 1 if a note was saved (or removed), 0 if the box was cancelled */
static int edit_mark_note_idx(int i)
{
    if (i < 0 || i >= g.bm.n) return 0;
    wchar_t t[300];
    if (!input_box(dlg_parent(), g.font, g.dpi, L"Note on this mark", L"Note (leave empty to remove it):", g.bm.b[i].note ? g.bm.b[i].note : L"", t, 300)) return 0;
    bm_set_note(&g.bm.b[i], t);
    save_settings();
    refresh_gutter();
    return 1;
}

/* A note needs a mark to hang on. On a passage with no mark yet, a "revisit" flag is added first (and taken away again if the
 * note box is cancelled). */
static void cmd_mark_note(void)
{
    int s = flag_target();
    if (s < 0) { set_status(L"Nothing to mark."); return; }
    int i = mark_at_sentence(s);
    if (i >= 0) { edit_mark_note_idx(i); return; }
    int n = bm_add_kind(&g.bm, &g.doc, &g.chapters, s, MK_REVISIT, NULL);
    if (n < 0) { set_status(L"Could not add the mark."); return; }
    if (edit_mark_note_idx(n)) say(L"Flagged: Revisit, with a note.", 1800);
    else { bm_remove(&g.bm, n); refresh_gutter(); save_settings(); }
}

static void redetect_cb(void *ctx);

static void chapter_context(int ci, int x, int y)
{
    if (ci < 0 || ci >= g.chapters.n) return;
    const Chapter *c = &g.chapters.c[ci];
    HMENU m = CreatePopupMenu(), fl = CreatePopupMenu();
    wchar_t head[200];
    _snwprintf(head, 200, L"Chapter %d: %ls", ci + 1, c->title ? c->title : L"");
    head[199] = 0;
    for (wchar_t *p = head; *p; p++) if (*p == L'&') *p = L'+';
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, head);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1, L"Go to this chapter");
    AppendMenuW(m, MF_STRING, 2, L"Play from here");
    AppendMenuW(m, MF_STRING, 3, L"Rename chapter...");
    AppendMenuW(m, MF_STRING, 4, L"Remove this heading (not a chapter)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int k = 0; k < MK_COUNT; k++) {
        wchar_t it[60];
        _snwprintf(it, 60, k == 0 ? L"Bookmark" : L"Flag: %ls", mark_kind_name(k));
        AppendMenuW(fl, MF_STRING, (UINT_PTR)(10 + k), it);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fl, L"Flag the start of this chapter");
    AppendMenuW(m, MF_STRING, 5, L"Add a bookmark with a note...");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, g.hwnd, NULL);
    DestroyMenu(m);
    int first = c->first_sent, para = c->para;
    if (cmd == 1) jump_to(first);
    else if (cmd == 2) { poshist_jump(&X.hist, here()); begin_playback(first, g.doc.nsent - 1); }
    else if (cmd == 3) {
        wchar_t t[160];
        if (input_box(dlg_parent(), g.font, g.dpi, L"Rename chapter", L"Chapter name:", c->title ? c->title : L"", t, 160) && t[0]) {
            overrides_rename(&g.ov, &g.doc, para, t);
            redetect_cb(NULL);
            refresh_gutter();
        }
    } else if (cmd == 4) {
        overrides_remove_heading(&g.ov, &g.doc, para);
        redetect_cb(NULL);
        refresh_gutter();
        set_status(L"Heading removed from the chapter list.");
    } else if (cmd >= 10 && cmd < 10 + MK_COUNT) {
        int r = bm_add_kind(&g.bm, &g.doc, &g.chapters, first, cmd - 10, NULL);
        if (r == -2) set_status(L"That chapter start already has this mark.");
        else if (r >= 0) { save_settings(); refresh_gutter(); set_status(L"Marked the start of the chapter."); }
    } else if (cmd == 5) {
        wchar_t t[300];
        if (input_box(dlg_parent(), g.font, g.dpi, L"Note on this chapter", L"Note:", L"", t, 300) && t[0]) {
            int r = bm_add_kind(&g.bm, &g.doc, &g.chapters, first, MK_BOOKMARK, t);
            if (r == -2) { int i = bm_find_kind_at(&g.bm, &g.doc, &g.chapters, first, MK_BOOKMARK); if (i >= 0) bm_set_note(&g.bm.b[i], t); }
            save_settings(); refresh_gutter();
        }
    }
}

static void gutter_context(int tag, int x, int y)
{
    if (tag >= 100000) { chapter_context(tag - 100000, x, y); return; }
    if (tag < 0 || tag >= g.bm.n) return;
    Bookmark *b = &g.bm.b[tag];
    HMENU m = CreatePopupMenu(), kinds = CreatePopupMenu();
    wchar_t head[200];
    _snwprintf(head, 200, L"%ls%ls%ls", mark_kind_name(b->kind), b->note ? L": " : L"", b->note ? b->note : L"");
    head[199] = 0;
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, head);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1, L"Go to this passage");
    AppendMenuW(m, MF_STRING, 2, L"Edit note...");
    for (int k = 0; k < MK_COUNT; k++) AppendMenuW(kinds, MF_STRING | (k == b->kind ? MF_CHECKED : 0), (UINT_PTR)(10 + k), mark_kind_name(k));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)kinds, L"Change type");
    AppendMenuW(m, MF_STRING, 3, L"Delete");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, g.hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) {
        int s = bm_resolve(b, &g.doc, &g.chapters);
        if (s >= 0) { int h = here(); if (h >= 0 && h != s) poshist_jump(&X.hist, h); goto_sentence(s); }
    } else if (cmd == 2) edit_mark_note_idx(tag);
    else if (cmd == 3) { bm_remove(&g.bm, tag); save_settings(); refresh_gutter(); set_status(L"Mark deleted."); }
    else if (cmd >= 10 && cmd < 10 + MK_COUNT) {
        int s = bm_resolve(b, &g.doc, &g.chapters);
        if (bm_find_kind_at(&g.bm, &g.doc, &g.chapters, s, cmd - 10) >= 0) set_status(L"There is already a mark of that type on this passage.");
        else { b->kind = cmd - 10; save_settings(); refresh_gutter(); }
    }
}

/* ---- jumping around ---- */
static void jump_to(int target)
{
    ensure_doc();
    if (target < 0 || g.doc.nsent == 0) return;
    int h = here();
    if (h >= 0 && h != target) poshist_jump(&X.hist, h);
    goto_sentence(target);
}

static void history_move(int dir)
{
    ensure_doc();
    int t = dir < 0 ? poshist_back(&X.hist, here()) : poshist_forward(&X.hist, here());
    if (t < 0) { set_status(dir < 0 ? L"No earlier place to go back to." : L"No later place to go forward to."); return; }
    if (t >= g.doc.nsent) t = g.doc.nsent - 1;
    goto_sentence(t);
    wchar_t m[100];
    _snwprintf(m, 100, dir < 0 ? L"Jumped back (sentence %d)." : L"Jumped forward (sentence %d).", t + 1);
    m[99] = 0;
    say(m, 1200);
}

static void skip_time(double seconds)
{
    ensure_doc();
    int h = here();
    if (h < 0) return;
    int t = nav_skip_seconds(&g.doc, h, seconds, nav_chars_per_sec(play_rate()));
    if (t < 0) return;
    goto_sentence(t);
    wchar_t m[80];
    _snwprintf(m, 80, L"%ls %d seconds (estimated)", seconds < 0 ? L"Back" : L"Forward", (int)(seconds < 0 ? -seconds : seconds));
    m[79] = 0;
    say(m, 1000);
}

static void fmt_hms(double s, wchar_t *o, size_t cap)
{
    int t = (int)s;
    if (t >= 3600) _snwprintf(o, cap, L"%dh %02dm", t / 3600, (t / 60) % 60);
    else _snwprintf(o, cap, L"%dm %02ds", t / 60, t % 60);
    o[cap - 1] = 0;
}

static double gz_fraction_of(void *ctx, int sent)
{
    (void)ctx;
    double cps = nav_chars_per_sec(play_rate()), tot = nav_total_seconds(&g.doc, cps);
    return tot > 0 ? nav_seconds_before(&g.doc, sent, cps) / tot : 0;
}

static int gz_at_fraction(void *ctx, double f) { (void)ctx; return nav_at_fraction(&g.doc, f, nav_chars_per_sec(play_rate())); }

static void gz_describe(void *ctx, int sent, wchar_t *out, int cap)
{
    (void)ctx;
    if (sent < 0 || sent >= g.doc.nsent) { out[0] = 0; return; }
    double cps = nav_chars_per_sec(play_rate()), tot = nav_total_seconds(&g.doc, cps), at = nav_seconds_before(&g.doc, sent, cps);
    wchar_t a[40], b[40], ch[120], snip[70];
    fmt_hms(at, a, 40); fmt_hms(tot, b, 40);
    ch[0] = 0;
    int c = g.chapters.n ? chapter_of_sentence(&g.chapters, sent) : -1;
    if (c >= 0) { _snwprintf(ch, 120, L"Chapter %d: %.60ls", c + 1, g.chapters.c[c].title ? g.chapters.c[c].title : L""); ch[119] = 0; }
    wchar_t *t = doc_speech_text(&g.doc, sent);
    snip[0] = 0;
    if (t) { wcsncpy(snip, t, 60); snip[60] = 0; for (wchar_t *q = snip; *q; q++) if (*q < L' ') *q = L' '; free(t); }
    _snwprintf(out, (size_t)cap, L"%d%%  -  about %ls of %ls  -  sentence %d of %d%ls%ls\n\"%ls\"", tot > 0 ? (int)(at * 100 / tot) : 0, a, b,
               sent + 1, g.doc.nsent, ch[0] ? L"  -  " : L"", ch, snip);
    out[cap - 1] = 0;
}

static int gz_parse(void *ctx, const wchar_t *spec, wchar_t *err, int errcap)
{
    (void)ctx;
    GotoSpec sp;
    if (!goto_parse(spec, &sp)) { _snwprintf(err, (size_t)errcap, L"Not understood. Examples: 40%%  c12  p300  s1200  m3  +30s  -2m"); return -1; }
    double cps = nav_chars_per_sec(play_rate());
    switch (sp.kind) {
    case GT_PERCENT: return nav_at_fraction(&g.doc, sp.value / 100.0, cps);
    case GT_CHAPTER:
        if (sp.value > g.chapters.n) { _snwprintf(err, (size_t)errcap, L"There are only %d chapters.", g.chapters.n); return -1; }
        return g.chapters.c[(int)sp.value - 1].first_sent;
    case GT_PARAGRAPH:
        if (sp.value > g.doc.npara) { _snwprintf(err, (size_t)errcap, L"There are only %d paragraphs.", g.doc.npara); return -1; }
        return g.doc.paras[(int)sp.value - 1].first_sentence;
    case GT_SENTENCE:
        if (sp.value > g.doc.nsent) { _snwprintf(err, (size_t)errcap, L"There are only %d sentences.", g.doc.nsent); return -1; }
        return (int)sp.value - 1;
    case GT_MARK: {
        if (sp.value > g.bm.n) { _snwprintf(err, (size_t)errcap, L"There are only %d marks.", g.bm.n); return -1; }
        int t = bm_resolve(&g.bm.b[(int)sp.value - 1], &g.doc, &g.chapters);
        if (t < 0) _snwprintf(err, (size_t)errcap, L"The text of that mark can no longer be found.");
        return t;
    }
    case GT_SKIP: { int h = here(); return nav_skip_seconds(&g.doc, h < 0 ? 0 : h, sp.value, cps); }
    }
    return -1;
}

static void open_goto(void)
{
    ensure_doc();
    if (g.doc.nsent == 0) { set_status(L"There is no text."); return; }
    GotoUi u;
    memset(&u, 0, sizeof u);
    u.font = g.font; u.dpi = g.dpi;
    u.cur = here() < 0 ? 0 : here();
    u.fraction_of = gz_fraction_of; u.at_fraction = gz_at_fraction; u.describe = gz_describe; u.parse = gz_parse;
    u.chapters = (GotoItem *)calloc((size_t)g.chapters.n + 1, sizeof(GotoItem));
    for (int i = 0; u.chapters && i < g.chapters.n; i++) {
        _snwprintf(u.chapters[i].label, 220, L"%d.  %ls", i + 1, g.chapters.c[i].title ? g.chapters.c[i].title : L"");
        u.chapters[i].sent = g.chapters.c[i].first_sent;
        u.nch++;
    }
    u.marks = (GotoItem *)calloc((size_t)g.bm.n + 1, sizeof(GotoItem));
    for (int i = 0; u.marks && i < g.bm.n; i++) {
        int t = bm_resolve(&g.bm.b[i], &g.doc, &g.chapters);
        if (t < 0) continue;
        int c = g.chapters.n ? chapter_of_sentence(&g.chapters, t) : -1;
        _snwprintf(u.marks[u.nmk].label, 220, L"%d.  [%ls]  %.100ls%ls%.60ls", i + 1, mark_kind_name(g.bm.b[i].kind), g.bm.b[i].snippet,
                   g.bm.b[i].note ? L"   -   " : L"", g.bm.b[i].note ? g.bm.b[i].note : L"");
        (void)c;
        u.marks[u.nmk].sent = t;
        u.nmk++;
    }
    int nb = X.hist.nb;
    u.recent = (GotoItem *)calloc((size_t)nb + 1, sizeof(GotoItem));
    for (int i = nb - 1; u.recent && i >= 0; i--) {
        int t = X.hist.back[i];
        if (t < 0 || t >= g.doc.nsent) continue;
        wchar_t *tx = doc_speech_text(&g.doc, t);
        wchar_t sn[80];
        sn[0] = 0;
        if (tx) { wcsncpy(sn, tx, 70); sn[70] = 0; free(tx); }
        _snwprintf(u.recent[u.nrec].label, 220, L"sentence %d:  %ls", t + 1, sn);
        u.recent[u.nrec].sent = t;
        u.nrec++;
    }
    if (goto_dialog(dlg_parent(), &u) && u.out_sent >= 0) {
        jump_to(u.out_sent);
        if (u.out_play) begin_playback(u.out_sent, g.doc.nsent - 1);
    }
    free(u.chapters); free(u.marks); free(u.recent);
}

static void goto_any_mark(int dir)
{
    ensure_doc();
    int s = here();
    if (s < 0) return;
    int i = bm_next_mask(&g.bm, &g.doc, &g.chapters, s, dir, ~0u);
    if (i < 0) { set_status(L"There are no marks yet (Ctrl+B bookmark, Alt+1..5 flags)."); return; }
    int t = bm_resolve(&g.bm.b[i], &g.doc, &g.chapters);
    if (t < 0) return;
    jump_to(t);
    wchar_t m[260];
    _snwprintf(m, 260, L"%ls%ls%ls", mark_kind_name(g.bm.b[i].kind), g.bm.b[i].note ? L": " : L"", g.bm.b[i].note ? g.bm.b[i].note : g.bm.b[i].snippet);
    m[259] = 0;
    say(m, 2500);
}

/* ---- sleep timer ---- */
static void sleep_check(void)
{
    if (X.sleep_deadline && GetTickCount64() >= X.sleep_deadline) {
        X.sleep_deadline = 0;
        stop_playback(L"Sleep timer: stopped.");
        caption_notice(L"Sleep timer: stopped.", 3000);
    }
}

static void set_sleep(int minutes)
{
    X.sleep_deadline = minutes > 0 ? GetTickCount64() + (ULONGLONG)minutes * 60000ull : 0;
    say(minutes > 0 ? L"Sleep timer set." : L"Sleep timer off.", 1200);
}

static void stop_at_chapter_end(void)
{
    ensure_doc();
    if ((g.state != ST_PLAYING && g.state != ST_PAUSED) || g.cur_sent < 0 || g.chapters.n == 0) {
        set_status(L"Start playing first (and the document needs chapters) to stop at the end of the chapter."); return;
    }
    int ch = chapter_of_sentence(&g.chapters, g.cur_sent);
    int last = ch < 0 ? g.chapters.c[0].first_sent - 1 : chapter_last_sentence(&g.chapters, &g.doc, ch);
    if (last < g.cur_sent) return;
    g.player.last = last;
    g.play_last = last;
    say(L"Will stop at the end of this chapter.", 1500);
}

/* ---- tray icon ---- */
static HICON make_icon(int style)      /* 0 idle, 1 playing, 2 paused */
{
    const int N = 32;
    HDC sdc = GetDC(NULL);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP color = CreateCompatibleBitmap(sdc, N, N), mask = CreateBitmap(N, N, 1, 1, NULL);
    ReleaseDC(NULL, sdc);
    HGDIOBJ old = SelectObject(dc, color);
    COLORREF bgc = style == 1 ? RGB(30, 130, 70) : style == 2 ? RGB(200, 140, 20) : RGB(40, 90, 170);
    HBRUSH bb = CreateSolidBrush(bgc), wb = CreateSolidBrush(RGB(255, 255, 255));
    RECT all = { 0, 0, N, N };
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(dc, bb);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 1, 1, N - 1, N - 1);
    SelectObject(dc, wb);
    if (style == 2) { RECT a = { 10, 9, 14, 23 }, b = { 18, 9, 22, 23 }; FillRect(dc, &a, wb); FillRect(dc, &b, wb); }
    else if (style == 1) { POINT p[3] = { { 11, 8 }, { 11, 24 }, { 25, 16 } }; Polygon(dc, p, 3); }
    else { RECT a = { 8, 12, 11, 20 }, b = { 13, 8, 16, 24 }, c = { 18, 11, 21, 21 }, d = { 23, 14, 26, 18 }; FillRect(dc, &a, wb); FillRect(dc, &b, wb); FillRect(dc, &c, wb); FillRect(dc, &d, wb); }
    SelectObject(dc, mask);
    PatBlt(dc, 0, 0, N, N, WHITENESS);
    HBRUSH blk = (HBRUSH)GetStockObject(BLACK_BRUSH);
    SelectObject(dc, blk);
    SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, 0, 0, N, N);
    SelectObject(dc, old);
    DeleteObject(bb); DeleteObject(wb);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    DeleteDC(dc);
    return ic;
}

static void tray_update(void)
{
    if (!X.ready || !g.hwnd) return;
    int style = g.state == ST_PLAYING ? 1 : g.state == ST_PAUSED ? 2 : 0;
    if (!X.icons[style]) X.icons[style] = make_icon(style);
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof n);
    n.cbSize = sizeof n;
    n.hWnd = g.hwnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = TRAY_MSG;
    n.hIcon = X.icons[style] ? X.icons[style] : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wchar_t chp[60] = L"", name[60] = L"";
    int s = (g.state != ST_IDLE) ? g.cur_sent : -1;
    if (s >= 0 && s < g.doc.nsent) {
        int pct = g.doc.len ? (int)((g.doc.sents[s].start * 100) / g.doc.len) : 0;
        int ch = g.chapters.n ? chapter_of_sentence(&g.chapters, s) : -1;
        if (ch >= 0) _snwprintf(chp, 60, L"Chapter %d/%d, %d%%", ch + 1, g.chapters.n, pct); else _snwprintf(chp, 60, L"%d%%", pct);
        chp[59] = 0;
    }
    if (g.project_path[0]) { _snwprintf(name, 60, L"%ls\n", g.project_name); name[59] = 0; }
    _snwprintf(n.szTip, 128, L"Mouthpiece - %ls\n%ls%ls", style == 1 ? L"playing" : style == 2 ? L"paused" : L"stopped", name, chp);
    n.szTip[127] = 0;
    if (!X.tray_added) { Shell_NotifyIconW(NIM_ADD, &n); X.tray_added = 1; }
    else Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void tray_remove(void)
{
    if (!X.tray_added) return;
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof n);
    n.cbSize = sizeof n; n.hWnd = g.hwnd; n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
    X.tray_added = 0;
}

static void show_main(void)
{
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

/* ---- captions ---- */
static void open_caption_dialog(void);
static void caption_apply(void)
{
    caption_configure(&X.cap);
    if (X.cap.enabled && g.state == ST_PLAYING && g.cur_sent >= 0) {
        wchar_t *t = doc_speech_text(&g.doc, g.cur_sent);
        if (t) { caption_set(t, wcslen(t), nav_sentence_seconds(&g.doc, g.cur_sent, nav_chars_per_sec(play_rate()))); free(t); }
    }
    menu_check(CMD_CAP_TOGGLE, X.cap.enabled);
    save_settings();
}

/* Captions follow the words the voice really reaches (its own word events), not a timer. */
static void caption_follow(void)
{
    int ps;
    double fr;
    if (X.ready && g.state == ST_PLAYING && X.cap.enabled && player_progress(&g.player, &ps, &fr) && ps == g.cur_sent) caption_progress(fr);
}

static void on_sentence_shown(int sent)
{
    if (!X.ready) return;
    ULONGLONG now = GetTickCount64();
    /* learn how fast this voice really speaks, from sentences that played through without a pause or jump */
    if (g.state == ST_PLAYING && X.meas_natural && !X.pause_used && sent == X.meas_sent + 1 && X.meas_sent >= 0 && X.meas_sent < g.doc.nsent) {
        double dt = (double)(now - X.sent_tick) / 1000.0;
        if (dt > 0.4 && dt < 120.0) {
            double c = (double)g.doc.sents[X.meas_sent].len / dt;
            X.cps_meas = X.cps_meas > 0 ? 0.7 * X.cps_meas + 0.3 * c : c;
        }
    }
    X.meas_natural = (g.state == ST_PLAYING && sent == X.meas_sent + 1);
    X.meas_sent = sent;
    X.pause_used = 0;
    X.sent_tick = now;
    if (g.state == ST_PLAYING && X.cap.enabled && sent >= 0 && sent < g.doc.nsent) {
        wchar_t *t = doc_speech_text(&g.doc, sent);
        if (t) { caption_set(t, wcslen(t), nav_sentence_seconds(&g.doc, sent, nav_chars_per_sec(play_rate()))); free(t); }
    }
    tray_update();
}

static void on_state_changed(void)
{
    if (!X.ready) return;
    if (g.state == ST_PLAYING && g.cur_sent >= 0) on_sentence_shown(g.cur_sent);
    else { caption_idle(); tray_update(); }
    if (g.state == ST_IDLE) X.sleep_deadline = 0;
}

/* ---- global shortcuts ---- */
static void unregister_armed(void)
{
    UnregisterHotKey(g.hwnd, HK_ID_ESC);
    int n;
    leader_keys(&n);
    for (int i = 0; i < n; i++) UnregisterHotKey(g.hwnd, HK_ID_ARMED + i);
}

static void disarm(void)
{
    if (!X.armed) return;
    X.armed = 0;
    KillTimer(g.hwnd, TIMER_ARM);
    unregister_armed();
    caption_hide();
    if (X.cap.enabled && g.state == ST_PLAYING) caption_apply();
}

static void arm(void)
{
    int n;
    const LeaderKey *k = leader_keys(&n);
    if (!X.armed) {
        RegisterHotKey(g.hwnd, HK_ID_ESC, MOD_NOREPEAT, VK_ESCAPE);
        for (int i = 0; i < n; i++) RegisterHotKey(g.hwnd, HK_ID_ARMED + i, MOD_NOREPEAT, (UINT)k[i].vk);
        X.armed = 1;
    }
    SetTimer(g.hwnd, TIMER_ARM, (UINT)X.hk.arm_seconds * 1000u, NULL);
    wchar_t help[400];
    leader_help(help, 400);
    caption_notice(help, X.hk.arm_seconds * 1000);
}

static void do_action(int a);

static void register_hotkeys(void)
{
    UnregisterHotKey(g.hwnd, HK_ID_LEADER);
    for (int a = 1; a < ACT_COUNT; a++) UnregisterHotKey(g.hwnd, HK_ID_DIRECT + a);
    for (int i = 0; i < 4; i++) UnregisterHotKey(g.hwnd, HK_ID_MEDIA + i);
    disarm();
    X.hk_report[0] = 0;
    memset(X.hk.failed, 0, sizeof X.hk.failed);
    if (!X.hk.enabled) return;
    wchar_t t[64];
    if (X.hk.leader_vk) {
        UINT mods = (UINT)(((X.hk.leader_mod & HK_MOD_ALT) ? MOD_ALT : 0) | ((X.hk.leader_mod & HK_MOD_CONTROL) ? MOD_CONTROL : 0) |
                           ((X.hk.leader_mod & HK_MOD_SHIFT) ? MOD_SHIFT : 0) | ((X.hk.leader_mod & HK_MOD_WIN) ? MOD_WIN : 0)) | MOD_NOREPEAT;
        if (!RegisterHotKey(g.hwnd, HK_ID_LEADER, mods, (UINT)X.hk.leader_vk)) {
            /* taken by another program: try a few alternatives before giving up */
            static const struct { int mod, vk; } alt[] = { { 3, 'M' }, { 7, 0x20 }, { 3, 'K' }, { 7, 'M' } };
            int ok = 0;
            hotkey_text(X.hk.leader_mod, X.hk.leader_vk, t, 64);
            for (size_t i = 0; i < sizeof alt / sizeof alt[0] && !ok; i++) {
                UINT m2 = (UINT)(((alt[i].mod & 1) ? MOD_ALT : 0) | ((alt[i].mod & 2) ? MOD_CONTROL : 0) | ((alt[i].mod & 4) ? MOD_SHIFT : 0)) | MOD_NOREPEAT;
                if (RegisterHotKey(g.hwnd, HK_ID_LEADER, m2, (UINT)alt[i].vk)) {
                    X.hk.leader_mod = alt[i].mod; X.hk.leader_vk = alt[i].vk; ok = 1;
                    wchar_t t2[64];
                    hotkey_text(alt[i].mod, alt[i].vk, t2, 64);
                    _snwprintf(X.hk_report, 600, L"%ls is already used by another program, so the leader key is now %ls. "
                                                  L"You can choose another one here.", t, t2);
                }
            }
            if (!ok) {
                X.hk.failed[HK_FAIL_LEADER] = 1;
                _snwprintf(X.hk_report, 600, L"%ls is already used by another program and no alternative was free. "
                                             L"Choose a different leader key here; the tray icon still works.", t);
            }
        }
    }
    for (int a = 1; a < ACT_COUNT; a++) {
        if (!X.hk.direct[a].vk) continue;
        int m = X.hk.direct[a].mod;
        UINT mods = (UINT)(((m & HK_MOD_ALT) ? MOD_ALT : 0) | ((m & HK_MOD_CONTROL) ? MOD_CONTROL : 0) | ((m & HK_MOD_SHIFT) ? MOD_SHIFT : 0)) | MOD_NOREPEAT;
        if (!RegisterHotKey(g.hwnd, HK_ID_DIRECT + a, mods, (UINT)X.hk.direct[a].vk)) {
            X.hk.failed[a] = 1;
            hotkey_text(m, X.hk.direct[a].vk, t, 64);
            size_t l = wcslen(X.hk_report);
            _snwprintf(X.hk_report + l, 600 - l, L"%ls%ls (%ls) is used by another program.", l ? L"\n" : L"", t, action_name(a));
        }
    }
    if (X.hk.media_keys) {
        static const UINT mk[4] = { VK_MEDIA_PLAY_PAUSE, VK_MEDIA_NEXT_TRACK, VK_MEDIA_PREV_TRACK, VK_MEDIA_STOP };
        for (int i = 0; i < 4; i++) if (!RegisterHotKey(g.hwnd, HK_ID_MEDIA + i, MOD_NOREPEAT, mk[i])) X.hk.failed[HK_FAIL_MEDIA(i)] = 1;
    }
    X.hk_report[599] = 0;
}

static void open_hotkeys(void)
{
    int changed = hotkeys_dialog(dlg_parent(), g.font, g.dpi, &X.hk, X.hk_report);
    if (changed) {
        register_hotkeys();
        save_settings();
        if (X.hk_report[0]) MessageBoxW(dlg_parent(), X.hk_report, L"Global shortcuts", MB_OK | MB_ICONWARNING);
    }
    menu_check(CMD_HK_ENABLE, X.hk.enabled);
}

static void change_rate(int d)
{
    int r = play_rate() + d;
    if (r < -10) r = -10;
    if (r > 10) r = 10;
    int old = play_rate();
    SendMessageW(g.rate, TBM_SETPOS, TRUE, r);
    update_value_labels();
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) rate_changed_live(old);
    wchar_t m[40];
    _snwprintf(m, 40, L"Speed %+d", r);
    m[39] = 0;
    say(m, 900);
}

static void do_action(int a)
{
    switch (a) {
    case ACT_PLAYPAUSE: toggle_play(); break;
    case ACT_STOP: if (g.state != ST_IDLE) stop_playback(NULL); break;
    case ACT_PREV_SENT: nav_sentence(-1); break;
    case ACT_NEXT_SENT: nav_sentence(+1); break;
    case ACT_PREV_PARA: nav_paragraph(-1); break;
    case ACT_NEXT_PARA: nav_paragraph(+1); break;
    case ACT_PREV_CH: nav_chapter(-1); break;
    case ACT_NEXT_CH: nav_chapter(+1); break;
    case ACT_REPLAY: replay_sentence(); break;
    case ACT_JUMP_BACK: history_move(-1); break;
    case ACT_JUMP_FWD: history_move(+1); break;
    case ACT_SKIP_BACK: skip_time(-30); break;
    case ACT_SKIP_FWD: skip_time(30); break;
    case ACT_MARK_BOOKMARK: flag_mark(MK_BOOKMARK); break;
    case ACT_MARK_REVISIT: flag_mark(MK_REVISIT); break;
    case ACT_MARK_IMPORTANT: flag_mark(MK_IMPORTANT); break;
    case ACT_MARK_UNCLEAR: flag_mark(MK_UNCLEAR); break;
    case ACT_MARK_QUESTION: flag_mark(MK_QUESTION); break;
    case ACT_MARK_DOUBT: flag_mark(MK_DOUBT); break;
    case ACT_NEXT_MARK: goto_any_mark(+1); break;
    case ACT_PREV_MARK: goto_any_mark(-1); break;
    case ACT_FASTER: change_rate(+1); break;
    case ACT_SLOWER: change_rate(-1); break;
    case ACT_CAPTIONS: X.cap.enabled = !X.cap.enabled; caption_apply(); say(X.cap.enabled ? L"Captions on" : L"Captions off", 800); break;
    case ACT_SHOW_WINDOW: show_main(); break;
    }
}

static void on_hotkey(int id)
{
    if (id == HK_ID_LEADER) { if (X.armed) disarm(); else arm(); return; }
    if (id == HK_ID_ESC) { disarm(); return; }
    if (id >= HK_ID_ARMED && id < HK_ID_ARMED + 64) {
        int n;
        const LeaderKey *k = leader_keys(&n);
        int i = id - HK_ID_ARMED;
        if (i < n) {
            int stay = k[i].stays_armed;
            if (!stay) disarm();
            do_action(k[i].action);
            if (stay && X.armed) arm();
        }
        return;
    }
    if (id >= HK_ID_DIRECT && id < HK_ID_DIRECT + ACT_COUNT) { do_action(id - HK_ID_DIRECT); return; }
    if (id >= HK_ID_MEDIA && id < HK_ID_MEDIA + 4) {
        static const int acts[4] = { ACT_PLAYPAUSE, ACT_NEXT_PARA, ACT_PREV_PARA, ACT_STOP };
        do_action(acts[id - HK_ID_MEDIA]);
    }
}

/* ---- tray menu ---- */
/* ---- tray menu items with their shortcut in grey on the right (owner-drawn: a normal menu cannot colour part of an item) ---- */
typedef struct TrayItem { wchar_t label[100], key[80]; int bold; } TrayItem;
#define TRAY_ITEMS 100
static TrayItem tray_items[TRAY_ITEMS];
static int tray_nitems;
static HFONT tray_fonts[2];

static HFONT tray_font(int bold)
{
    if (!tray_fonts[bold]) {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) return g.font;
        if (bold) ncm.lfMenuFont.lfWeight = FW_BOLD;
        tray_fonts[bold] = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    return tray_fonts[bold] ? tray_fonts[bold] : g.font;
}

static TrayItem *tray_item_of(ULONG_PTR data)
{
    for (int i = 0; i < tray_nitems; i++) if (data == (ULONG_PTR)&tray_items[i]) return &tray_items[i];
    return NULL;
}

/* The shortcut text for an action: its direct shortcut, else leader key + key. Says so when it is not active. */
static void act_key_text(wchar_t *k, size_t cap, int act)
{
    k[0] = 0;
    if (act <= 0 || act >= ACT_COUNT) return;
    int via_leader = 0;
    if (X.hk.direct[act].vk) hotkey_text(X.hk.direct[act].mod, X.hk.direct[act].vk, k, cap);
    else if (X.hk.leader_vk && leader_vk_of(act)) {
        wchar_t lt[40], kn[24];
        hotkey_text(X.hk.leader_mod, X.hk.leader_vk, lt, 40);
        vk_name(leader_vk_of(act), kn, 24);
        _snwprintf(k, cap, L"%ls, then %ls", lt, kn);
        via_leader = 1;
    }
    k[cap - 1] = 0;
    if (!k[0]) return;
    size_t l = wcslen(k);
    if (!X.hk.enabled) _snwprintf(k + l, cap - l, L"  (shortcuts off)");
    else if (via_leader ? X.hk.failed[HK_FAIL_LEADER] : X.hk.failed[act]) _snwprintf(k + l, cap - l, L"  (in use)");
    k[cap - 1] = 0;
}

static void add_act(HMENU m, UINT flags, UINT_PTR id, const wchar_t *label, int act)
{
    if (tray_nitems >= TRAY_ITEMS) { AppendMenuW(m, MF_STRING | flags, id, label); return; }
    TrayItem *t = &tray_items[tray_nitems++];
    int j = 0;
    for (const wchar_t *p = label; *p && j < 99; p++) if (*p != L'&') t->label[j++] = *p;       /* no mnemonics in owner-drawn items */
    t->label[j] = 0;
    t->bold = (id == CMD_SHOW);
    act_key_text(t->key, 80, act);
    AppendMenuW(m, MF_OWNERDRAW | flags, id, (LPCWSTR)t);
}

static int tray_measure(MEASUREITEMSTRUCT *mi)
{
    TrayItem *t = mi->CtlType == ODT_MENU ? tray_item_of(mi->itemData) : NULL;
    if (!t) return 0;
    HDC dc = GetDC(g.hwnd);
    HFONT old = (HFONT)SelectObject(dc, tray_font(t->bold));
    SIZE lw = { 0, 0 }, kw = { 0, 0 };
    GetTextExtentPoint32W(dc, t->label, (int)wcslen(t->label), &lw);
    if (t->key[0]) GetTextExtentPoint32W(dc, t->key, (int)wcslen(t->key), &kw);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    mi->itemWidth = (UINT)(S(26) + lw.cx + (t->key[0] ? S(32) + kw.cx : 0) + S(12));
    int h = tm.tmHeight + S(8), sys = GetSystemMetrics(SM_CYMENU);
    mi->itemHeight = (UINT)(h > sys ? h : sys);
    return 1;
}

static int tray_draw(const DRAWITEMSTRUCT *di)
{
    TrayItem *t = di->CtlType == ODT_MENU ? tray_item_of(di->itemData) : NULL;
    if (!t) return 0;
    int sel = (di->itemState & ODS_SELECTED) != 0, gray = (di->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    FillRect(dc, &r, GetSysColorBrush(sel && !gray ? COLOR_HIGHLIGHT : COLOR_MENU));
    COLORREF main_c = GetSysColor(gray ? COLOR_GRAYTEXT : sel ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    COLORREF key_c = GetSysColor(sel && !gray ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT);
    HFONT old = (HFONT)SelectObject(dc, tray_font(t->bold));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, main_c);
    if (di->itemState & ODS_CHECKED) {
        RECT cr = { r.left + S(4), r.top, r.left + S(24), r.bottom };
        DrawTextW(dc, L"\x2713", 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    RECT lr = { r.left + S(26), r.top, r.right - S(8), r.bottom };
    DrawTextW(dc, t->label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (t->key[0]) {
        SetTextColor(dc, key_c);
        RECT kr = { r.left + S(26), r.top, r.right - S(12), r.bottom };
        DrawTextW(dc, t->key, -1, &kr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    return 1;
}

static void tray_menu(void)
{
    tray_nitems = 0;
    HMENU m = CreatePopupMenu();                 /* rebuilt on every click, so it is not tracked in g_popups */
    int vis = IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd);
    add_act(m, 0, CMD_SHOW, vis ? L"&Hide the Mouthpiece window" : L"&Show the Mouthpiece window", ACT_SHOW_WINDOW);
    SetMenuDefaultItem(m, CMD_SHOW, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add_act(m, 0, CMD_PLAY_TRAY, g.state == ST_PLAYING ? L"&Pause" : g.state == ST_PAUSED ? L"&Resume" : L"&Play from the cursor", ACT_PLAYPAUSE);
    add_act(m, g.state == ST_IDLE ? MF_GRAYED : 0, CMD_STOP, L"S&top", ACT_STOP);
    HMENU mv = CreatePopupMenu();
    add_act(mv, 0, CMD_PREV, L"Previous sentence", ACT_PREV_SENT);
    add_act(mv, 0, CMD_NEXT, L"Next sentence", ACT_NEXT_SENT);
    add_act(mv, 0, CMD_PREV_PARA, L"Previous paragraph", ACT_PREV_PARA);
    add_act(mv, 0, CMD_NEXT_PARA, L"Next paragraph", ACT_NEXT_PARA);
    add_act(mv, 0, CMD_PREV_CH, L"Previous chapter", ACT_PREV_CH);
    add_act(mv, 0, CMD_NEXT_CH, L"Next chapter", ACT_NEXT_CH);
    AppendMenuW(mv, MF_SEPARATOR, 0, NULL);
    add_act(mv, 0, CMD_SKIP_BACK, L"Back 30 seconds", ACT_SKIP_BACK);
    add_act(mv, 0, CMD_SKIP_FWD, L"Forward 30 seconds", ACT_SKIP_FWD);
    add_act(mv, 0, CMD_JUMP_BACK, L"Back to where I was", ACT_JUMP_BACK);
    add_act(mv, 0, CMD_JUMP_FWD, L"Forward again", ACT_JUMP_FWD);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mv, L"&Move");
    AppendMenuW(m, MF_STRING, CMD_GOTO, L"&Go to...");
    if (g.chapters.n) {
        HMENU cm = CreatePopupMenu();
        int cur = g.cur_sent >= 0 ? chapter_of_sentence(&g.chapters, g.cur_sent) : chapter_of_sentence(&g.chapters, caret_sentence());
        for (int i = 0; i < g.chapters.n && i < 40; i++) {
            wchar_t it[120];
            _snwprintf(it, 120, L"%d.  %.90ls", i + 1, g.chapters.c[i].title ? g.chapters.c[i].title : L"");
            it[119] = 0;
            for (wchar_t *q = it; *q; q++) if (*q == L'&') *q = L'+';
            AppendMenuW(cm, MF_STRING | (i == cur ? MF_CHECKED : 0), (UINT_PTR)(CMD_CHJUMP_BASE + i), it);
        }
        if (g.chapters.n > 40) { AppendMenuW(cm, MF_SEPARATOR, 0, NULL); AppendMenuW(cm, MF_STRING, CMD_GOTO, L"More chapters (Go to)..."); }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)cm, L"&Chapters");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU fl = CreatePopupMenu();
    add_act(fl, 0, CMD_FLAG_BASE, L"Bookmark", ACT_MARK_BOOKMARK);
    for (int k = 1; k < MK_COUNT; k++) add_act(fl, 0, (UINT_PTR)(CMD_FLAG_BASE + k), mark_kind_name(k), ACT_MARK_BOOKMARK + k);
    AppendMenuW(fl, MF_SEPARATOR, 0, NULL);
    AppendMenuW(fl, MF_STRING, CMD_MARK_NOTE, L"Add or edit a note on the mark here...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fl, L"&Flag this passage");
    HMENU mk = CreatePopupMenu();
    add_act(mk, 0, CMD_BM_NEXT, L"Next mark", ACT_NEXT_MARK);
    add_act(mk, 0, CMD_BM_PREV, L"Previous mark", ACT_PREV_MARK);
    AppendMenuW(mk, MF_STRING, CMD_NAV, L"Manage marks and chapters...");
    if (g.bm.n) AppendMenuW(mk, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g.bm.n && i < 25; i++) {
        wchar_t it[120];
        _snwprintf(it, 120, L"%ls: %.70ls", mark_kind_name(g.bm.b[i].kind), g.bm.b[i].note ? g.bm.b[i].note : g.bm.b[i].snippet);
        it[119] = 0;
        for (wchar_t *q = it; *q; q++) if (*q == L'&') *q = L'+';
        AppendMenuW(mk, MF_STRING, (UINT_PTR)(CMD_MARKJUMP_BASE + i), it);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mk, L"&Marks");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU sp = CreatePopupMenu();
    wchar_t cur[40];
    _snwprintf(cur, 40, L"Current speed: %+d", play_rate());
    cur[39] = 0;
    AppendMenuW(sp, MF_STRING | MF_DISABLED, 0, cur);
    add_act(sp, 0, CMD_RATE_UP, L"Faster", ACT_FASTER);
    add_act(sp, 0, CMD_RATE_DOWN, L"Slower", ACT_SLOWER);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"S&peed");
    if (g.profiles.n) {
        HMENU pr = CreatePopupMenu();
        for (int i = 0; i < g.profiles.n && i < 40; i++) AppendMenuW(pr, MF_STRING, (UINT_PTR)(CMD_PROFILE_BASE + i), g.profiles.p[i].name);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)pr, L"&Voice profile");
    }
    HMENU sl = CreatePopupMenu();
    AppendMenuW(sl, MF_STRING | (!X.sleep_deadline ? MF_CHECKED : 0), CMD_SLEEP_BASE, L"Off");
    for (size_t i = 1; i < sizeof sleep_minutes / sizeof sleep_minutes[0]; i++) {
        wchar_t it[40];
        _snwprintf(it, 40, L"Stop in %d minutes", sleep_minutes[i]);
        AppendMenuW(sl, MF_STRING, (UINT_PTR)(CMD_SLEEP_BASE + i), it);
    }
    AppendMenuW(sl, MF_STRING, CMD_SLEEP_STOP_CH, L"Stop at the end of this chapter");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sl, L"S&leep timer");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU cm = CreatePopupMenu();
    add_act(cm, X.cap.enabled ? MF_CHECKED : 0, CMD_CAP_TOGGLE, L"Show captions", ACT_CAPTIONS);
    AppendMenuW(cm, MF_STRING, CMD_CAP_DIALOG, L"Caption settings...");
    AppendMenuW(cm, MF_STRING | (caption_moving() ? MF_CHECKED : 0), CMD_CAP_MOVE, L"Move captions by dragging");
    AppendMenuW(cm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cm, MF_STRING, CMD_CAP_BIGGER, L"Larger text");
    AppendMenuW(cm, MF_STRING, CMD_CAP_SMALLER, L"Smaller text");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cm, L"&Captions");
    HMENU hk = CreatePopupMenu();
    if (X.hk.enabled && X.hk.leader_vk) {
        wchar_t lt[40], ln[100];
        hotkey_text(X.hk.leader_mod, X.hk.leader_vk, lt, 40);
        _snwprintf(ln, 100, L"Leader key: %ls (then a single key)", lt);
        ln[99] = 0;
        AppendMenuW(hk, MF_STRING | MF_DISABLED, 0, ln);
        AppendMenuW(hk, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(hk, MF_STRING | (X.hk.enabled ? MF_CHECKED : 0), CMD_HK_ENABLE, L"Global shortcuts on");
    AppendMenuW(hk, MF_STRING, CMD_HK_HELP, L"Show all the shortcut keys...");
    AppendMenuW(hk, MF_STRING, CMD_HK_DIALOG, L"Shortcut settings...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)hk, L"Shortcut &keys");
    HMENU wm = CreatePopupMenu();
    AppendMenuW(wm, MF_STRING | (X.min_to_tray ? MF_CHECKED : 0), CMD_TRAY_MIN, L"Minimizing hides the window in the tray");
    AppendMenuW(wm, MF_STRING | (X.close_to_tray ? MF_CHECKED : 0), CMD_TRAY_CLOSE, L"Closing the window keeps it in the tray");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)wm, L"&Window");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"E&xit Mouthpiece");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);                          /* required so the menu closes when clicking elsewhere */
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.hwnd, NULL);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) PostMessageW(g.hwnd, WM_COMMAND, (WPARAM)cmd, 0);
}

static void caption_live(const CaptionCfg *c, void *ctx) { (void)ctx; X.cap = *c; caption_configure(&X.cap); }

static void caption_moved_cb(const CaptionCfg *c, void *ctx)
{
    (void)ctx;
    X.cap.position = c->position; X.cap.cx = c->cx; X.cap.cy = c->cy;
    menu_check(CMD_CAP_MOVE, caption_moving());
    save_settings();
}

static void looks_saved_cb(void *ctx) { (void)ctx; save_settings(); }

static void open_caption_dialog(void)
{
    int changed = caption_dialog(dlg_parent(), g.font, g.dpi, &X.cap, caption_live, NULL, &X.looks, looks_saved_cb);
    if (changed) caption_apply();
    menu_check(CMD_CAP_MOVE, 0);
}

static void sync_menu_checks(void)
{
    menu_check(CMD_CAP_TOGGLE, X.cap.enabled);
    menu_check(CMD_HK_ENABLE, X.hk.enabled);
    menu_check(CMD_TRAY_MIN, X.min_to_tray);
    menu_check(CMD_TRAY_CLOSE, X.close_to_tray);
    menu_check(CMD_CAP_MOVE, caption_moving());
}

/* WM_COMMAND ids of this section. Returns 1 if handled. */
static int ext_command(int id)
{
    if (id >= CMD_FLAG_BASE && id < CMD_FLAG_BASE + MK_COUNT) { flag_mark(id - CMD_FLAG_BASE); return 1; }
    if (id >= CMD_SLEEP_BASE && id < CMD_SLEEP_BASE + 20) { set_sleep(sleep_minutes[id - CMD_SLEEP_BASE]); return 1; }
    if (id >= CMD_PROFILE_BASE && id < CMD_PROFILE_BASE + 40) {
        int i = id - CMD_PROFILE_BASE;
        if (i < g.profiles.n) {
            if (g.state == ST_IDLE) prof_apply(NULL, &g.profiles.p[i]);
            else { VoiceProfile *p = &g.profiles.p[i]; int old = play_rate(); SendMessageW(g.rate, TBM_SETPOS, TRUE, p->rate); SendMessageW(g.vol, TBM_SETPOS, TRUE, p->volume); update_value_labels(); rate_changed_live(old); }
            wchar_t m[120];
            _snwprintf(m, 120, L"Profile: %ls", g.profiles.p[i].name);
            m[119] = 0;
            say(m, 1200);
        }
        return 1;
    }
    if (id >= CMD_CHJUMP_BASE && id < CMD_CHJUMP_BASE + 100) {
        int i = id - CMD_CHJUMP_BASE;
        if (i < g.chapters.n) jump_to(g.chapters.c[i].first_sent);
        return 1;
    }
    if (id >= CMD_MARKJUMP_BASE && id < CMD_MARKJUMP_BASE + 100) {
        int i = id - CMD_MARKJUMP_BASE;
        if (i < g.bm.n) jump_to(bm_resolve(&g.bm.b[i], &g.doc, &g.chapters));
        return 1;
    }
    switch (id) {
    case CMD_JUMP_BACK: history_move(-1); return 1;
    case CMD_JUMP_FWD: history_move(+1); return 1;
    case CMD_SKIP_BACK: skip_time(-30); return 1;
    case CMD_SKIP_FWD: skip_time(30); return 1;
    case CMD_GOTO: open_goto(); return 1;
    case CMD_MARK_NOTE: cmd_mark_note(); return 1;
    case CMD_RATE_UP: change_rate(+1); return 1;
    case CMD_RATE_DOWN: change_rate(-1); return 1;
    case CMD_CAP_TOGGLE: do_action(ACT_CAPTIONS); return 1;
    case CMD_CAP_BIGGER: if (X.cap.font_pt < 72) X.cap.font_pt += 2; caption_apply(); return 1;
    case CMD_CAP_SMALLER: if (X.cap.font_pt > 12) X.cap.font_pt -= 2; caption_apply(); return 1;
    case CMD_CAP_MOREOP: if (X.cap.bg_opacity < 100) X.cap.bg_opacity += 10; caption_apply(); return 1;
    case CMD_CAP_LESSOP: if (X.cap.bg_opacity > 0) X.cap.bg_opacity -= 10; caption_apply(); return 1;
    case CMD_CAP_POS: X.cap.position = X.cap.position == 1 ? 0 : 1; caption_apply(); return 1;
    case CMD_CAP_DIALOG: open_caption_dialog(); return 1;
    case CMD_CAP_MOVE:
        if (!caption_moving()) {
            if (X.cap.position != 2 && !X.cap.cx && !X.cap.cy) { X.cap.cx = GetSystemMetrics(SM_CXSCREEN) / 2; X.cap.cy = GetSystemMetrics(SM_CYSCREEN) * 3 / 4; }
            X.cap.position = 2;
            caption_configure(&X.cap);
            caption_move_mode(1);
            say(L"Drag the caption bar where you want it, then double-click it.", 0);
        } else caption_move_mode(0);
        menu_check(CMD_CAP_MOVE, caption_moving());
        return 1;
    case CMD_TRAY_MIN: X.min_to_tray = !X.min_to_tray; sync_menu_checks(); save_settings(); return 1;
    case CMD_TRAY_CLOSE: X.close_to_tray = !X.close_to_tray; sync_menu_checks(); save_settings(); return 1;
    case CMD_HK_DIALOG: open_hotkeys(); return 1;
    case CMD_HK_ENABLE:
        X.hk.enabled = !X.hk.enabled;
        register_hotkeys();
        save_settings();
        if (X.hk_report[0]) say(X.hk_report, 0);
        menu_check(CMD_HK_ENABLE, X.hk.enabled);
        return 1;
    case CMD_HK_HELP: hotkeys_help_dialog(dlg_parent(), g.font, g.dpi, &X.hk, X.hk_report); return 1;
    case CMD_SHOW:
        if (IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_HIDE); else show_main();
        return 1;
    case CMD_SLEEP_STOP_CH: stop_at_chapter_end(); return 1;
    }
    return 0;
}

/* settings of this section live in settings.txt (they are about the program, not about one book) */
static void ext_to_ini(Ini *a)
{
    /* Before ext_init has loaded them, these settings are all zero. Saving then (a project opening at start-up saves) would
     * overwrite the real saved values with "off" and "none". */
    if (!X.ready) return;
    hotkeys_to_ini(&X.hk, a);
    caption_cfg_to_ini(&X.cap, a);
    looks_to_ini(&X.looks, a);
    ini_set_int(a, L"Tray", L"HideOnMinimize", X.min_to_tray);
    ini_set_int(a, L"Tray", L"CloseToTray", X.close_to_tray);
}

static void ext_from_ini(const Ini *a)
{
    hotkeys_from_ini(&X.hk, a);
    caption_cfg_from_ini(&X.cap, a);
    looks_from_ini(&X.looks, a);
    /* "HideOnMinimize" replaces older keys: earlier builds could store a 0 here before the user ever chose it */
    X.min_to_tray = ini_get_int(a, L"Tray", L"HideOnMinimize", 1) != 0;
    X.close_to_tray = ini_get_int(a, L"Tray", L"CloseToTray", 0) != 0;
}

static void ext_init(void)
{
    Ini a;
    read_ini_file(g.settings_path, &a);
    ext_from_ini(&a);
    ini_free(&a);
    poshist_clear(&X.hist);
    X.msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    caption_init(GetModuleHandleW(NULL));
    caption_configure(&X.cap);
    caption_on_moved(caption_moved_cb, NULL);
    X.ready = 1;
    tray_update();
    register_hotkeys();
    sync_menu_checks();
    if (X.hk_report[0]) set_status(X.hk_report);
}

static void ext_shutdown(void)
{
    X.ready = 0;
    UnregisterHotKey(g.hwnd, HK_ID_LEADER);
    disarm();
    tray_remove();
    caption_shutdown();
    for (int i = 0; i < 3; i++) if (X.icons[i]) DestroyIcon(X.icons[i]);
}

/* ------------------------------------------------------------------ layout */

static void layout(int w, int h)
{
    int m = S(8), rowH = S(26), gap = S(12);
    int y = m, x = m, ly = y + S(4), lh = S(18);

    MoveWindow(g.lblVoice, x, ly, S(40), lh, TRUE);          x += S(44);
    MoveWindow(g.voice,    x, y, S(270), S(240), TRUE);      x += S(270) + gap;
    MoveWindow(g.lblRate,  x, ly, S(34), lh, TRUE);          x += S(36);
    MoveWindow(g.rate,     x, y, S(120), rowH, TRUE);        x += S(120);
    MoveWindow(g.rateVal,  x, ly, S(28), lh, TRUE);          x += S(28) + gap;
    MoveWindow(g.lblVol,   x, ly, S(50), lh, TRUE);          x += S(52);
    MoveWindow(g.vol,      x, y, S(120), rowH, TRUE);        x += S(120);
    MoveWindow(g.volVal,   x, ly, S(40), lh, TRUE);

    y += rowH + m;
    x = m;
    MoveWindow(g.prev,      x, y, S(60), S(28), TRUE);  x += S(66);
    MoveWindow(g.play,      x, y, S(90), S(28), TRUE);  x += S(96);
    MoveWindow(g.next,      x, y, S(60), S(28), TRUE);  x += S(66);
    MoveWindow(g.stop,      x, y, S(70), S(28), TRUE);  x += S(80);
    MoveWindow(g.exportBtn, x, y, S(120), S(28), TRUE);

    SendMessageW(g.status, WM_SIZE, 0, 0);
    int parts[3] = { w - S(580), w - S(120), -1 };
    SendMessageW(g.status, SB_SETPARTS, 3, (LPARAM)parts);
    RECT sr;
    GetWindowRect(g.status, &sr);
    int sh = sr.bottom - sr.top;

    y += S(28) + m;
    int gw = S(18);
    MoveWindow(g.gutter, 0, y, gw, h - y - sh, TRUE);
    MoveWindow(g.edit, gw, y, w - gw, h - y - sh, TRUE);
    gutter_refresh(g.gutter);
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           g.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static BOOL CALLBACK set_font_cb(HWND c, LPARAM f)
{
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU file = newpop(), edit = newpop(), play = newpop(), mark = newpop(), text = newpop(), caps = newpop(), tools = newpop(), help = newpop();

    /* File */
    AppendMenuW(file, MF_STRING, CMD_NEW,        L"&New blank document\tCtrl+N");
    AppendMenuW(file, MF_STRING, CMD_OPEN,       L"&Open text file...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    HMENU proj = newpop();
    AppendMenuW(proj, MF_STRING, CMD_PROJ_NEW,   L"&New project from this text...");
    AppendMenuW(proj, MF_STRING, CMD_PROJ_OPEN,  L"&Open project...");
    AppendMenuW(proj, MF_STRING, CMD_PROJ_SAVE,  L"&Save project\tCtrl+S");
    AppendMenuW(proj, MF_STRING, CMD_PROJ_CLOSE, L"&Close project");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)proj,  L"&Project");
    HMENU gen = newpop();
    AppendMenuW(gen, MF_STRING, CMD_EXPORT,      L"&Whole document...\tCtrl+E");
    AppendMenuW(gen, MF_STRING, CMD_EXPORT_SEL,  L"The &selection...\tCtrl+Shift+E");
    AppendMenuW(gen, MF_STRING, CMD_EXPORT_CH,   L"The current &chapter...");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)gen,   L"&Generate audio / export WAV");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, CMD_EXIT,       L"E&xit");

    /* Edit */
    AppendMenuW(edit, MF_STRING, CMD_SELALL,     L"Select &all\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, CMD_FIND,       L"&Find and replace...\tCtrl+F");
    AppendMenuW(edit, MF_STRING, CMD_FIND_NEXT,  L"Find &next\tF3");
    AppendMenuW(edit, MF_STRING, CMD_FIND_PREV,  L"Find &previous\tShift+F3");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, CMD_GOTO,       L"&Go to...\tCtrl+G");

    /* Playback */
    AppendMenuW(play, MF_STRING, CMD_PLAY,       L"&Play from cursor / Pause\tF5");
    AppendMenuW(play, MF_STRING, CMD_STOP,       L"S&top\tEsc");
    AppendMenuW(play, MF_STRING, CMD_REPLAY,     L"&Replay sentence\tF7");
    AppendMenuW(play, MF_SEPARATOR, 0, NULL);
    HMENU only = newpop();
    AppendMenuW(only, MF_STRING, CMD_PLAY_SEL,   L"The &selection\tCtrl+F5");
    AppendMenuW(only, MF_STRING, CMD_PLAY_SENT,  L"This &sentence only\tF6");
    AppendMenuW(only, MF_STRING, CMD_PLAY_PARA,  L"This &paragraph only\tShift+F6");
    AppendMenuW(only, MF_STRING, CMD_PLAY_CH,    L"This &chapter only");
    AppendMenuW(play, MF_POPUP, (UINT_PTR)only,  L"Play o&nly");
    HMENU move = newpop();
    AppendMenuW(move, MF_STRING, CMD_PREV,       L"Previous sentence\tAlt+Left");
    AppendMenuW(move, MF_STRING, CMD_NEXT,       L"Next sentence\tAlt+Right");
    AppendMenuW(move, MF_STRING, CMD_PREV_PARA,  L"Previous paragraph\tAlt+Up");
    AppendMenuW(move, MF_STRING, CMD_NEXT_PARA,  L"Next paragraph\tAlt+Down");
    AppendMenuW(move, MF_STRING, CMD_PREV_CH,    L"Previous chapter\tCtrl+PgUp");
    AppendMenuW(move, MF_STRING, CMD_NEXT_CH,    L"Next chapter\tCtrl+PgDn");
    AppendMenuW(play, MF_POPUP, (UINT_PTR)move,  L"&Move");
    HMENU jump = newpop();
    AppendMenuW(jump, MF_STRING, CMD_JUMP_BACK,  L"Back to where I was\tAlt+Backspace");
    AppendMenuW(jump, MF_STRING, CMD_JUMP_FWD,   L"Forward again\tAlt+Shift+Backspace");
    AppendMenuW(jump, MF_STRING, CMD_SKIP_BACK,  L"Back 30 seconds (estimate)\tCtrl+,");
    AppendMenuW(jump, MF_STRING, CMD_SKIP_FWD,   L"Forward 30 seconds (estimate)\tCtrl+.");
    AppendMenuW(play, MF_POPUP, (UINT_PTR)jump,  L"&Jump");
    AppendMenuW(play, MF_STRING, CMD_GOTO,       L"&Go to...\tCtrl+G");
    AppendMenuW(play, MF_SEPARATOR, 0, NULL);
    HMENU spd = newpop();
    AppendMenuW(spd, MF_STRING, CMD_RATE_UP,     L"Faster");
    AppendMenuW(spd, MF_STRING, CMD_RATE_DOWN,   L"Slower");
    AppendMenuW(play, MF_POPUP, (UINT_PTR)spd,   L"S&peed");
    AppendMenuW(play, MF_STRING, CMD_PROFILES,   L"&Voice profiles...");

    /* Mark */
    AppendMenuW(mark, MF_STRING, CMD_FLAG_BASE,                L"&Bookmark this passage\tCtrl+B");
    HMENU flags = newpop();
    AppendMenuW(flags, MF_STRING, CMD_FLAG_BASE + MK_REVISIT,   L"&Revisit\tAlt+1");
    AppendMenuW(flags, MF_STRING, CMD_FLAG_BASE + MK_IMPORTANT, L"&Important\tAlt+2");
    AppendMenuW(flags, MF_STRING, CMD_FLAG_BASE + MK_UNCLEAR,   L"&Unclear\tAlt+3");
    AppendMenuW(flags, MF_STRING, CMD_FLAG_BASE + MK_QUESTION,  L"&Question\tAlt+4");
    AppendMenuW(flags, MF_STRING, CMD_FLAG_BASE + MK_DOUBT,     L"&Doubt (skeptical)\tAlt+5");
    AppendMenuW(mark, MF_POPUP, (UINT_PTR)flags,               L"&Flag this passage");
    AppendMenuW(mark, MF_STRING, CMD_MARK_NOTE,                L"Add or edit a &note on the mark here...\tCtrl+M");
    AppendMenuW(mark, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mark, MF_STRING, CMD_BM_NEXT,                  L"&Next mark\tF2");
    AppendMenuW(mark, MF_STRING, CMD_BM_PREV,                  L"&Previous mark\tShift+F2");
    AppendMenuW(mark, MF_STRING, CMD_NAV,                      L"&Manage marks and chapters...\tCtrl+Shift+N");

    /* Text processing */
    AppendMenuW(text, MF_STRING, CMD_CLEANUP,        L"&Cleanup options and preview...\tCtrl+Shift+C");
    AppendMenuW(text, MF_STRING, CMD_PRON,           L"&Pronunciation editor...\tCtrl+Shift+P");
    AppendMenuW(text, MF_STRING, CMD_SPEECH_PREVIEW, L"Speech text pre&view...\tCtrl+Shift+V");
    AppendMenuW(text, MF_STRING, CMD_ANALYZE,        L"Pro&blem words (report)...");

    /* Captions */
    AppendMenuW(caps, MF_STRING, CMD_CAP_TOGGLE,     L"&Show captions on the screen\tCtrl+Shift+L");
    AppendMenuW(caps, MF_STRING, CMD_CAP_DIALOG,     L"Caption &settings (font, colors, size, position)...");
    AppendMenuW(caps, MF_STRING, CMD_CAP_MOVE,       L"&Move captions by dragging");
    AppendMenuW(caps, MF_SEPARATOR, 0, NULL);
    AppendMenuW(caps, MF_STRING, CMD_CAP_BIGGER,     L"&Larger text");
    AppendMenuW(caps, MF_STRING, CMD_CAP_SMALLER,    L"Smaller &text");

    /* Tools */
    AppendMenuW(tools, MF_STRING, CMD_VOICES_LIST,   L"&Installed voices...");
    AppendMenuW(tools, MF_STRING, CMD_VOICES_RESCAN, L"&Rescan voices");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    HMENU tray = newpop();
    AppendMenuW(tray, MF_STRING, CMD_HK_ENABLE,      L"&Global shortcuts (work in other programs)");
    AppendMenuW(tray, MF_STRING, CMD_HK_DIALOG,      L"Shortcut &settings...");
    AppendMenuW(tray, MF_STRING, CMD_HK_HELP,        L"Show &all the shortcut keys...");
    AppendMenuW(tray, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tray, MF_STRING, CMD_TRAY_MIN,       L"&Minimizing hides the window in the tray");
    AppendMenuW(tray, MF_STRING, CMD_TRAY_CLOSE,     L"&Closing the window keeps it in the tray");
    AppendMenuW(tools, MF_POPUP, (UINT_PTR)tray,     L"&Tray and global shortcuts");
    HMENU dat = newpop();
    AppendMenuW(dat, MF_STRING, CMD_DATA,            L"&Show or change where data is kept...");
    AppendMenuW(dat, MF_STRING, CMD_DATA_RESET,      L"&Reset to the default location");
    AppendMenuW(tools, MF_POPUP, (UINT_PTR)dat,      L"&Data location (settings, dictionary, cache)");
    AppendMenuW(tools, MF_STRING, CMD_HOMEEND,       L"&Home/End jump to document start/end");
    g.menu_tools = tools;

    AppendMenuW(help, MF_STRING, CMD_ABOUT, L"&About and FAQ");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file,  L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit,  L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)play,  L"&Playback");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mark,  L"&Mark");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)text,  L"Te&xt");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)caps,  L"&Captions");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help,  L"&Help");
    SetMenu(hwnd, bar);
}

/* Home/End in the editor: jump to the start/end of the DOCUMENT instead of the line.
 * The edit control already does exactly that for Ctrl+Home/Ctrl+End (including Shift
 * to extend the selection), so we simply present the key as Ctrl+Home/Ctrl+End. */
static LRESULT CALLBACK edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_KEYDOWN && (wp == VK_HOME || wp == VK_END) && g.home_end_doc &&
        !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        BYTE ks[256];
        if (GetKeyboardState(ks)) {
            BYTE saved = ks[VK_CONTROL];
            ks[VK_CONTROL] |= 0x80;
            SetKeyboardState(ks);
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            ks[VK_CONTROL] = saved;
            SetKeyboardState(ks);
            if (g.state == ST_IDLE) update_position(caret_sentence());
            return r;
        }
    }
    if (msg == WM_NCDESTROY) { RemoveWindowSubclass(h, edit_subclass, 1); return DefSubclassProc(h, msg, wp, lp); }
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (g.gutter && (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN || msg == WM_CHAR ||
                     msg == WM_SETTEXT || msg == WM_SIZE || msg == WM_LBUTTONUP || msg == WM_PASTE ||
                     msg == WM_CUT || msg == WM_UNDO || msg == EM_LINESCROLL || msg == EM_SCROLLCARET))
        gutter_refresh(g.gutter);
    return r;
}

static int on_create(HWND hwnd)
{
    g.hwnd = hwnd;
    HDC dc = GetDC(NULL);
    g.dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);

    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);

    build_menu(hwnd);
    g.lblVoice  = mk(L"STATIC", L"&Voice", SS_LEFT, 0, 0);
    g.voice     = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, ID_VOICE);
    g.lblRate   = mk(L"STATIC", L"Rate", SS_LEFT, 0, 0);
    g.rate      = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, ID_RATE);
    g.rateVal   = mk(L"STATIC", L"0", SS_LEFT, 0, 0);
    g.lblVol    = mk(L"STATIC", L"Volume", SS_LEFT, 0, 0);
    g.vol       = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, ID_VOLUME);
    g.volVal    = mk(L"STATIC", L"100%", SS_LEFT, 0, 0);
    g.prev      = mk(L"BUTTON", L"Pre&v", BS_PUSHBUTTON | WS_TABSTOP, 0, CMD_PREV);
    g.play      = mk(L"BUTTON", L"&Play", BS_PUSHBUTTON | WS_TABSTOP, 0, CMD_PLAY);
    g.next      = mk(L"BUTTON", L"&Next", BS_PUSHBUTTON | WS_TABSTOP, 0, CMD_NEXT);
    g.stop      = mk(L"BUTTON", L"&Stop", BS_PUSHBUTTON | WS_TABSTOP, 0, CMD_STOP);
    g.exportBtn = mk(L"BUTTON", L"&Generate audio...", BS_PUSHBUTTON | WS_TABSTOP, 0, CMD_EXPORT);
    g.edit      = mk(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL |
                                   WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_EDIT);
    g.status    = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_STATUS, GetModuleHandleW(NULL), NULL);
    g.gutter    = gutter_create(hwnd, g.edit, ID_GUTTER);
    EnumChildWindows(hwnd, set_font_cb, (LPARAM)g.font);

    SendMessageW(g.edit, EM_SETLIMITTEXT, 256u * 1024u * 1024u, 0);
    SetWindowSubclass(g.edit, edit_subclass, 1, 0);
    SendMessageW(g.rate, TBM_SETRANGE, TRUE, MAKELONG(-10, 10));
    SendMessageW(g.rate, TBM_SETTICFREQ, 5, 0);
    SendMessageW(g.vol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(g.vol, TBM_SETTICFREQ, 25, 0);
    SendMessageW(g.vol, TBM_SETPAGESIZE, 0, 10);

    wchar_t err[TTS_ERR_MAX];
    init_paths();
    g.engine = tts_engine_create(err, TTS_ERR_MAX);
    int haveList = tts_voices_enumerate(&g.voices, err, TTS_ERR_MAX) == TTS_SUCCESS;
    if (!g.engine || !haveList) {
        set_status(err);
    } else {
        populate_voices(NULL);
    }
    doc_init(&g.doc);
    g.cur_sent = -1;
    if (g.engine) player_init(&g.player, g.engine);
    {   Ini pi; read_ini_file(g.settings_path, &pi); profiles_from_ini(&g.profiles, &pi); ini_free(&pi); }
    int missing = load_settings();
    CheckMenuItem(g.menu_tools, CMD_HOMEEND, MF_BYCOMMAND | (g.home_end_doc ? MF_CHECKED : MF_UNCHECKED));
    load_dictionary();
    open_cache();
    update_value_labels();
    update_controls();
    update_char_count();
    if (g.engine && haveList)
        set_status(missing ? L"The previously selected voice is no longer installed; using the first available voice."
                           : L"Ready. Type or paste text, or use File > Open.");
    update_title();
    {   /* reopen the project that was open when the program was last closed */
        Ini a;
        read_ini_file(g.settings_path, &a);
        const wchar_t *lp = ini_get(&a, L"App", L"LastProject");
        wchar_t last[MAX_PATH];
        last[0] = 0;
        if (lp) wcsncpy(last, lp, MAX_PATH - 1);
        ini_free(&a);
        if (last[0] && GetFileAttributesW(last) != INVALID_FILE_ATTRIBUTES) project_open_path(last);
    }
    DragAcceptFiles(g.hwnd, TRUE);
    ext_init();
    SetFocus(g.edit);
    return 0;
}

/* The window position and size are remembered in settings.txt (not in projects). */
static void save_window_placement(void)
{
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof wp);
    wp.length = sizeof wp;
    if (!GetWindowPlacement(g.hwnd, &wp)) return;
    Ini a;
    read_ini_file(g.settings_path, &a);
    ini_set_int(&a, L"Window", L"Left", wp.rcNormalPosition.left);
    ini_set_int(&a, L"Window", L"Top", wp.rcNormalPosition.top);
    ini_set_int(&a, L"Window", L"Width", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    ini_set_int(&a, L"Window", L"Height", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    ini_set_int(&a, L"Window", L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    write_ini_file(g.settings_path, &a, 0);
    ini_free(&a);
}

/* Returns SW_SHOWMAXIMIZED or `show`; moves the window to its remembered place if that is still on a screen. */
static int restore_window_placement(HWND hwnd, int show)
{
    Ini a;
    read_ini_file(g.settings_path, &a);
    long l = ini_get_int(&a, L"Window", L"Left", -32000), t = ini_get_int(&a, L"Window", L"Top", -32000);
    long w = ini_get_int(&a, L"Window", L"Width", 0), h = ini_get_int(&a, L"Window", L"Height", 0);
    int max = ini_get_int(&a, L"Window", L"Maximized", 0) != 0;
    ini_free(&a);
    if (w >= 300 && h >= 200 && l != -32000) {
        RECT r = { l, t, l + w, t + h };
        HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (m) SetWindowPos(hwnd, NULL, l, t, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return max ? SW_SHOWMAXIMIZED : show;
}

static void close_app(void)
{
    if (!confirm_discard()) return;
    if (g.state == ST_PLAYING || g.state == ST_PAUSED) player_stop(&g.player);
    save_window_placement();
    save_settings();
    DestroyWindow(g.hwnd);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        return on_create(hwnd) ? -1 : 0;

    case WM_MEASUREITEM:
        if (wp == 0 && tray_measure((MEASUREITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_DRAWITEM:
        if (wp == 0 && tray_draw((const DRAWITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && X.min_to_tray) { tray_update(); if (X.tray_added) { ShowWindow(hwnd, SW_HIDE); return 0; } }
        break;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && X.min_to_tray) { tray_update(); if (X.tray_added) { ShowWindow(hwnd, SW_HIDE); return 0; } }
        if (g.status) layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(780);
        mm->ptMinTrackSize.y = S(360);
        return 0;
    }

    case WM_SETFOCUS:
        if (g.edit) SetFocus(g.edit);
        return 0;

    case WM_HSCROLL:
        int prev_rate = shown_rate_value;
        update_value_labels();
        if (g.state == ST_PLAYING || g.state == ST_PAUSED) {   /* live adjust */
            if ((HWND)lp == g.rate) rate_changed_live(prev_rate);
            else { TTSSpeechSettings s = current_settings(); tts_apply_settings(g.engine, &s, NULL, 0); }
        }
        return 0;

    case WM_TIMER:
        if (wp == POLL_TIMER_ID) on_poll_timer();
        else if (wp == TIMER_ARM) disarm();
        else if (wp == TIMER_ANCHOR) { if (g_anchor && IsWindowEnabled(g_anchor)) { ShowWindow(g_anchor, SW_HIDE); KillTimer(hwnd, TIMER_ANCHOR); } }
        else if (wp == TIMER_RATE) { KillTimer(hwnd, TIMER_RATE); if (g.state == ST_PLAYING) restart_here(); }
        else if (wp == DOC_TIMER_ID) {
            KillTimer(hwnd, DOC_TIMER_ID);
            if (g.doc_dirty) {
                rebuild_doc();
                if (g.state == ST_IDLE) update_position(caret_sentence());
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_PLAY:   play_from_cursor(); return 0;
        case CMD_PLAY_TRAY: toggle_play(); return 0;
        case CMD_STOP:
            if (g.state != ST_IDLE) stop_playback(NULL);
            return 0;
        case CMD_EXPORT:     open_generate(0); return 0;
        case CMD_EXPORT_CH:  open_generate(2); return 0;
        case CMD_EXPORT_SEL: open_generate(1); return 0;
        case CMD_OPEN:       open_file(); return 0;
        case CMD_PROJ_NEW:   project_new(); return 0;
        case CMD_PROJ_OPEN:  project_open_dialog(); return 0;
        case CMD_PROJ_SAVE:  project_save(); return 0;
        case CMD_PROJ_CLOSE: project_close(); return 0;
        case CMD_NEW:        new_document(); return 0;
        case CMD_FIND:       open_find(); return 0;
        case CMD_PROFILES:   open_profiles(); return 0;
        case CMD_ANALYZE:    open_analyze(); return 0;
        case CMD_FIND_NEXT:  find_dialog_next(1); return 0;
        case CMD_FIND_PREV:  find_dialog_next(-1); return 0;
        case CMD_VOICES_LIST: show_voices(); return 0;
        case CMD_VOICES_RESCAN: rescan_voices(); return 0;
        case CMD_DATA:       data_location(); return 0;
        case CMD_DATA_RESET: data_location_reset(); return 0;
        case CMD_PREV_CH:    nav_chapter(-1); return 0;
        case CMD_NEXT_CH:    nav_chapter(+1); return 0;
        case CMD_PLAY_CH:    play_chapter(); return 0;
        case CMD_NAV:        open_nav(); return 0;
        case CMD_BM_TOGGLE:  toggle_bookmark(); return 0;
        case CMD_BM_NEXT:    goto_bookmark(+1); return 0;
        case CMD_BM_PREV:    goto_bookmark(-1); return 0;
        case CMD_CLEANUP:    open_cleanup(); return 0;
        case CMD_PRON:       open_pron(); return 0;
        case CMD_HOMEEND:
            g.home_end_doc = !g.home_end_doc;
            CheckMenuItem(g.menu_tools, CMD_HOMEEND, MF_BYCOMMAND | (g.home_end_doc ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case CMD_SPEECH_PREVIEW: open_speech_preview(); return 0;
        case CMD_PREV:       nav_sentence(-1); return 0;
        case CMD_NEXT:       nav_sentence(+1); return 0;
        case CMD_PREV_PARA:  nav_paragraph(-1); return 0;
        case CMD_NEXT_PARA:  nav_paragraph(+1); return 0;
        case CMD_PLAY_SENT:  play_sentence_only(); return 0;
        case CMD_PLAY_PARA:  play_paragraph(); return 0;
        case CMD_PLAY_SEL:   play_selection(); return 0;
        case CMD_REPLAY:     replay_sentence(); return 0;
        default: if (ext_command(LOWORD(wp))) return 0; break;
        case CMD_SELALL: SendMessageW(g.edit, EM_SETSEL, 0, -1); return 0;
        case CMD_EXIT:   close_app(); return 0;
        case CMD_ABOUT:
            about_dialog(hwnd, g.font, g.dpi);
            return 0;
        case ID_EDIT:
            if (HIWORD(wp) == EN_CHANGE) {
                update_char_count();
                if (g.state == ST_PLAYING || g.state == ST_PAUSED)
                    stop_playback(L"Stopped: text was edited.");
                g.doc_dirty = 1;
                g.cur_sent = -1;
                if (g.project_path[0] && !g.dirty) { g.dirty = 1; update_title(); }
                SetTimer(hwnd, DOC_TIMER_ID, DOC_DEBOUNCE_MS, NULL);   /* restarts on each keystroke */
            }
            return 0;
        }
        break;

    case WM_DROPFILES: {
        wchar_t f[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, f, MAX_PATH) && g.state == ST_IDLE) {
            if (wcslen(f) > 5 && _wcsicmp(f + wcslen(f) - 5, L".ttsp") == 0) { if (confirm_discard()) project_open_path(f); }
            else if (g.project_path[0] && g.dirty && !confirm_discard()) { /* cancelled */ }
            else load_text_path(f);
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    case WM_CLOSE:
        if (X.close_to_tray && X.tray_added) { ShowWindow(hwnd, SW_HIDE); return 0; }
        close_app();
        return 0;

    case WM_HOTKEY: on_hotkey((int)wp); return 0;

    case TRAY_MSG:
        if (LOWORD(lp) == WM_LBUTTONUP) { if (g.doc.nsent == 0 && g.state == ST_IDLE && !IsWindowVisible(hwnd)) show_main(); else do_action(ACT_PLAYPAUSE); }
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) { show_main(); }
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) tray_menu();
        return 0;

    case WM_GUTTER_RCLICK:
        gutter_context((int)wp, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_DESTROY:
        ext_shutdown();
        if (g_anchor) DestroyWindow(g_anchor);
        KillTimer(hwnd, POLL_TIMER_ID);
        KillTimer(hwnd, DOC_TIMER_ID);
        tts_engine_destroy(g.engine);
        g.engine = NULL;
        doc_free(&g.doc);
        preproc_free(&g.pre);
        pron_free(&g.dict);
        if (g.cache_ok) cache_close(&g.cache);
        tts_voices_free(&g.voices);
        profiles_free(&g.profiles);
        if (g.font) DeleteObject(g.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM initialisation failed.", APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MouthpieceMain";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) { CoUninitialize(); return 1; }

    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, APP_TITLE,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(860, dpi, 96), MulDiv(580, dpi, 96),
                                NULL, NULL, inst, NULL);
    if (!hwnd) { CoUninitialize(); return 1; }

    ACCEL acc[] = {
        { FVIRTKEY,                       VK_F5,    CMD_PLAY },
        { FVIRTKEY | FCONTROL,            VK_F5,    CMD_PLAY_SEL },
        { FVIRTKEY,                       VK_F6,    CMD_PLAY_SENT },
        { FVIRTKEY | FSHIFT,              VK_F6,    CMD_PLAY_PARA },
        { FVIRTKEY,                       VK_F7,    CMD_REPLAY },
        { FVIRTKEY | FALT,                VK_LEFT,  CMD_PREV },
        { FVIRTKEY | FALT,                VK_RIGHT, CMD_NEXT },
        { FVIRTKEY | FALT,                VK_UP,    CMD_PREV_PARA },
        { FVIRTKEY | FALT,                VK_DOWN,  CMD_NEXT_PARA },
        { FVIRTKEY | FCONTROL,            'O',      CMD_OPEN },
        { FVIRTKEY | FCONTROL,            'E',      CMD_EXPORT },
        { FVIRTKEY | FCONTROL | FSHIFT,   'E',      CMD_EXPORT_SEL },
        { FVIRTKEY | FCONTROL,            'A',      CMD_SELALL },
        { FVIRTKEY | FCONTROL | FSHIFT,   'C',      CMD_CLEANUP },
        { FVIRTKEY | FCONTROL | FSHIFT,   'P',      CMD_PRON },
        { FVIRTKEY | FCONTROL,            'S',      CMD_PROJ_SAVE },
        { FVIRTKEY | FCONTROL,            VK_PRIOR, CMD_PREV_CH },
        { FVIRTKEY | FCONTROL,            VK_NEXT,  CMD_NEXT_CH },
        { FVIRTKEY | FCONTROL,            'B',      CMD_FLAG_BASE },
        { FVIRTKEY | FALT,                '1',      CMD_FLAG_BASE + MK_REVISIT },
        { FVIRTKEY | FALT,                '2',      CMD_FLAG_BASE + MK_IMPORTANT },
        { FVIRTKEY | FALT,                '3',      CMD_FLAG_BASE + MK_UNCLEAR },
        { FVIRTKEY | FALT,                '4',      CMD_FLAG_BASE + MK_QUESTION },
        { FVIRTKEY | FALT,                '5',      CMD_FLAG_BASE + MK_DOUBT },
        { FVIRTKEY | FCONTROL,            'M',      CMD_MARK_NOTE },
        { FVIRTKEY | FCONTROL,            'G',      CMD_GOTO },
        { FVIRTKEY | FALT,                VK_BACK,  CMD_JUMP_BACK },
        { FVIRTKEY | FALT | FSHIFT,       VK_BACK,  CMD_JUMP_FWD },
        { FVIRTKEY | FCONTROL,            VK_OEM_COMMA,  CMD_SKIP_BACK },
        { FVIRTKEY | FCONTROL,            VK_OEM_PERIOD, CMD_SKIP_FWD },
        { FVIRTKEY | FCONTROL | FSHIFT,   'L',      CMD_CAP_TOGGLE },
        { FVIRTKEY,                       VK_F2,    CMD_BM_NEXT },
        { FVIRTKEY | FSHIFT,              VK_F2,    CMD_BM_PREV },
        { FVIRTKEY | FCONTROL | FSHIFT,   'N',      CMD_NAV },
        { FVIRTKEY | FCONTROL,            'N',      CMD_NEW },
        { FVIRTKEY | FCONTROL,            'F',      CMD_FIND },
        { FVIRTKEY,                       VK_F3,    CMD_FIND_NEXT },
        { FVIRTKEY | FSHIFT,              VK_F3,    CMD_FIND_PREV },
        { FVIRTKEY | FCONTROL | FSHIFT,   'V',      CMD_SPEECH_PREVIEW },
    };
    HACCEL haccel = CreateAcceleratorTableW(acc, (int)(sizeof acc / sizeof acc[0]));

    show = restore_window_placement(hwnd, show);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        /* Esc stops playback/export, except while the voice drop-down is open. */
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && IsChild(hwnd, m.hwnd) &&
            !SendMessageW(g.voice, CB_GETDROPPEDSTATE, 0, 0)) {
            PostMessageW(hwnd, WM_COMMAND, CMD_STOP, 0);
            continue;
        }
        if (find_dialog_hwnd() && IsDialogMessageW(find_dialog_hwnd(), &m)) continue;
        if (TranslateAcceleratorW(hwnd, haccel, &m)) continue;
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyAcceleratorTable(haccel);
    CoUninitialize();
    return (int)m.wParam;
}
