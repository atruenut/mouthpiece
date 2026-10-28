/*
 * tts_remote.h - out-of-process voices.
 *
 * A 64-bit program cannot load a 32-bit SAPI voice (and vice versa). So when this program is
 * 64-bit it starts a small 32-bit helper, MouthpieceVoiceHost32.exe, that sits next to it and
 * runs the very same SAPI engine code (tts_sapi.c) for 32-bit voices. The two talk over the
 * helper's stdin/stdout with one ASCII line per request:
 *
 *   V                      list voices            -> OK <n>, then n lines "<id-hex> <name-hex>"
 *   S <id-hex>             select a voice         -> OK | ERR <msg-hex>
 *   A <rate> <volume>      speech settings        -> OK | ERR
 *   Q <text-hex>           enqueue an utterance   -> OK <stream-id> | ERR
 *   T                      queue status           -> OK <current-stream> <done 0/1> | ERR
 *   P / R / X              pause / resume / stop  -> OK
 *   W <path-hex> <text-hex> render text to WAV    -> OK | ERR      (cancel = kill the helper)
 *   Q (quit)               "E"                    -> helper exits
 *
 * Text is UTF-16 as hex so any character survives. All of it is only compiled into 64-bit builds
 * of the main program; the helper itself is always 32-bit.
 */
#ifndef TTS_REMOTE_H
#define TTS_REMOTE_H
#include <windows.h>
#include "tts_engine.h"

typedef struct RemoteHost RemoteHost;

/* Path of the helper next to the running exe (empty string if it is not there). */
int  remote_host_path(wchar_t *out, size_t cap);
RemoteHost *remote_open(wchar_t *err, size_t errcap);          /* starts the helper */
int  remote_alive(RemoteHost *h);                              /* 0 once it has died or been stopped */
void remote_close(RemoteHost *h);                              /* NULL-safe */
/* Voice list of the helper (heap array, free with tts_voices_free). 1 ok, 0 failed. */
int  remote_list_voices(RemoteHost *h, TTSVoiceList *out, wchar_t *err, size_t errcap);
/* Generic request: `line` without newline. On success the reply payload (after "OK ") is copied to
 * `reply`. Returns 1 for OK, 0 for ERR/transport failure (message in err). */
int  remote_call(RemoteHost *h, const char *line, char *reply, size_t replycap, wchar_t *err, size_t errcap);
/* Render: waits while polling `cancel`; on cancel the helper is killed. Returns 1 ok, 0 fail, -1 cancelled. */
int  remote_render(RemoteHost *h, const char *line, volatile LONG *cancel, wchar_t *err, size_t errcap);
#endif
