#include "gen.h"
#include "plat.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

typedef struct RangeCtx { GenJob *j; int start; int base, grand; } RangeCtx;

static FILE *open_seg(void *ctx, int i)
{
    RangeCtx *r = (RangeCtx *)ctx;
    wchar_t p[700];
    cache_segment_path(r->j->cache, &r->j->ir.seg[r->start + i].key, p, 700);
    return plat_fopen(p, "rb");
}

static int asm_progress(void *ctx, int done, int total)
{
    RangeCtx *r = (RangeCtx *)ctx;
    (void)total;
    r->j->phase = 1;
    r->j->done = r->base + done;
    r->j->total = r->grand;
    return r->j->cancel != 0;
}

/* Make sure segment i is in the cache. 1 ok, 0 failed (err set), -1 cancelled. */
static int ensure_segment(GenJob *j, TTSEngine **engine, int i)
{
    IRSegment *s = &j->ir.seg[i];
    if (cache_lookup(j->cache, &s->key, NULL)) return 1;
    wchar_t e[TTS_ERR_MAX], tmp[700];
    if (!*engine) {
        TTSSpeechSettings st;
        st.rate = j->synth.rate; st.volume = j->synth.volume;
        *engine = tts_engine_create(e, TTS_ERR_MAX);
        if (!*engine || !tts_select_voice(*engine, j->synth.voice_id, e, TTS_ERR_MAX) ||
            !tts_apply_settings(*engine, &st, e, TTS_ERR_MAX)) {
            _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"%ls", e);
            return 0;
        }
    }
    cache_tmp_path(j->cache, &s->key, tmp, 700);
    plat_remove(tmp);
    TTSStatus st = tts_render_wav(*engine, s->text, tmp, &j->cancel, e, TTS_ERR_MAX);
    if (st == TTS_CANCELLED) return -1;
    if (st != TTS_SUCCESS) {
        _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"Segment %d of %d (sentence %d) failed:\n\n%ls",
                   i + 1, j->ir.n, s->first_sent + 1, e);
        return 0;
    }
    if (!cache_commit(j->cache, &s->key, tmp, j->synth.wav_rate, e, TTS_ERR_MAX)) {
        _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"%ls", e);
        return 0;
    }
    InterlockedIncrement(&j->synthesized);
    return 1;
}

/* Assemble segments [start,end) into `path` (via path.part). Repairs corrupt cache entries
 * and retries. 1 ok, 0 failed (err set), -1 cancelled. *samples receives the audio length. */
static int build_file(GenJob *j, TTSEngine **engine, int start, int end, const wchar_t *path,
                      const unsigned char *kinds, int base, int grand, uint64_t *samples)
{
    int cnt = end - start;
    uint32_t *crc = (uint32_t *)malloc((size_t)cnt * sizeof *crc);
    wchar_t part[MAX_PATH + 8];
    int result = 0;
    if (!crc) { _snwprintf(j->err, TTS_ERR_MAX, L"Out of memory."); return 0; }
    _snwprintf(part, MAX_PATH + 8, L"%ls.part", path);
    part[MAX_PATH + 7] = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        for (int i = 0; i < cnt; i++) {
            CacheEntry e;
            if (!cache_lookup(j->cache, &j->ir.seg[start + i].key, &e)) {
                int r = ensure_segment(j, engine, start + i);
                if (r <= 0) { result = r; goto done; }
                cache_lookup(j->cache, &j->ir.seg[start + i].key, &e);
            }
            crc[i] = e.crc;
        }
        FILE *out = plat_fopen(part, "w+b");
        if (!out) {
            wchar_t why[256];
            util_format_win32(why, 256, L"", GetLastError());
            _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"Unable to write output file:\n%ls\n\nReason:\n%ls", path, why);
            goto done;
        }
        RangeCtx rc = { j, start, base, grand };
        AsmResult ar;
        AsmStatus st = assemble_wav(out, cnt, kinds, crc, open_seg, &rc, asm_progress, &rc, &j->asmo, &ar);
        int closed_ok = fclose(out) == 0;
        if (st == ASM_OK && closed_ok) {
            if (!plat_rename_replace(part, path)) {
                wchar_t why[256];
                util_format_win32(why, 256, L"", GetLastError());
                _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"Unable to write output file:\n%ls\n\nReason:\n%ls", path, why);
                plat_remove(part);
                goto done;
            }
            *samples = ar.samples;
            result = 1;
            goto done;
        }
        plat_remove(part);
        if (st == ASM_CANCELLED) { result = -1; goto done; }
        if (st == ASM_BAD_SEGMENT) {                     /* "Cached audio segment is invalid. Regenerating..." */
            cache_remove(j->cache, &j->ir.seg[start + ar.bad_segment].key);
            InterlockedIncrement(&j->repaired);
            j->phase = 0;
            continue;
        }
        if (st == ASM_TOO_BIG) _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"The result would be larger than a WAV file can hold (4 GB). Export one WAV per chapter or a smaller range.");
        else _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"Unable to write output file:\n%ls", path);
        goto done;
    }
    _snwprintf(j->err, TTS_ERR_MAX + MAX_PATH, L"Cached audio kept failing verification. Clear the audio cache and try again.");
done:
    free(crc);
    return result;
}

static DWORD WINAPI gen_thread(LPVOID arg)
{
    GenJob *j = (GenJob *)arg;
    TTSEngine *engine = NULL;
    GenState result = GEN_FAILED;
    int n = j->ir.n > 0 ? j->ir.n : 0;
    size_t cnt = n > 0 ? (size_t)n : 1;
    unsigned char *kinds = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { util_format_hresult(j->err, TTS_ERR_MAX, L"COM initialisation failed", hr); goto finish; }

    /* ---- phase 0: synthesize what the cache does not have ---- */
    j->phase = 0; j->total = n; j->done = 0;
    for (int i = 0; i < n; i++) {
        if (j->cancel) { result = GEN_CANCELLED; goto cleanup; }
        if (cache_lookup(j->cache, &j->ir.seg[i].key, NULL)) InterlockedIncrement(&j->hits);
        else {
            int r = ensure_segment(j, &engine, i);
            if (r < 0) { result = GEN_CANCELLED; goto cleanup; }
            if (r == 0) goto cleanup;
        }
        j->done = i + 1;
    }

    /* ---- phase 1: assemble one file, or one file per chapter ---- */
    kinds = (unsigned char *)malloc(cnt);
    if (!kinds) { _snwprintf(j->err, TTS_ERR_MAX, L"Out of memory."); goto cleanup; }
    ir_pause_kinds(&j->ir, kinds);
    int grand = n * (j->asmo.normalize ? 2 : 1), base = 0, ordinal = 0;
    j->phase = 1; j->done = 0; j->total = grand;
    uint64_t total_samples = 0;
    for (int gs = 0; gs < n;) {
        int ge = gs + 1;
        if (j->per_chapter) while (ge < n && j->ir.seg[ge].chapter == j->ir.seg[gs].chapter) ge++;
        else ge = n;
        wchar_t path[MAX_PATH];
        if (j->per_chapter) {
            int c = j->ir.seg[gs].chapter;
            wchar_t fn[200];
            chapter_filename(fn, 200, ++ordinal, (c >= 0 && c < j->ntitles) ? j->titles[c] : L"Beginning");
            _snwprintf(path, MAX_PATH, L"%ls\\%ls", j->out_dir, fn);
            path[MAX_PATH - 1] = 0;
        } else {
            wcscpy(path, j->out_path);
        }
        uint64_t samples = 0;
        int r = build_file(j, &engine, gs, ge, path, kinds + gs, base, grand, &samples);
        if (r < 0) { result = GEN_CANCELLED; goto cleanup; }
        if (r == 0) goto cleanup;
        total_samples += samples;
        InterlockedIncrement(&j->files);
        base += (ge - gs) * (j->asmo.normalize ? 2 : 1);
        gs = ge;
    }
    j->samples = total_samples;
    result = GEN_OK;

cleanup:
    tts_engine_destroy(engine);
    free(kinds);
    CoUninitialize();
finish:
    j->state = result;
    PostMessageW(j->notify, j->msg_done, 0, 0);
    return 0;
}

GenJob *gen_start(Cache *cache, IR *ir, const SynthParams *synth, const AsmOptions *asmo,
                  const wchar_t *out_path, int per_chapter, const wchar_t *out_dir, const ChapterList *chs,
                  HWND notify, UINT msg_done, wchar_t *err, size_t errcap)
{
    GenJob *j = (GenJob *)calloc(1, sizeof *j);
    if (!j) { _snwprintf(err, errcap, L"Out of memory."); return NULL; }
    j->cache = cache;
    j->ir = *ir;
    memset(ir, 0, sizeof *ir);
    j->synth = *synth;
    j->asmo = *asmo;
    if (out_path) wcsncpy(j->out_path, out_path, MAX_PATH - 1);
    j->per_chapter = per_chapter;
    if (out_dir) wcsncpy(j->out_dir, out_dir, MAX_PATH - 1);
    if (chs && chs->n > 0) {
        j->titles = (wchar_t **)calloc((size_t)chs->n, sizeof(wchar_t *));
        if (j->titles) {
            j->ntitles = chs->n;
            for (int i = 0; i < chs->n; i++) {
                size_t l = wcslen(chs->c[i].title);
                j->titles[i] = (wchar_t *)malloc((l + 1) * sizeof(wchar_t));
                if (j->titles[i]) memcpy(j->titles[i], chs->c[i].title, (l + 1) * sizeof(wchar_t));
            }
        }
    }
    j->notify = notify;
    j->msg_done = msg_done;
    j->state = GEN_RUNNING;
    j->thread = CreateThread(NULL, 0, gen_thread, j, 0, NULL);
    if (!j->thread) {
        util_format_win32(err, errcap, L"Unable to start the generation thread", GetLastError());
        ir_free(&j->ir);
        free(j);
        return NULL;
    }
    return j;
}

void gen_cancel(GenJob *j) { if (j) InterlockedExchange(&j->cancel, 1); }

void gen_free(GenJob *j)
{
    if (!j) return;
    if (j->thread) { WaitForSingleObject(j->thread, INFINITE); CloseHandle(j->thread); }
    ir_free(&j->ir);
    for (int i = 0; i < j->ntitles; i++) free(j->titles[i]);
    free(j->titles);
    free(j);
}
