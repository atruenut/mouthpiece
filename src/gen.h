/*
 * gen.h - background GENERATION job (Milestone 5, Windows).
 *
 *   IR segments --(cache hit? use : synthesize via SAPI -> cache)--> assemble --> output WAV
 *
 * Runs on a worker thread with its own COM apartment and its own TTSEngine. The UI only reads
 * the progress counters and receives one PostMessage when the job ends. While a job runs the
 * Cache is owned by the worker: the UI must not touch it until the "done" message arrives.
 * Cancelling keeps every segment already committed to the cache (they remain valid).
 */
#ifndef GEN_H
#define GEN_H
#include <windows.h>
#include "cache.h"
#include "ir.h"
#include "assemble.h"
#include "tts_engine.h"

typedef enum { GEN_RUNNING, GEN_OK, GEN_CANCELLED, GEN_FAILED } GenState;

typedef struct GenJob {
    Cache *cache;
    IR ir;                              /* owned */
    SynthParams synth;
    AsmOptions asmo;
    wchar_t out_path[MAX_PATH];         /* single-file mode */
    int per_chapter;                    /* one WAV per chapter into out_dir, named "NN - Title.wav" */
    wchar_t out_dir[MAX_PATH];
    wchar_t **titles; int ntitles;      /* chapter titles for file names (owned copy) */
    HWND notify;
    UINT msg_done;

    volatile LONG phase;                /* 0 = synthesizing, 1 = assembling */
    volatile LONG done, total;
    volatile LONG hits, synthesized, repaired, files;
    volatile LONG cancel;

    GenState state;                     /* valid once msg_done arrived */
    wchar_t err[TTS_ERR_MAX + MAX_PATH];
    uint64_t samples;
    HANDLE thread;
} GenJob;

/* Takes ownership of *ir (it is zeroed). NULL on failure (err filled). */
GenJob *gen_start(Cache *cache, IR *ir, const SynthParams *synth, const AsmOptions *asmo,
                  const wchar_t *out_path, int per_chapter, const wchar_t *out_dir, const ChapterList *chs,
                  HWND notify, UINT msg_done, wchar_t *err, size_t errcap);
void gen_cancel(GenJob *j);
void gen_free(GenJob *j);               /* waits for the thread, frees everything */
#endif
