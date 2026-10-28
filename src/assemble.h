/*
 * assemble.h - AUDIO ASSEMBLY (Milestone 5). PURE C.
 *
 * Turns cached PCM segments into one WAV. Nothing here calls the speech engine, so every
 * setting below is "assembly-affecting": changing it never causes re-synthesis.
 *   - deterministic silence between segments (sentence vs paragraph pause)
 *   - optional trimming of leading/trailing near-silence of each segment (5 ms margin kept)
 *   - optional peak normalization of the whole file (to about -1 dBFS, gain capped at 8x)
 * Segment audio is CRC-checked while it is read; a mismatch is reported (not hidden) so the
 * caller can drop the cache entry and regenerate it.
 */
#ifndef ASSEMBLE_H
#define ASSEMBLE_H
#include <stdint.h>
#include <stdio.h>

typedef struct AsmOptions {
    uint32_t rate;               /* required sample rate of every segment */
    int sentence_ms, paragraph_ms, chapter_ms;
    int trim_silence;
    int trim_threshold;          /* |sample| below this counts as silence (default 300) */
    int normalize;
} AsmOptions;

typedef enum { ASM_OK = 0, ASM_BAD_SEGMENT, ASM_IO, ASM_CANCELLED, ASM_TOO_BIG } AsmStatus;

typedef struct AsmResult {
    uint64_t samples;            /* total samples written */
    int bad_segment;             /* index when ASM_BAD_SEGMENT, else -1 */
    int peak;                    /* loudest sample seen (after trimming), normalize only */
    double gain;
} AsmResult;

typedef FILE *(*AsmOpenFn)(void *ctx, int index);               /* caller's file; assemble_wav() closes it */
typedef int   (*AsmProgressFn)(void *ctx, int done, int total); /* return non-zero to cancel */

/* pause_kind[i] says what follows segment i: 0 sentence pause, 1 paragraph pause, 2 chapter pause
 * (NULL = all sentence pauses). crc may be NULL (no verification). */
AsmStatus assemble_wav(FILE *out, int nseg, const unsigned char *pause_kind, const uint32_t *crc,
                       AsmOpenFn open, void *ctx, AsmProgressFn progress, void *pctx,
                       const AsmOptions *o, AsmResult *res);
#endif
