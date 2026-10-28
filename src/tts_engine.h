/*
 * tts_engine.h - speech backend abstraction.
 *
 * The rest of the program talks ONLY to this header. The SAPI-specific code
 * lives in tts_sapi.c; no <sapi.h> type leaks out of it, so a second backend
 * could be added later by providing another implementation of these functions.
 *
 * THREADING / OWNERSHIP RULES
 *  - The calling thread must have initialised COM (CoInitializeEx) before
 *    calling tts_engine_create() and must keep COM initialised until after
 *    tts_engine_destroy().
 *  - A TTSEngine is owned by the thread that created it. Never share one
 *    between threads: create one engine per thread (UI thread = playback,
 *    each worker = its own engine). tts_voices_enumerate() has no engine and
 *    may be called from any COM-initialised thread.
 *  - All functions that can fail return TTS_FAIL and, if err != NULL, write a
 *    human-readable message (already formatted, wide) into err[errcap].
 *  - Strings passed in are borrowed for the duration of the call, EXCEPT that
 *    tts_enqueue() copies the text (the engine owns the copy until that
 *    utterance has finished or tts_stop()).
 */
#ifndef TTS_ENGINE_H
#define TTS_ENGINE_H

#include <windows.h>

#define TTS_ID_MAX   256   /* stable backend identifier (SAPI token id)   */
#define TTS_NAME_MAX 128   /* human readable name for the UI              */
#define TTS_ERR_MAX  512
#define TTS_WAV_RATE 22050 /* sample rate of tts_render_wav() output: 16-bit mono PCM */

typedef enum {
    TTS_FAIL      = 0,
    TTS_SUCCESS   = 1,
    TTS_CANCELLED = 2      /* only returned by tts_render_wav() */
} TTSStatus;

typedef struct TTSVoice {
    wchar_t id[TTS_ID_MAX];     /* stable; use this (not name) for caching */
    wchar_t name[TTS_NAME_MAX]; /* display only */
} TTSVoice;

typedef struct TTSVoiceList {
    TTSVoice *items;            /* heap array, freed by tts_voices_free() */
    int       count;
} TTSVoiceList;

typedef struct TTSSpeechSettings {
    int rate;     /* -10 .. +10 (SAPI scale) */
    int volume;   /*   0 .. 100              */
    /* pitch etc. will be added with the Speech IR; SAPI has no plain pitch API */
} TTSSpeechSettings;

typedef struct TTSEngine TTSEngine;

/* --- voices ----------------------------------------------------------- */
TTSStatus tts_voices_enumerate(TTSVoiceList *out, wchar_t *err, size_t errcap);
void      tts_voices_free(TTSVoiceList *list);

/* --- engine lifetime -------------------------------------------------- */
TTSEngine *tts_engine_create(wchar_t *err, size_t errcap);
void       tts_engine_destroy(TTSEngine *e);   /* NULL-safe; stops speech */

/* --- configuration ---------------------------------------------------- */
/* Empty/NULL voice_id keeps the system default voice. */
TTSStatus tts_select_voice(TTSEngine *e, const wchar_t *voice_id,
                           wchar_t *err, size_t errcap);
TTSStatus tts_apply_settings(TTSEngine *e, const TTSSpeechSettings *s,
                             wchar_t *err, size_t errcap);

/* --- live playback: a queue of utterances, spoken back-to-back ---------- *
 * tts_enqueue() appends one utterance without interrupting what is already
 * speaking (gapless). Each gets a stream number (1,2,3... as assigned by SAPI);
 * tts_queue_status() reports which stream is speaking now. The Player module
 * uses this to follow sentences. */
TTSStatus tts_enqueue(TTSEngine *e, const wchar_t *text, unsigned long *stream_id,
                      wchar_t *err, size_t errcap);
/* current_stream: stream being spoken (0 if none yet); done: 1 when the queue
 * is empty and nothing is speaking. Fails if the last utterance failed. */
TTSStatus tts_queue_status(TTSEngine *e, unsigned long *current_stream, int *done,
                           wchar_t *err, size_t errcap);
/* Where the voice is inside the stream it is speaking: the character position (in the text given to tts_enqueue) of the word
 * being spoken, as of the last tts_queue_status call. Returns 0 when the voice does not report word positions. */
int       tts_word_position(TTSEngine *e, unsigned long *pos);
void      tts_pause(TTSEngine *e);
void      tts_resume(TTSEngine *e);
void      tts_stop(TTSEngine *e);              /* purge the whole queue; safe when idle */

/* --- offline rendering ------------------------------------------------ */
/* Blocks (polling `cancel` every ~100 ms) until the whole text has been
 * written to `path` as WAV in the voice's native PCM format. Run it on a
 * worker thread. On cancel/failure the partial file is deleted. */
TTSStatus tts_render_wav(TTSEngine *e, const wchar_t *text, const wchar_t *path,
                         volatile LONG *cancel, wchar_t *err, size_t errcap);

#endif
