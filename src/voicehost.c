/*
 * voicehost.c - MouthpieceVoiceHost32.exe: a 32-bit process that runs the normal SAPI engine on behalf
 * of a 64-bit Mouthpiece (see tts_remote.h for the line protocol). Built 32-bit only.
 */
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tts_engine.h"
#include "hexcodec.h"

static void reply(const char *s)
{
    DWORD n;
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(o, s, (DWORD)strlen(s), &n, NULL);
    WriteFile(o, "\n", 1, &n, NULL);
}

static void reply_err(const wchar_t *msg)
{
    char *h = hex_encode(msg);
    size_t n = h ? strlen(h) : 0;
    char *b = (char *)malloc(n + 8);
    if (b) { sprintf(b, "ERR %s", h ? h : ""); reply(b); free(b); }
    free(h);
}

static char *read_line(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    size_t cap = 4096, len = 0;
    char *b = (char *)malloc(cap);
    for (;;) {
        char c;
        DWORD n;
        if (!b || !ReadFile(in, &c, 1, &n, NULL) || n == 0) { free(b); return NULL; }
        if (c == '\n') break;
        if (len + 2 > cap) { cap *= 2; char *nb = (char *)realloc(b, cap); if (!nb) { free(b); return NULL; } b = nb; }
        if (c != '\r') b[len++] = c;
    }
    b[len] = 0;
    return b;
}

int main(void)
{
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 2;
    wchar_t err[TTS_ERR_MAX];
    TTSEngine *e = tts_engine_create(err, TTS_ERR_MAX);
    char *line;
    while ((line = read_line()) != NULL) {
        char cmd = line[0];
        const char *arg = line[0] && line[1] == ' ' ? line + 2 : "";
        char out[96];
        err[0] = 0;
        if (cmd == 'E') { free(line); break; }
        if (!e && cmd != 'V') { reply_err(L"The speech engine could not be created."); free(line); continue; }
        switch (cmd) {
        case 'V': {
            TTSVoiceList l;
            if (tts_voices_enumerate(&l, err, TTS_ERR_MAX) != TTS_SUCCESS) { sprintf(out, "OK 0"); reply(out); break; }
            sprintf(out, "OK %d", l.count);
            reply(out);
            for (int i = 0; i < l.count; i++) {
                char *a = hex_encode(l.items[i].id), *b = hex_encode(l.items[i].name);
                if (a && b) {
                    char *m = (char *)malloc(strlen(a) + strlen(b) + 2);
                    if (m) { sprintf(m, "%s %s", a, b); reply(m); free(m); }
                }
                free(a); free(b);
            }
            tts_voices_free(&l);
            break;
        }
        case 'S': {
            wchar_t *id = hex_decode(arg, strlen(arg));
            if (id && tts_select_voice(e, id, err, TTS_ERR_MAX) == TTS_SUCCESS) reply("OK"); else reply_err(err[0] ? err : L"Bad voice.");
            free(id);
            break;
        }
        case 'A': {
            TTSSpeechSettings s;
            s.rate = 0; s.volume = 100;
            sscanf(arg, "%d %d", &s.rate, &s.volume);
            if (tts_apply_settings(e, &s, err, TTS_ERR_MAX) == TTS_SUCCESS) reply("OK"); else reply_err(err);
            break;
        }
        case 'Q': {
            wchar_t *t = hex_decode(arg, strlen(arg));
            unsigned long sn = 0;
            if (t && tts_enqueue(e, t, &sn, err, TTS_ERR_MAX) == TTS_SUCCESS) { sprintf(out, "OK %lu", sn); reply(out); }
            else reply_err(err[0] ? err : L"Bad text.");
            free(t);
            break;
        }
        case 'T': {
            unsigned long cur = 0;
            int done = 0;
            if (tts_queue_status(e, &cur, &done, err, TTS_ERR_MAX) == TTS_SUCCESS) {
                unsigned long wp = 0;
                if (tts_word_position(e, &wp)) sprintf(out, "OK %lu %d %lu", cur, done, wp);
                else sprintf(out, "OK %lu %d", cur, done);
                reply(out);
            }
            else reply_err(err);
            break;
        }
        case 'P': tts_pause(e); reply("OK"); break;
        case 'R': tts_resume(e); reply("OK"); break;
        case 'X': tts_stop(e); reply("OK"); break;
        case 'W': {
            const char *sp = strchr(arg, ' ');
            if (!sp) { reply_err(L"Bad request."); break; }
            wchar_t *path = hex_decode(arg, (size_t)(sp - arg)), *t = hex_decode(sp + 1, strlen(sp + 1));
            volatile LONG nocancel = 0;
            if (path && t && tts_render_wav(e, t, path, &nocancel, err, TTS_ERR_MAX) == TTS_SUCCESS) reply("OK");
            else reply_err(err[0] ? err : L"Bad request.");
            free(path); free(t);
            break;
        }
        default: reply_err(L"Unknown request.");
        }
        free(line);
    }
    tts_engine_destroy(e);
    CoUninitialize();
    return 0;
}
