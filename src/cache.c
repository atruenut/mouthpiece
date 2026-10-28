#include "cache.h"
#include "plat.h"
#include "wav.h"
#include <stdlib.h>
#include <string.h>

/* Use the platform's own separator in every path we hand out (SAPI and Win32 are happiest with '\\'). */
static void fix_seps(wchar_t *p)
{
#ifdef _WIN32
    for (; *p; p++) if (*p == L'/') *p = L'\\';
#else
    (void)p;
#endif
}

#define SET_ERR(...) do { if (err && errcap) { _snwprintf(err, errcap, __VA_ARGS__); err[errcap - 1] = 0; } } while (0)

static size_t slot_for(const CacheEntry *tab, size_t cap, const Hash128 *k)
{
    size_t i = (size_t)(k->a ^ (k->b >> 7)) & (cap - 1);
    while (tab[i].used && !hash_eq(&tab[i].key, k)) i = (i + 1) & (cap - 1);
    return i;
}

static int table_put(Cache *c, const CacheEntry *e)
{
    if ((c->count + 1) * 2 > c->cap) {                       /* grow at 50% load */
        size_t nc = c->cap ? c->cap * 2 : 1024;
        CacheEntry *nt = (CacheEntry *)calloc(nc, sizeof *nt);
        if (!nt) return 0;
        for (size_t i = 0; i < c->cap; i++)
            if (c->tab[i].used) nt[slot_for(nt, nc, &c->tab[i].key)] = c->tab[i];
        free(c->tab);
        c->tab = nt; c->cap = nc;
    }
    size_t i = slot_for(c->tab, c->cap, &e->key);
    if (!c->tab[i].used) c->count++;
    c->tab[i] = *e;
    c->tab[i].used = 1;
    return 1;
}

/* Deleting from open addressing: rebuild without the entry (rare operation). */
static void table_del(Cache *c, const Hash128 *k)
{
    if (!c->cap) return;
    size_t i = slot_for(c->tab, c->cap, k);
    if (!c->tab[i].used) return;
    CacheEntry *old = c->tab;
    size_t oc = c->cap;
    c->tab = (CacheEntry *)calloc(oc, sizeof *c->tab);
    if (!c->tab) { c->tab = old; return; }
    c->count = 0;
    for (size_t j = 0; j < oc; j++)
        if (old[j].used && !hash_eq(&old[j].key, k)) { c->tab[slot_for(c->tab, oc, &old[j].key)] = old[j]; c->count++; }
    free(old);
}

static void hex_wide(const Hash128 *k, wchar_t out[33])
{
    char hex[33];
    hash_hex(k, hex);
    for (int i = 0; i < 33; i++) out[i] = (wchar_t)hex[i];
}

void cache_segment_path(const Cache *c, const Hash128 *k, wchar_t *out, size_t cap)
{
    wchar_t hex[33], d[3];
    hex_wide(k, hex);
    d[0] = hex[0]; d[1] = hex[1]; d[2] = 0;
    _snwprintf(out, cap, L"%ls/segments/%ls/%ls.wav", c->root, d, hex);
    out[cap - 1] = 0;
    fix_seps(out);
}

void cache_tmp_path(const Cache *c, const Hash128 *k, wchar_t *out, size_t cap)
{
    wchar_t hex[33];
    hex_wide(k, hex);
    _snwprintf(out, cap, L"%ls/tmp/%ls.wav", c->root, hex);
    out[cap - 1] = 0;
    fix_seps(out);
}

static void log_path(const Cache *c, wchar_t *out, size_t cap)
{
    _snwprintf(out, cap, L"%ls/index.log", c->root);
    out[cap - 1] = 0;
    fix_seps(out);
}

static void load_index(Cache *c)
{
    wchar_t lp[560];
    log_path(c, lp, 560);
    FILE *f = plat_fopen(lp, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n == 0 || line[n - 1] != '\n') continue;           /* torn last line: ignore */
        CacheEntry e;
        memset(&e, 0, sizeof e);
        if (line[0] == '-') {                                  /* tombstone: -<hex> */
            Hash128 k;
            if (hash_from_hex(line + 1, &k)) table_del(c, &k);
            continue;
        }
        unsigned long s, crc, rate, fb;
        if (strlen(line) < 34 || !hash_from_hex(line, &e.key)) continue;
        if (sscanf(line + 32, " %lu %lx %lu %lu", &s, &crc, &rate, &fb) != 4) continue;
        e.samples = (uint32_t)s; e.crc = (uint32_t)crc; e.rate = (uint32_t)rate; e.file_bytes = (uint32_t)fb;
        table_put(c, &e);
    }
    fclose(f);
}

int cache_open(Cache *c, const wchar_t *root, wchar_t *err, size_t errcap)
{
    memset(c, 0, sizeof *c);
    if (wcslen(root) >= sizeof c->root / sizeof(wchar_t) - 1) { SET_ERR(L"Cache path is too long."); return 0; }
    wcscpy(c->root, root);
    wchar_t d[560];
    _snwprintf(d, 560, L"%ls/segments", c->root); d[559] = 0; fix_seps(d);
    if (!plat_mkdir_p(d)) { SET_ERR(L"Unable to create the audio cache folder:\n%ls", d); return 0; }
    _snwprintf(d, 560, L"%ls/tmp", c->root); d[559] = 0; fix_seps(d);
    plat_mkdir_p(d);
    load_index(c);
    wchar_t lp[560];
    log_path(c, lp, 560);
    c->log = plat_fopen(lp, "ab");
    if (!c->log) { SET_ERR(L"Unable to open the audio cache index:\n%ls", lp); free(c->tab); c->tab = NULL; return 0; }
    return 1;
}

void cache_close(Cache *c)
{
    if (c->log) fclose(c->log);
    free(c->tab);
    memset(c, 0, sizeof *c);
}

int cache_lookup(const Cache *c, const Hash128 *k, CacheEntry *e)
{
    if (!c->cap) return 0;
    size_t i = slot_for(c->tab, c->cap, k);
    if (!c->tab[i].used) return 0;
    wchar_t p[600];
    unsigned long long sz = 0;
    cache_segment_path(c, k, p, 600);
    if (!plat_file_size(p, &sz) || sz != c->tab[i].file_bytes) return 0;
    if (e) *e = c->tab[i];
    return 1;
}

int cache_commit(Cache *c, const Hash128 *k, const wchar_t *tmp, uint32_t rate, wchar_t *err, size_t errcap)
{
    FILE *f = plat_fopen(tmp, "rb");
    if (!f) { SET_ERR(L"Synthesized audio file is missing:\n%ls", tmp); return 0; }
    WavInfo w;
    if (!wav_read_info(f, &w) || w.channels != 1 || w.rate != rate) {
        fclose(f);
        plat_remove(tmp);
        SET_ERR(L"The speech engine produced audio in an unexpected format (expected 16-bit mono %lu Hz).", (unsigned long)rate);
        return 0;
    }
    uint32_t crc = 0;
    unsigned char buf[16384];
    fseek(f, w.data_offset, SEEK_SET);
    for (uint32_t left = w.data_bytes; left;) {
        size_t n = left > sizeof buf ? sizeof buf : left;
        if (fread(buf, 1, n, f) != n) { fclose(f); plat_remove(tmp); SET_ERR(L"Unable to read synthesized audio."); return 0; }
        crc = crc32_update(crc, buf, n);
        left -= (uint32_t)n;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);

    wchar_t dest[600], dir[600];
    cache_segment_path(c, k, dest, 600);
    wcscpy(dir, dest);
    wchar_t *slash = wcsrchr(dir, L'/'), *bslash = wcsrchr(dir, L'\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = 0;
    if (!plat_mkdir_p(dir) || !plat_rename_replace(tmp, dest)) {
        plat_remove(tmp);
        SET_ERR(L"Unable to store audio in the cache:\n%ls", dest);
        return 0;
    }
    CacheEntry e;
    memset(&e, 0, sizeof e);
    e.key = *k; e.samples = w.data_bytes / 2; e.crc = crc; e.rate = rate; e.file_bytes = (uint32_t)fsize;
    char hex[33];
    hash_hex(k, hex);
    if (fprintf(c->log, "%s %lu %08lx %lu %lu\n", hex, (unsigned long)e.samples, (unsigned long)e.crc,
                (unsigned long)e.rate, (unsigned long)e.file_bytes) < 0 || fflush(c->log) != 0) {
        SET_ERR(L"Unable to update the audio cache index.");
        return 0;
    }
    return table_put(c, &e);
}

void cache_remove(Cache *c, const Hash128 *k)
{
    wchar_t p[600];
    cache_segment_path(c, k, p, 600);
    plat_remove(p);
    char hex[33];
    hash_hex(k, hex);
    if (c->log) { fprintf(c->log, "-%s\n", hex); fflush(c->log); }
    table_del(c, k);
}

void cache_stats(const Cache *c, unsigned long *files, unsigned long long *bytes)
{
    wchar_t d[560];
    _snwprintf(d, 560, L"%ls/segments", c->root); d[559] = 0; fix_seps(d);
    plat_tree_stats(d, files, bytes);
}

int cache_clear(Cache *c, wchar_t *err, size_t errcap)
{
    wchar_t root[480];
    wcscpy(root, c->root);
    if (c->log) fclose(c->log);
    free(c->tab);
    plat_remove_tree(root);
    return cache_open(c, root, err, errcap);
}
