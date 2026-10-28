/*
 * cache.h - persistent per-segment PCM cache. PURE C (uses plat.h for the file system).
 *
 *   <root>/segments/ab/<32 hex>.wav     one synthesized chunk, plain PCM WAV
 *   <root>/tmp/<32 hex>.wav             synthesis target; moved into place by cache_commit()
 *   <root>/index.log                    append-only text log: key, samples, crc32, rate, file size
 *
 * Correctness never rests on a file name alone: an entry only counts as a hit if the index knows
 * it AND the file exists with exactly the recorded size. The CRC recorded at commit time is
 * verified when the audio is read for assembly (assemble.c), so a bit-rotted or truncated
 * file is detected, removed and regenerated.
 *
 * Crash safety: audio is written to tmp/, validated, renamed into place, and only then is the
 * index line appended and flushed. A crash can leave at most an orphan file or a torn last
 * index line (ignored on load). Completed segments are therefore always valid.
 * Not thread-safe: use from one thread at a time.
 */
#ifndef CACHE_H
#define CACHE_H
#include <stdint.h>
#include <stdio.h>
#include "hash.h"

typedef struct CacheEntry {
    Hash128  key;
    uint32_t samples, crc, rate, file_bytes;
    int      used;
} CacheEntry;

typedef struct Cache {
    wchar_t root[480];
    CacheEntry *tab;
    size_t cap, count;
    FILE *log;
} Cache;

int  cache_open(Cache *c, const wchar_t *root, wchar_t *err, size_t errcap);
void cache_close(Cache *c);
void cache_segment_path(const Cache *c, const Hash128 *k, wchar_t *out, size_t cap);
void cache_tmp_path(const Cache *c, const Hash128 *k, wchar_t *out, size_t cap);
/* 1 = valid hit (fills *e). */
int  cache_lookup(const Cache *c, const Hash128 *k, CacheEntry *e);
/* Validate the WAV at `tmp` (PCM, 16-bit, mono, `rate`), compute its CRC, move it into the cache, log it. */
int  cache_commit(Cache *c, const Hash128 *k, const wchar_t *tmp, uint32_t rate, wchar_t *err, size_t errcap);
void cache_remove(Cache *c, const Hash128 *k);
void cache_stats(const Cache *c, unsigned long *files, unsigned long long *bytes);
int  cache_clear(Cache *c, wchar_t *err, size_t errcap);
#endif
