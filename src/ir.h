/*
 * ir.h - Speech Intermediate Representation (Milestone 5). PURE C.
 *
 *   Document (sentences) --speech rules--> chunks --> IRSegment[] (what gets synthesized)
 *
 * A segment is one synthesis chunk of 1..chunk_sentences consecutive sentences of the SAME
 * paragraph, with a deterministic cache key. The key covers everything that changes the audio
 * samples and nothing that does not:
 *
 *   IN the key (speech-affecting)      speech text (so dictionary edits only touch the chunks whose
 *                                      text actually changed), voice id, rate, volume, pitch, engine id,
 *                                      output format (sample rate)
 *   NOT in the key (assembly-affecting) sentence/paragraph pauses, trimming, normalization, output file,
 *                                      pause scaling -> changing these never re-synthesizes anything.
 */
#ifndef IR_H
#define IR_H
#include <stddef.h>
#include <stdint.h>
#include "doc.h"
#include "speech.h"
#include "hash.h"
#include "chapters.h"

typedef struct SynthParams {
    wchar_t voice_id[256];
    int rate, volume, pitch;           /* SAPI scale; pitch is reserved (0) */
    const wchar_t *engine_id;          /* e.g. L"sapi5" */
    uint32_t wav_rate;                 /* output sample rate of the cached PCM */
    int chunk_sentences;               /* 1..8 sentences per synthesis chunk */
    int chunk_max_chars;               /* start a new chunk before exceeding this */
} SynthParams;

typedef struct IRSegment {
    int id;
    int first_sent, last_sent;         /* sentence range (document indices) */
    int para;                          /* paragraph index */
    int para_end;                      /* last segment of its paragraph */
    int chapter;                       /* chapter index (-1 = before the first chapter) */
    int chapter_end;                   /* last segment of its chapter */
    size_t src_start, src_len;         /* span in the normalized text */
    wchar_t *text;                     /* exactly what the engine receives */
    Hash128 key;
} IRSegment;

typedef struct IR { IRSegment *seg; int n, cap; } IR;

Hash128 ir_key(const wchar_t *text, size_t n, const SynthParams *p);
/* Build segments for sentences first..last (inclusive). 1 = ok, 0 = out of memory. */
/* chs may be NULL (no chapters). */
int  ir_build(IR *ir, const Document *doc, const Speech *sp, int first, int last, const SynthParams *p, const ChapterList *chs);
/* Pause kind after each segment for assemble_wav(): 0 sentence, 1 paragraph, 2 chapter. n bytes. */
void ir_pause_kinds(const IR *ir, unsigned char *kinds);
void ir_free(IR *ir);
#endif
