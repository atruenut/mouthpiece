/*
 * tts_sapi.c - SAPI 5 backend for tts_engine.h (plain C, COM via lpVtbl).
 *
 * Design notes
 *  - Voices are identified by their SAPI object-token id (a registry path),
 *    which is stable and unique; the display name is for the UI only.
 *  - Text is always sent with SPF_IS_NOT_XML so that '<' and '&' in messy
 *    source text are never interpreted as SAPI markup.
 *  - Offline rendering writes 16-bit mono PCM at TTS_WAV_RATE Hz. SAPI inserts
 *    its own format converter if a voice's native rate differs (SetOutput with
 *    fAllowFormatChanges = FALSE pins the file format). Most Microsoft desktop
 *    voices are 22.05 kHz, so no resampling is normally needed. Querying a
 *    voice's true native format is deferred (see ARCHITECTURE.md, risks).
 */
#include "tts_engine.h"
#include "util.h"
#include <sapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN64
#include "tts_remote.h"
#include "hexcodec.h"
#endif


/* SPDFID_WaveFormatEx is declared but not exported by MinGW's libsapi, so we
 * carry our own copy of the (fixed, documented) GUID. */
static const GUID kWaveFormatExId =
    { 0xC31ADBAE, 0x527F, 0x4FF5, { 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C } };

typedef struct { ULONG id; wchar_t *text; } Pending;   /* queued utterance + its owned text copy */

struct TTSEngine {
    ISpVoice *voice;
    Pending  *pend;
    int       npend, cap_pend;
    int       word_events;      /* the voice was asked for word-boundary events, so GetStatus reports word positions */
    int       word_ok;
    unsigned long word_pos;
#ifdef _WIN64
    RemoteHost *rh;        /* 32-bit voice helper, started on demand */
    int       remote;      /* the selected voice lives in the helper */
    int       have_set, set_rate, set_vol;
#endif
};

#ifdef _WIN64
/* Run a request on the helper; 1 ok. */
static int rcall(TTSEngine *e, const char *line, char *reply, size_t cap, wchar_t *err, size_t errcap)
{
    return e->rh && remote_call(e->rh, line, reply, cap, err, errcap);
}
#endif

static void pend_release(TTSEngine *e, ULONG below)   /* free copies of finished streams (id < below) */
{
    int w = 0;
    for (int r = 0; r < e->npend; r++) {
        if (e->pend[r].id < below) free(e->pend[r].text);
        else e->pend[w++] = e->pend[r];
    }
    e->npend = w;
}

#define SET_ERR(...) do { if (err && errcap) { _snwprintf(err, errcap, __VA_ARGS__); err[errcap - 1] = 0; } } while (0)
#define SET_HR(ctx, hr) do { if (err && errcap) util_format_hresult(err, errcap, (ctx), (hr)); } while (0)

/* ------------------------------------------------------------------ voices */

/*
 * Voices come from several token categories, because Windows keeps them apart:
 *   - the classic SAPI 5 category (what SAPI itself lists: Microsoft David/Zira, 3rd-party voices)
 *   - the "OneCore" category (the newer Windows 10/11 voices and language-pack voices),
 *     which the classic category does not list but which SAPI can usually still speak with
 *   - the same two under HKEY_CURRENT_USER (per-user installs)
 * A 64-bit process only sees 64-bit voices and a 32-bit process only 32-bit voices;
 * the 32-bit build exists for that reason.
 */
static const struct { const wchar_t *cat; const wchar_t *tag; } kVoiceCats[] = {
    { L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices",          L"" },
    { L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\Voices",           L"" },
    { L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices",  L" [OneCore]" },
    { L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices",   L" [OneCore]" },
};

static int voice_known(const TTSVoiceList *l, const wchar_t *id)
{
    for (int i = 0; i < l->count; i++)
        if (_wcsicmp(l->items[i].id, id) == 0) return 1;
    return 0;
}

static void enum_category(TTSVoiceList *out, int *cap, const wchar_t *catid, const wchar_t *tag)
{
    ISpObjectTokenCategory *cat = NULL;
    IEnumSpObjectTokens *en = NULL;
    ULONG n = 0;
    if (FAILED(CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL,
                                &IID_ISpObjectTokenCategory, (void **)&cat))) return;
    if (FAILED(cat->lpVtbl->SetId(cat, catid, FALSE))) goto done;
    if (FAILED(cat->lpVtbl->EnumTokens(cat, NULL, NULL, &en))) goto done;
    if (FAILED(en->lpVtbl->GetCount(en, &n))) goto done;
    for (ULONG i = 0; i < n; i++) {
        ISpObjectToken *tok = NULL;
        if (FAILED(en->lpVtbl->Item(en, i, &tok)) || !tok) continue;
        LPWSTR id = NULL, name = NULL;
        if (SUCCEEDED(tok->lpVtbl->GetId(tok, &id)) && id && wcslen(id) < TTS_ID_MAX && !voice_known(out, id)) {
            if (out->count == *cap) {
                int nc = *cap ? *cap * 2 : 32;
                TTSVoice *p = (TTSVoice *)realloc(out->items, (size_t)nc * sizeof(TTSVoice));
                if (!p) { CoTaskMemFree(id); tok->lpVtbl->Release(tok); goto done; }
                out->items = p;
                *cap = nc;
            }
            TTSVoice *v = &out->items[out->count];
            memset(v, 0, sizeof *v);
            wcscpy(v->id, id);
            wchar_t base[TTS_NAME_MAX];
            if (SUCCEEDED(tok->lpVtbl->GetStringValue(tok, NULL, &name)) && name) {
                wcsncpy(base, name, TTS_NAME_MAX - 1);
            } else {
                const wchar_t *tail = wcsrchr(id, L'\\');
                wcsncpy(base, tail ? tail + 1 : id, TTS_NAME_MAX - 1);
            }
            base[TTS_NAME_MAX - 1] = 0;
            _snwprintf(v->name, TTS_NAME_MAX, L"%ls%ls", base, tag);
            v->name[TTS_NAME_MAX - 1] = 0;
            out->count++;
        }
        if (id) CoTaskMemFree(id);
        if (name) CoTaskMemFree(name);
        tok->lpVtbl->Release(tok);
    }
done:
    if (en) en->lpVtbl->Release(en);
    if (cat) cat->lpVtbl->Release(cat);
}

TTSStatus tts_voices_enumerate(TTSVoiceList *out, wchar_t *err, size_t errcap)
{
    int cap = 0;
    out->items = NULL;
    out->count = 0;
    for (size_t k = 0; k < sizeof kVoiceCats / sizeof kVoiceCats[0]; k++)
        enum_category(out, &cap, kVoiceCats[k].cat, kVoiceCats[k].tag);
    if (out->count == 0) {
        /* last resort: whatever SAPI itself considers the voice category */
        enum_category(out, &cap, SPCAT_VOICES, L"");
    }
#ifdef _WIN64
    {   /* 32-bit voices cannot be loaded into this 64-bit process; the helper lists and speaks them */
        wchar_t herr[TTS_ERR_MAX];
        RemoteHost *h = remote_open(herr, TTS_ERR_MAX);
        if (h) {
            TTSVoiceList rl;
            if (remote_list_voices(h, &rl, herr, TTS_ERR_MAX)) {
                for (int i = 0; i < rl.count; i++) {
                    if (voice_known(out, rl.items[i].id)) continue;
                    if (out->count == cap) {
                        int nc = cap ? cap * 2 : 32;
                        TTSVoice *p = (TTSVoice *)realloc(out->items, (size_t)nc * sizeof(TTSVoice));
                        if (!p) break;
                        out->items = p;
                        cap = nc;
                    }
                    TTSVoice *v = &out->items[out->count++];
                    memset(v, 0, sizeof *v);
                    wcscpy(v->id, rl.items[i].id);
                    _snwprintf(v->name, TTS_NAME_MAX, L"%ls (32-bit)", rl.items[i].name);
                    v->name[TTS_NAME_MAX - 1] = 0;
                }
                tts_voices_free(&rl);
            }
            remote_close(h);
        }
    }
#endif
    if (out->count == 0) { SET_ERR(L"No SAPI voices were found."); tts_voices_free(out); return TTS_FAIL; }
    return TTS_SUCCESS;
}

void tts_voices_free(TTSVoiceList *list)
{
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

/* ------------------------------------------------------------------ engine */

TTSEngine *tts_engine_create(wchar_t *err, size_t errcap)
{
    TTSEngine *e = (TTSEngine *)calloc(1, sizeof *e);
    if (!e) { SET_ERR(L"Out of memory."); return NULL; }
    HRESULT hr = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL,
                                  &IID_ISpVoice, (void **)&e->voice);
    if (FAILED(hr)) {
        SET_HR(L"Unable to create the SAPI speech engine", hr);
        free(e);
        return NULL;
    }
    /* word-boundary events make SAPI keep track of (and report) the position of the word being spoken; the captions use it */
    e->word_events = SUCCEEDED(e->voice->lpVtbl->SetInterest(e->voice, SPFEI(SPEI_WORD_BOUNDARY), SPFEI(SPEI_WORD_BOUNDARY)));
    return e;
}

void tts_engine_destroy(TTSEngine *e)
{
    if (!e) return;
    tts_stop(e);
#ifdef _WIN64
    remote_close(e->rh);
#endif
    if (e->voice) e->voice->lpVtbl->Release(e->voice);
    free(e->pend);
    free(e);
}

static TTSStatus select_local(TTSEngine *e, const wchar_t *voice_id, wchar_t *err, size_t errcap)
{
    if (!voice_id || !*voice_id) return TTS_SUCCESS;

    ISpObjectToken *tok = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_SpObjectToken, NULL, CLSCTX_ALL,
                                  &IID_ISpObjectToken, (void **)&tok);
    if (FAILED(hr)) { SET_HR(L"Unable to create voice token", hr); return TTS_FAIL; }

    hr = tok->lpVtbl->SetId(tok, NULL, voice_id, FALSE);
    if (FAILED(hr)) {
        const wchar_t *tail = wcsrchr(voice_id, L'\\');
        SET_ERR(L"The voice \"%ls\" is no longer installed or cannot be opened (0x%08lX).",
                tail ? tail + 1 : voice_id, (unsigned long)hr);
        tok->lpVtbl->Release(tok);
        return TTS_FAIL;
    }
    hr = e->voice->lpVtbl->SetVoice(e->voice, tok);
    tok->lpVtbl->Release(tok);
    if (FAILED(hr)) { SET_HR(L"Unable to select voice", hr); return TTS_FAIL; }
    return TTS_SUCCESS;
}

TTSStatus tts_select_voice(TTSEngine *e, const wchar_t *voice_id, wchar_t *err, size_t errcap)
{
    TTSStatus r = select_local(e, voice_id, err, errcap);
#ifdef _WIN64
    if (r == TTS_SUCCESS) { e->remote = 0; return r; }
    /* Not loadable here: maybe a 32-bit voice. Ask the helper. */
    wchar_t rerr[TTS_ERR_MAX];
    if (e->rh && !remote_alive(e->rh)) { remote_close(e->rh); e->rh = NULL; }
    if (!e->rh) e->rh = remote_open(rerr, TTS_ERR_MAX);
    if (e->rh) {
        char *hx = hex_encode(voice_id);
        char *line = hx ? (char *)malloc(strlen(hx) + 4) : NULL;
        if (line) {
            sprintf(line, "S %s", hx);
            if (rcall(e, line, NULL, 0, rerr, TTS_ERR_MAX)) {
                e->remote = 1;
                free(line); free(hx);
                if (e->have_set) {
                    char a[64];
                    sprintf(a, "A %d %d", e->set_rate, e->set_vol);
                    rcall(e, a, NULL, 0, rerr, TTS_ERR_MAX);
                }
                return TTS_SUCCESS;
            }
        }
        free(line); free(hx);
    }
#endif
    return r;
}

TTSStatus tts_apply_settings(TTSEngine *e, const TTSSpeechSettings *s, wchar_t *err, size_t errcap)
{
    int rate = s->rate < -10 ? -10 : (s->rate > 10 ? 10 : s->rate);
    int vol  = s->volume < 0 ? 0 : (s->volume > 100 ? 100 : s->volume);
#ifdef _WIN64
    e->have_set = 1; e->set_rate = rate; e->set_vol = vol;
    if (e->remote) {
        char a[64];
        sprintf(a, "A %d %d", rate, vol);
        if (!rcall(e, a, NULL, 0, err, errcap)) return TTS_FAIL;
        return TTS_SUCCESS;
    }
#endif
    HRESULT hr = e->voice->lpVtbl->SetRate(e->voice, rate);
    if (FAILED(hr)) { SET_HR(L"Unable to set speech rate", hr); return TTS_FAIL; }
    hr = e->voice->lpVtbl->SetVolume(e->voice, (USHORT)vol);
    if (FAILED(hr)) { SET_HR(L"Unable to set volume", hr); return TTS_FAIL; }
    return TTS_SUCCESS;
}

/* ---------------------------------------------------------------- playback */

TTSStatus tts_enqueue(TTSEngine *e, const wchar_t *text, unsigned long *stream_id,
                      wchar_t *err, size_t errcap)
{
#ifdef _WIN64
    if (e->remote) {
        if (wcslen(text) > 20000) { SET_ERR(L"This sentence is too long to send to the 32-bit voice helper."); return TTS_FAIL; }
        char *hx = hex_encode(text);
        char *line = hx ? (char *)malloc(strlen(hx) + 4) : NULL;
        char rep[32];
        int ok = 0;
        if (line) { sprintf(line, "Q %s", hx); ok = rcall(e, line, rep, sizeof rep, err, errcap); }
        free(line); free(hx);
        if (!ok) return TTS_FAIL;
        if (stream_id) *stream_id = strtoul(rep, NULL, 10);
        return TTS_SUCCESS;
    }
#endif
    if (e->npend == e->cap_pend) {
        int nc = e->cap_pend ? e->cap_pend * 2 : 16;
        Pending *p = (Pending *)realloc(e->pend, (size_t)nc * sizeof *p);
        if (!p) { SET_ERR(L"Out of memory."); return TTS_FAIL; }
        e->pend = p; e->cap_pend = nc;
    }
    wchar_t *copy = _wcsdup(text);
    if (!copy) { SET_ERR(L"Out of memory."); return TTS_FAIL; }

    ULONG sn = 0;
    HRESULT hr = e->voice->lpVtbl->Speak(e->voice, copy, SPF_ASYNC | SPF_IS_NOT_XML, &sn);
    if (FAILED(hr)) { SET_HR(L"Speech failed to start", hr); free(copy); return TTS_FAIL; }

    e->pend[e->npend].id = sn;
    e->pend[e->npend].text = copy;
    e->npend++;
    if (stream_id) *stream_id = sn;
    return TTS_SUCCESS;
}

TTSStatus tts_queue_status(TTSEngine *e, unsigned long *current_stream, int *done,
                           wchar_t *err, size_t errcap)
{
#ifdef _WIN64
    if (e->remote) {
        char rep[48];
        if (!rcall(e, "T", rep, sizeof rep, err, errcap)) return TTS_FAIL;
        unsigned long c = 0, wp = 0;
        int d = 0;
        int got = sscanf(rep, "%lu %d %lu", &c, &d, &wp);
        *current_stream = c;
        *done = d;
        e->word_ok = got >= 3;
        e->word_pos = wp;
        return TTS_SUCCESS;
    }
#endif
    SPVOICESTATUS st;
    memset(&st, 0, sizeof st);
    HRESULT hr = e->voice->lpVtbl->GetStatus(e->voice, &st, NULL);
    if (FAILED(hr)) { SET_HR(L"Unable to query speech status", hr); return TTS_FAIL; }
    if (FAILED(st.hrLastResult)) { SET_HR(L"Speech synthesis failed", st.hrLastResult); return TTS_FAIL; }

    *current_stream = st.ulCurrentStream;
    e->word_ok = e->word_events;
    e->word_pos = st.ulInputWordPos;
    if (e->word_events) {          /* nobody reads the events themselves: empty the queue so it never grows over a long book */
        SPEVENT ev[16];
        ULONG got = 0;
        do { got = 0; if (FAILED(e->voice->lpVtbl->GetEvents(e->voice, 16, ev, &got))) break; } while (got == 16);
    }
    *done = e->voice->lpVtbl->WaitUntilDone(e->voice, 0) == S_OK;
    if (*done) pend_release(e, (ULONG)-1);
    else if (st.ulCurrentStream) pend_release(e, st.ulCurrentStream);
    return TTS_SUCCESS;
}

int tts_word_position(TTSEngine *e, unsigned long *pos)
{
    if (!e || !e->word_ok) return 0;
    *pos = e->word_pos;
    return 1;
}

void tts_pause(TTSEngine *e)
{
#ifdef _WIN64
    if (e && e->remote) { wchar_t m[64]; rcall(e, "P", NULL, 0, m, 64); return; }
#endif
    if (e && e->voice) e->voice->lpVtbl->Pause(e->voice);
}
void tts_resume(TTSEngine *e)
{
#ifdef _WIN64
    if (e && e->remote) { wchar_t m[64]; rcall(e, "R", NULL, 0, m, 64); return; }
#endif
    if (e && e->voice) e->voice->lpVtbl->Resume(e->voice);
}

void tts_stop(TTSEngine *e)
{
    if (!e || !e->voice) return;
#ifdef _WIN64
    if (e->remote) { wchar_t m[64]; rcall(e, "X", NULL, 0, m, 64); return; }
#endif
    /* A paused voice must be resumed before a purge is honoured. */
    e->voice->lpVtbl->Resume(e->voice);
    e->voice->lpVtbl->Speak(e->voice, NULL, SPF_PURGEBEFORESPEAK, NULL);
    pend_release(e, (ULONG)-1);
}

/* --------------------------------------------------------------- rendering */

TTSStatus tts_render_wav(TTSEngine *e, const wchar_t *text, const wchar_t *path,
                         volatile LONG *cancel, wchar_t *err, size_t errcap)
{
#ifdef _WIN64
    if (e->remote) {
        if (wcslen(text) > 20000) { SET_ERR(L"This text is too long to send to the 32-bit voice helper."); return TTS_FAIL; }
        char *hp = hex_encode(path), *ht = hex_encode(text);
        char *line = (hp && ht) ? (char *)malloc(strlen(hp) + strlen(ht) + 4) : NULL;
        int r = 0;
        if (line) { sprintf(line, "W %s %s", hp, ht); r = remote_render(e->rh, line, cancel, err, errcap); }
        free(line); free(hp); free(ht);
        if (r < 0) { DeleteFileW(path); return TTS_CANCELLED; }
        return r ? TTS_SUCCESS : TTS_FAIL;
    }
#endif
    ISpStream *stream = NULL;
    WAVEFORMATEX wfx;
    TTSStatus rc = TTS_FAIL;
    int bound = 0;

    HRESULT hr = CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL,
                                  &IID_ISpStream, (void **)&stream);
    if (FAILED(hr)) { SET_HR(L"Unable to create audio file stream", hr); return TTS_FAIL; }

    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = TTS_WAV_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    hr = stream->lpVtbl->BindToFile(stream, path, SPFM_CREATE_ALWAYS, &kWaveFormatExId, &wfx, 0);
    if (FAILED(hr)) {
        wchar_t why[256];
        util_format_hresult(why, 256, L"", hr);
        SET_ERR(L"Unable to write output file:\n%ls\n\nReason:\n%ls", path, why);
        goto done;
    }
    bound = 1;

    hr = e->voice->lpVtbl->SetOutput(e->voice, (IUnknown *)stream, FALSE);
    if (FAILED(hr)) { SET_HR(L"Unable to direct speech to the file", hr); goto done; }

    hr = e->voice->lpVtbl->Speak(e->voice, text, SPF_ASYNC | SPF_IS_NOT_XML, NULL);
    if (FAILED(hr)) { SET_HR(L"Speech synthesis failed to start", hr); goto done; }

    for (;;) {
        hr = e->voice->lpVtbl->WaitUntilDone(e->voice, 100);
        if (hr == S_OK) break;
        if (cancel && *cancel) {
            e->voice->lpVtbl->Speak(e->voice, NULL, SPF_PURGEBEFORESPEAK, NULL);
            rc = TTS_CANCELLED;
            goto done;
        }
    }
    {
        SPVOICESTATUS st;
        memset(&st, 0, sizeof st);
        if (SUCCEEDED(e->voice->lpVtbl->GetStatus(e->voice, &st, NULL)) && FAILED(st.hrLastResult)) {
            SET_HR(L"Speech synthesis failed", st.hrLastResult);
            goto done;
        }
    }
    rc = TTS_SUCCESS;

done:
    /* Detach the file from the voice and close it so the RIFF header is finalised. */
    e->voice->lpVtbl->SetOutput(e->voice, NULL, TRUE);
    if (bound) stream->lpVtbl->Close(stream);
    stream->lpVtbl->Release(stream);
    if (rc != TTS_SUCCESS && bound) DeleteFileW(path);
    return rc;
}
