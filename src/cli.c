/*
 * cli.c - command line front end ("MouthpieceCLI.exe"). Uses the same engine as the GUI:
 * the same settings files (config.c), cleanup, document model, speech rules and
 * pronunciation dictionary, the same Speech IR, audio cache, generation worker
 * (gen.c) and WAV assembly. Nothing here re-implements synthesis.
 *
 *   tts INPUT [options]          INPUT is a .txt file or a project (.ttsp)
 *   tts --list-voices
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "tts_engine.h"
#include "util.h"
#include "doc.h"
#include "preproc.h"
#include "pron.h"
#include "speech.h"
#include "utf8.h"
#include "cache.h"
#include "ir.h"
#include "chapters.h"
#include "config.h"
#include "gen.h"
#include "pronscope.h"
#include "analyze.h"
#include "plat.h"

static void out(const wchar_t *fmt, ...)
{
    wchar_t b[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(b, 2047, fmt, ap);
    va_end(ap);
    b[2047] = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, n;
    size_t len = wcslen(b);
    if (GetConsoleMode(h, &mode)) { WriteConsoleW(h, b, (DWORD)len, &n, NULL); return; }
    char u[6200];
    int k = WideCharToMultiByte(CP_UTF8, 0, b, (int)len, u, sizeof u, NULL, NULL);
    if (k > 0) WriteFile(h, u, (DWORD)k, &n, NULL);
}

static void errout(const wchar_t *fmt, ...)
{
    wchar_t b[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(b, 2047, fmt, ap);
    va_end(ap);
    b[2047] = 0;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode, n;
    size_t len = wcslen(b);
    if (GetConsoleMode(h, &mode)) { WriteConsoleW(h, b, (DWORD)len, &n, NULL); return; }
    char u[6200];
    int k = WideCharToMultiByte(CP_UTF8, 0, b, (int)len, u, sizeof u, NULL, NULL);
    if (k > 0) WriteFile(h, u, (DWORD)k, &n, NULL);
}

static void usage(void)
{
    out(L"Mouthpiece command line\n\n"
        L"  tts INPUT [options]        INPUT: a text file (.txt) or a project (.ttsp)\n"
        L"  tts --list-voices          list the installed voices\n\n"
        L"Actions (default is --build):\n"
        L"  --build                    synthesize (missing segments only) and write one WAV file\n"
        L"  --export-chapters DIR      write one WAV per chapter into DIR\n"
        L"  --profile NAME             use a saved voice profile (voice, rate, volume) from the GUI's settings\n"
        L"  --analyze                  dry run: counts, chapters, cache status, estimated length; nothing is synthesized\n\n"
        L"Options:\n"
        L"  --output FILE              output WAV (default: next to the input, or <project>\\export)\n"
        L"  --voice NAME               voice id, or part of its name (see --list-voices)\n"
        L"  --rate N                   -10..10        --volume N   0..100\n"
        L"  --sentence-pause MS  --paragraph-pause MS  --chapter-pause MS\n"
        L"  --normalize                two-pass peak normalization       --no-trim   keep leading/trailing silence\n"
        L"  --no-cleanup               skip the text cleanup stage\n"
        L"  --pronunciations FILE      pronunciation dictionary (default: the project's, else the global one)\n"
        L"  --cache DIR                audio cache folder (default: the project's, else the global one)\n"
        L"  --help\n\n"
        L"Exit code: 0 success, 1 error, 2 bad arguments.\n"
        L"Settings (voice, cleanup, speech rules, pauses) come from the project, or from the GUI's settings.txt;\n"
        L"command line options override them.\n");
}

/* ---- data folder: same rules as the GUI ---- */
static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void data_dir(wchar_t *dir)
{
    wchar_t exe[MAX_PATH], ad[MAX_PATH];
    dir[0] = 0;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *sl = wcsrchr(exe, L'\\');
        if (sl) {
            *sl = 0;
            _snwprintf(dir, MAX_PATH, L"%ls\\MouthpieceData", exe);
            dir[MAX_PATH - 1] = 0;
            if (dir_exists(dir)) return;
            dir[0] = 0;
        }
    }
    ad[0] = 0;
    n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return;
    wcscat(ad, L"\\Mouthpiece");
    if (!dir_exists(ad)) {
        wchar_t old[MAX_PATH];
        wcscpy(old, ad);
        wchar_t *k = wcsrchr(old, L'\\');
        if (k) { wcscpy(k, L"\\TTSForge"); if (dir_exists(old)) { if (!MoveFileExW(old, ad, 0)) wcscpy(ad, old); } }
    }
    wchar_t ptr[MAX_PATH], err[TTS_ERR_MAX];
    _snwprintf(ptr, MAX_PATH, L"%ls\\datadir.txt", ad);
    ptr[MAX_PATH - 1] = 0;
    unsigned char *buf = NULL;
    size_t len = 0;
    int missing = 0;
    if (util_read_file(ptr, &buf, &len, &missing, err, TTS_ERR_MAX) && buf) {
        wchar_t w[MAX_PATH];
        size_t off = (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
        int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)buf + off, (int)(len - off), w, MAX_PATH - 1);
        free(buf);
        if (wn > 0) {
            size_t k = (size_t)wn;
            while (k && (w[k - 1] == L'\r' || w[k - 1] == L'\n' || w[k - 1] == L' ')) k--;
            w[k] = 0;
            if (k && dir_exists(w)) { wcscpy(dir, w); return; }
        }
    }
    wcscpy(dir, ad);
}

static int read_ini(const wchar_t *path, Ini *d)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    int missing = 0;
    wchar_t err[TTS_ERR_MAX];
    ini_init(d);
    if (!util_read_file(path, &buf, &len, &missing, err, TTS_ERR_MAX)) return 0;
    ini_from_utf8(d, (const char *)buf, len);
    free(buf);
    return 1;
}

static int ends_with_i(const wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && _wcsicmp(s + a - b, suf) == 0;
}

static int want_int(const wchar_t *s, int lo, int hi, int *v, const wchar_t *name)
{
    wchar_t *e;
    long x = wcstol(s, &e, 10);
    if (*s == 0 || *e != 0 || x < lo || x > hi) { errout(L"%ls must be a number from %d to %d.\n", name, lo, hi); return 0; }
    *v = (int)x;
    return 1;
}

static void fmt_time(double sec, wchar_t *b, size_t n)
{
    long t = (long)(sec + 0.5);
    _snwprintf(b, n, L"%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    b[n - 1] = 0;
}

static void progress(GenJob *j)
{
    LONG last = -1;
    while (WaitForSingleObject(j->thread, 250) == WAIT_TIMEOUT) {
        LONG d = j->done, t = j->total;
        if (d != last && t > 0) {
            errout(L"\r%ls %ld/%ld   ", j->phase ? L"assembling " : L"synthesizing", d, t);
            last = d;
        }
    }
    errout(L"\r                                    \r");
}

int wmain(int argc, wchar_t **argv)
{
    enum { A_BUILD, A_CHAPTERS, A_ANALYZE } action = A_BUILD;
    const wchar_t *input = NULL, *output = NULL, *chdir_ = NULL, *voice_arg = NULL, *profile_arg = NULL, *dict_arg = NULL, *cache_arg = NULL;
    int rate = 0, volume = 100, have_rate = 0, have_vol = 0, no_cleanup = 0, no_trim = 0, normalize = 0;
    int sp = -1, pp = -1, cp = -1, list = 0;

    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        int more = i + 1 < argc;
        if (!wcscmp(a, L"--help") || !wcscmp(a, L"-h") || !wcscmp(a, L"/?")) { usage(); return 0; }
        else if (!wcscmp(a, L"--list-voices")) list = 1;
        else if (!wcscmp(a, L"--build")) action = A_BUILD;
        else if (!wcscmp(a, L"--analyze")) action = A_ANALYZE;
        else if (!wcscmp(a, L"--export-chapters") && more) { action = A_CHAPTERS; chdir_ = argv[++i]; }
        else if (!wcscmp(a, L"--output") && more) output = argv[++i];
        else if (!wcscmp(a, L"--voice") && more) voice_arg = argv[++i];
        else if (!wcscmp(a, L"--profile") && more) profile_arg = argv[++i];
        else if (!wcscmp(a, L"--rate") && more) { if (!want_int(argv[++i], -10, 10, &rate, L"--rate")) return 2; have_rate = 1; }
        else if (!wcscmp(a, L"--volume") && more) { if (!want_int(argv[++i], 0, 100, &volume, L"--volume")) return 2; have_vol = 1; }
        else if (!wcscmp(a, L"--sentence-pause") && more) { if (!want_int(argv[++i], 0, 10000, &sp, L"--sentence-pause")) return 2; }
        else if (!wcscmp(a, L"--paragraph-pause") && more) { if (!want_int(argv[++i], 0, 10000, &pp, L"--paragraph-pause")) return 2; }
        else if (!wcscmp(a, L"--chapter-pause") && more) { if (!want_int(argv[++i], 0, 20000, &cp, L"--chapter-pause")) return 2; }
        else if (!wcscmp(a, L"--normalize")) normalize = 1;
        else if (!wcscmp(a, L"--no-trim")) no_trim = 1;
        else if (!wcscmp(a, L"--no-cleanup")) no_cleanup = 1;
        else if (!wcscmp(a, L"--pronunciations") && more) dict_arg = argv[++i];
        else if (!wcscmp(a, L"--cache") && more) cache_arg = argv[++i];
        else if (a[0] == L'-' && a[1]) { errout(L"Unknown or incomplete option: %ls\nTry: tts --help\n", a); return 2; }
        else if (!input) input = a;
        else { errout(L"Only one input file is accepted (got an extra: %ls).\n", a); return 2; }
    }
    if (argc < 2) { usage(); return 2; }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { errout(L"COM initialisation failed.\n"); return 1; }
    wchar_t err[TTS_ERR_MAX + MAX_PATH];
    int rc = 1;

    TTSVoiceList voices;
    memset(&voices, 0, sizeof voices);
    int have_voices = tts_voices_enumerate(&voices, err, TTS_ERR_MAX) == TTS_SUCCESS;
    if (list) {
        if (!have_voices) { errout(L"%ls\n", err); CoUninitialize(); return 1; }
        for (int i = 0; i < voices.count; i++) out(L"%ls\n    %ls\n", voices.items[i].name, voices.items[i].id);
        tts_voices_free(&voices);
        CoUninitialize();
        return 0;
    }
    if (!input) { errout(L"No input file. Try: tts --help\n"); CoUninitialize(); return 2; }

    /* ---- locate settings, text, dictionary and cache ---- */
    wchar_t proj_dir[MAX_PATH] = L"", data[MAX_PATH], src_path[MAX_PATH], settings_path[MAX_PATH], dict_path[MAX_PATH],
            cache_root[MAX_PATH], base_name[MAX_PATH];
    int is_project = ends_with_i(input, L".ttsp");
    data_dir(data);
    wcsncpy(base_name, input, MAX_PATH - 1);
    base_name[MAX_PATH - 1] = 0;
    if (is_project) {
        wcsncpy(proj_dir, input, MAX_PATH - 1);
        wchar_t *sl = wcsrchr(proj_dir, L'\\');
        if (sl) *sl = 0; else wcscpy(proj_dir, L".");
        _snwprintf(settings_path, MAX_PATH, L"%ls", input);
        _snwprintf(src_path, MAX_PATH, L"%ls\\source.txt", proj_dir);
        _snwprintf(dict_path, MAX_PATH, L"%ls\\pronunciations.txt", proj_dir);
        _snwprintf(cache_root, MAX_PATH, L"%ls\\cache", proj_dir);
    } else {
        _snwprintf(settings_path, MAX_PATH, L"%ls\\settings.txt", data);
        wcsncpy(src_path, input, MAX_PATH - 1);
        _snwprintf(dict_path, MAX_PATH, L"%ls\\pronunciations.txt", data);
        _snwprintf(cache_root, MAX_PATH, L"%ls\\cache", data);
    }
    settings_path[MAX_PATH - 1] = src_path[MAX_PATH - 1] = dict_path[MAX_PATH - 1] = cache_root[MAX_PATH - 1] = 0;
    if (dict_arg) { wcsncpy(dict_path, dict_arg, MAX_PATH - 1); }
    if (cache_arg) { wcsncpy(cache_root, cache_arg, MAX_PATH - 1); }

    Ini ini;
    read_ini(settings_path, &ini);                           /* absent file = defaults */
    PreprocOptions po;
    Speech speech;
    GenSettings gs;
    memset(&speech, 0, sizeof speech);
    config_options_from_ini(&ini, &po, &speech.opts, &gs);
    if (no_cleanup) po.enabled = 0;
    if (sp >= 0) gs.sentence_ms = sp;
    if (pp >= 0) gs.paragraph_ms = pp;
    if (cp >= 0) gs.chapter_ms = cp;
    if (normalize) gs.normalize = 1;
    if (no_trim) gs.trim_silence = 0;
    if (profile_arg) {
        ProfileList pl;
        profiles_from_ini(&pl, &ini);
        int pi = profiles_find(&pl, profile_arg);
        if (pi < 0) { errout(L"No voice profile named \"%ls\". Profiles are created in the GUI (Tools > Voice profiles).\n", profile_arg); profiles_free(&pl); return 2; }
        static wchar_t prof_voice[256];
        wcsncpy(prof_voice, pl.p[pi].voice_id, 255); prof_voice[255] = 0;
        if (!voice_arg) voice_arg = prof_voice;
        if (!have_rate) { rate = pl.p[pi].rate; have_rate = 1; }
        if (!have_vol) { volume = pl.p[pi].volume; have_vol = 1; }
        profiles_free(&pl);
    }
    if (!have_rate) rate = (int)ini_get_int(&ini, L"Speech", L"Rate", 0);
    if (!have_vol) volume = (int)ini_get_int(&ini, L"Speech", L"Volume", 100);
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;

    Overrides ov;
    memset(&ov, 0, sizeof ov);
    if (is_project) overrides_from_ini(&ov, &ini);

    /* ---- voice ---- */
    SynthParams synth;
    memset(&synth, 0, sizeof synth);
    synth.rate = rate;
    synth.volume = volume;
    synth.engine_id = L"sapi5";
    synth.wav_rate = TTS_WAV_RATE;
    synth.chunk_sentences = gs.chunk_sentences;
    synth.chunk_max_chars = 700;
    const wchar_t *want = voice_arg ? voice_arg : ini_get(&ini, L"Speech", L"VoiceId");
    if (action != A_ANALYZE || have_voices) {
        if (!have_voices) { errout(L"%ls\n", err); goto fail_early; }
        int pick = -1;
        if (want && *want) {
            for (int i = 0; i < voices.count && pick < 0; i++) if (!_wcsicmp(voices.items[i].id, want)) pick = i;
            for (int i = 0; i < voices.count && pick < 0; i++) if (wcsstr(voices.items[i].name, want)) pick = i;
            if (pick < 0) {
                wchar_t lw[256], ln[256];
                wcsncpy(lw, want, 255); lw[255] = 0; _wcslwr(lw);
                for (int i = 0; i < voices.count && pick < 0; i++) {
                    wcsncpy(ln, voices.items[i].name, 255); ln[255] = 0; _wcslwr(ln);
                    if (wcsstr(ln, lw)) pick = i;
                }
            }
            if (pick < 0 && voice_arg) { errout(L"No installed voice matches \"%ls\". See: tts --list-voices\n", voice_arg); goto fail_early; }
            if (pick < 0) errout(L"Note: the voice saved in the settings is not installed; using the first one.\n");
        }
        if (pick < 0) pick = 0;
        wcsncpy(synth.voice_id, voices.items[pick].id, 255);
        out(L"Voice: %ls\n", voices.items[pick].name);
    }

    /* ---- text -> document ---- */
    wchar_t *text = NULL;
    if (!util_load_text_file(src_path, &text, err, TTS_ERR_MAX)) { errout(L"%ls\n", err); goto fail_early; }
    PreprocResult pre;
    memset(&pre, 0, sizeof pre);
    if (!preproc_run(text, wcslen(text), &po, 0, &pre)) { errout(L"Out of memory.\n"); free(text); goto fail_early; }
    free(text);
    Document doc;
    doc_init(&doc);
    wchar_t *norm = pre.text;
    size_t nlen = pre.len;
    pre.text = NULL;
    if (!doc_build_take(&doc, norm, nlen)) { errout(L"Out of memory.\n"); preproc_free(&pre); goto fail_early; }
    preproc_free(&pre);
    if (doc.nsent == 0) { errout(L"There is no text to speak in %ls.\n", src_path); doc_free(&doc); goto fail_early; }

    PronDict dict;
    pron_init(&dict);
    {
        unsigned char *buf = NULL;
        size_t len = 0;
        int missing = 0, bad = 0;
        if (util_read_file(dict_path, &buf, &len, &missing, err, TTS_ERR_MAX)) {
            pron_from_utf8(&dict, (const char *)buf, len, &bad);
            free(buf);
            if (bad) errout(L"Note: %d line(s) of the pronunciation dictionary were skipped.\n", bad);
        } else if (!missing || dict_arg) {
            errout(L"%ls\n", err);
            if (dict_arg) { pron_free(&dict); doc_free(&doc); goto fail_early; }
        }
    }
    speech.dict = &dict;

    ChapterList chs;
    memset(&chs, 0, sizeof chs);
    if (!chapters_detect(&chs, &doc, &ov)) memset(&chs, 0, sizeof chs);
    pron_resolve_scopes(&dict, &doc, &chs);              /* chapter / selection scopes follow the text */
    pron_prepare(&dict);

    IR ir;
    if (!ir_build(&ir, &doc, &speech, 0, doc.nsent - 1, &synth, &chs)) { errout(L"Out of memory.\n"); goto fail_doc; }

    plat_mkdir_p(cache_root);
    Cache cache;
    if (!cache_open(&cache, cache_root, err, TTS_ERR_MAX)) { errout(L"%ls\n", err); ir_free(&ir); goto fail_doc; }

    /* ---- analysis (also printed before a build) ---- */
    size_t words = 0, chars = 0;
    for (size_t k = 0; k < doc.len; k++) {
        wchar_t c = doc.text[k];
        int in = (c > 32);
        int prev = k > 0 && doc.text[k - 1] > 32;
        if (in && !prev) words++;
        if (in) chars++;
    }
    int cached = 0;
    uint64_t cached_samples = 0;
    size_t missing_chars = 0;
    for (int i = 0; i < ir.n; i++) {
        CacheEntry e;
        if (cache_lookup(&cache, &ir.seg[i].key, &e)) { cached++; cached_samples += e.samples; }
        else missing_chars += wcslen(ir.seg[i].text);
    }
    double est = (double)cached_samples / TTS_WAV_RATE + (double)missing_chars / (15.0 * (1.0 + rate * 0.07 > 0.3 ? 1.0 + rate * 0.07 : 0.3));
    double ps = config_pause_scale(&gs, rate);
    int npara_ends = 0, nch_ends = 0;
    for (int i = 0; i < ir.n; i++) { npara_ends += ir.seg[i].para_end; nch_ends += ir.seg[i].chapter_end; }
    est += (ir.n * (gs.sentence_ms * ps) + npara_ends * (gs.paragraph_ms * ps) + nch_ends * (gs.chapter_ms * ps)) / 1000.0;
    wchar_t tb[32];
    fmt_time(est, tb, 32);
    out(L"Input:        %ls%ls\n", input, is_project ? L"  (project)" : L"");
    out(L"Words:        %llu\nParagraphs:   %d\nSentences:    %d\nChapters:     %d\n", (unsigned long long)words, doc.npara, doc.nsent, chs.n);
    out(L"Segments:     %d  (%d cached, %d to synthesize)\n", ir.n, cached, ir.n - cached);
    out(L"Estimated length: %ls  (about %.0f MB as WAV)\n", tb, est * TTS_WAV_RATE * 2.0 / 1048576.0);
    if (action == A_ANALYZE) {
        for (int i = 0; i < chs.n; i++) out(L"  %3d  %ls\n", i + 1, chs.c[i].title);
        Analysis an;
        if (analyze_document(&an, &doc, &chs, &dict, &speech.opts)) {
            int unhandled = 0;
            for (int i = 0; i < an.n; i++) if (!an.issues[i].handled) unhandled++;
            out(L"\nProblem words: %d distinct (%d not yet handled by a speech rule or pronunciation)\n", an.n, unhandled);
            int last = 0;
            for (int i = 0; i < an.n; i++) {
                const Issue *is = &an.issues[i];
                if (is->kind != last) { out(L"\n%ls\n  %ls\n", analysis_kind_name(is->kind), analysis_kind_hint(is->kind)); last = is->kind; }
                out(L"    %-30ls x%-4d sentence %-6d %ls\n", is->token, is->count, is->first_sent + 1, is->handled ? L"handled" : L"");
            }
            analysis_free(&an);
        }
        rc = 0;
        cache_close(&cache);
        ir_free(&ir);
        goto ok_cleanup;
    }

    /* ---- build ---- */
    wchar_t out_path[MAX_PATH], out_dir[MAX_PATH];
    out_path[0] = out_dir[0] = 0;
    int per_chapter = action == A_CHAPTERS;
    if (per_chapter) {
        wcsncpy(out_dir, chdir_, MAX_PATH - 1);
        if (!plat_mkdir_p(out_dir)) { errout(L"Unable to create the folder %ls\n", out_dir); cache_close(&cache); ir_free(&ir); goto fail_doc; }
    } else if (output) {
        wcsncpy(out_path, output, MAX_PATH - 1);
    } else if (is_project) {
        wchar_t name[MAX_PATH], *sl = wcsrchr(base_name, L'\\');
        wcsncpy(name, sl ? sl + 1 : base_name, MAX_PATH - 1);
        name[MAX_PATH - 1] = 0;
        wchar_t *dot = wcsrchr(name, L'.');
        if (dot) *dot = 0;
        _snwprintf(out_path, MAX_PATH, L"%ls\\export", proj_dir);
        plat_mkdir_p(out_path);
        _snwprintf(out_path, MAX_PATH, L"%ls\\export\\%ls.wav", proj_dir, name);
    } else {
        wcsncpy(out_path, input, MAX_PATH - 5);
        wchar_t *dot = wcsrchr(out_path, L'.');
        wchar_t *sl = wcsrchr(out_path, L'\\');
        if (dot && (!sl || dot > sl)) *dot = 0;
        wcscat(out_path, L".wav");
    }
    out_path[MAX_PATH - 1] = out_dir[MAX_PATH - 1] = 0;

    AsmOptions ao;
    memset(&ao, 0, sizeof ao);
    ao.rate = synth.wav_rate;
    ao.sentence_ms = (int)(gs.sentence_ms * ps + 0.5);
    ao.paragraph_ms = (int)(gs.paragraph_ms * ps + 0.5);
    ao.chapter_ms = (int)(gs.chapter_ms * ps + 0.5);
    ao.trim_silence = gs.trim_silence;
    ao.trim_threshold = 300;
    ao.normalize = gs.normalize;

    GenJob *job = gen_start(&cache, &ir, &synth, &ao, per_chapter ? NULL : out_path, per_chapter,
                            per_chapter ? out_dir : NULL, &chs, NULL, 0, err, TTS_ERR_MAX);
    if (!job) { errout(L"%ls\n", err); ir_free(&ir); cache_close(&cache); goto fail_doc; }
    progress(job);
    if (job->state == GEN_OK) {
        fmt_time((double)job->samples / TTS_WAV_RATE, tb, 32);
        out(L"Done: %ld segment(s) synthesized, %ld reused from the cache%ls. Length %ls.\n",
            (long)job->synthesized, (long)job->hits, job->repaired ? L" (some damaged cache files were regenerated)" : L"", tb);
        if (per_chapter) out(L"Wrote %ld file(s) to %ls\n", (long)job->files, out_dir);
        else out(L"Wrote %ls\n", out_path);
        rc = 0;
    } else {
        errout(L"Failed: %ls\n", job->err[0] ? job->err : L"unknown error");
        rc = 1;
    }
    gen_free(job);
    cache_close(&cache);

ok_cleanup:
fail_doc:
    chapters_free(&chs);
    pron_free(&dict);
    doc_free(&doc);
fail_early:
    overrides_free(&ov);
    ini_free(&ini);
    tts_voices_free(&voices);
    CoUninitialize();
    return rc;
}
