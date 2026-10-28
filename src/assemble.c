#include "assemble.h"
#include "wav.h"
#include <stdlib.h>
#include <string.h>

typedef struct Seg {
    int16_t *s;
    size_t n, a, b;              /* samples; kept range [a,b) after trimming */
} Seg;

static void seg_free(Seg *g) { free(g->s); memset(g, 0, sizeof *g); }

/* Load segment i. Returns 1 ok, 0 bad/unreadable (closes the file either way). */
static int seg_load(Seg *g, FILE *f, uint32_t expect_crc, int check_crc, const AsmOptions *o)
{
    WavInfo w;
    memset(g, 0, sizeof *g);
    if (!f) return 0;
    if (!wav_read_info(f, &w) || w.channels != 1 || w.rate != o->rate || (w.data_bytes & 1)) { fclose(f); return 0; }
    unsigned char *raw = (unsigned char *)malloc(w.data_bytes ? w.data_bytes : 1);
    if (!raw) { fclose(f); return 0; }
    if (fseek(f, w.data_offset, SEEK_SET) != 0 || fread(raw, 1, w.data_bytes, f) != w.data_bytes) { free(raw); fclose(f); return 0; }
    fclose(f);
    if (check_crc && crc32_update(0, raw, w.data_bytes) != expect_crc) { free(raw); return 0; }
    g->n = w.data_bytes / 2;
    g->s = (int16_t *)malloc((g->n ? g->n : 1) * sizeof(int16_t));
    if (!g->s) { free(raw); return 0; }
    for (size_t i = 0; i < g->n; i++) g->s[i] = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
    free(raw);
    g->a = 0; g->b = g->n;
    if (o->trim_silence) {
        int thr = o->trim_threshold > 0 ? o->trim_threshold : 300;
        size_t margin = (size_t)o->rate * 5 / 1000, a = 0, b = g->n;
        while (a < b && abs(g->s[a]) < thr) a++;
        if (a == b) { g->a = g->b = 0; return 1; }              /* all silence */
        while (b > a && abs(g->s[b - 1]) < thr) b--;
        g->a = a > margin ? a - margin : 0;
        g->b = b + margin < g->n ? b + margin : g->n;
    }
    return 1;
}

static int write_zeros(FILE *out, uint64_t n)
{
    static const unsigned char z[8192];
    uint64_t bytes = n * 2;
    while (bytes) {
        size_t c = bytes > sizeof z ? sizeof z : (size_t)bytes;
        if (fwrite(z, 1, c, out) != c) return 0;
        bytes -= c;
    }
    return 1;
}

AsmStatus assemble_wav(FILE *out, int nseg, const unsigned char *pause_kind, const uint32_t *crc,
                       AsmOpenFn open, void *ctx, AsmProgressFn progress, void *pctx,
                       const AsmOptions *o, AsmResult *res)
{
    AsmResult r;
    memset(&r, 0, sizeof r);
    r.bad_segment = -1; r.gain = 1.0;
    if (res) *res = r;
    int passes = o->normalize ? 2 : 1, step = 0, total = nseg * passes;
    int peak = 0;

    if (o->normalize) {                                          /* pass 1: find the peak */
        for (int i = 0; i < nseg; i++) {
            Seg g;
            if (!seg_load(&g, open(ctx, i), crc ? crc[i] : 0, crc != NULL, o)) { r.bad_segment = i; if (res) *res = r; return ASM_BAD_SEGMENT; }
            for (size_t k = g.a; k < g.b; k++) { int v = abs(g.s[k]); if (v > peak) peak = v; }
            seg_free(&g);
            if (progress && progress(pctx, ++step, total)) { if (res) *res = r; return ASM_CANCELLED; }
        }
        r.peak = peak;
        if (peak > 0) { r.gain = 29000.0 / peak; if (r.gain > 8.0) r.gain = 8.0; }
    }

    if (!wav_write_header(out, o->rate, 1, 16, 0)) return ASM_IO;
    uint64_t samples = 0;
    unsigned char buf[8192];
    for (int i = 0; i < nseg; i++) {
        Seg g;
        if (!seg_load(&g, open(ctx, i), crc ? crc[i] : 0, crc != NULL, o)) { r.bad_segment = i; if (res) *res = r; return ASM_BAD_SEGMENT; }
        for (size_t k = g.a; k < g.b;) {
            size_t c = 0;
            while (k < g.b && c + 2 <= sizeof buf) {
                long v = g.s[k++];
                if (r.gain != 1.0) {
                    double d = v * r.gain;
                    v = (long)(d < 0 ? d - 0.5 : d + 0.5);
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                }
                buf[c++] = (unsigned char)(v & 0xFF);
                buf[c++] = (unsigned char)((v >> 8) & 0xFF);
            }
            if (fwrite(buf, 1, c, out) != c) { seg_free(&g); return ASM_IO; }
        }
        samples += g.b - g.a;
        seg_free(&g);
        if (i + 1 < nseg) {
            int k = pause_kind ? pause_kind[i] : 0;
            int ms = k >= 2 ? o->chapter_ms : (k == 1 ? o->paragraph_ms : o->sentence_ms);
            uint64_t z = ms > 0 ? (uint64_t)o->rate * (uint64_t)ms / 1000 : 0;
            if (!write_zeros(out, z)) return ASM_IO;
            samples += z;
        }
        if (samples * 2 > 0xFFFFFFFFULL - 36) { if (res) { r.samples = samples; *res = r; } return ASM_TOO_BIG; }
        if (progress && progress(pctx, ++step, total)) { r.samples = samples; if (res) *res = r; return ASM_CANCELLED; }
    }
    if (fseek(out, 0, SEEK_SET) != 0 || !wav_write_header(out, o->rate, 1, 16, (uint32_t)(samples * 2))) return ASM_IO;
    fseek(out, 0, SEEK_END);
    r.samples = samples;
    if (res) *res = r;
    return ASM_OK;
}
