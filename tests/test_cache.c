/* Tests for hash.c, wav.c, cache.c, ir.c and assemble.c - the Milestone 5 pipeline. */
#include "../src/ir.h"
#include "../src/cache.h"
#include "../src/assemble.h"
#include "../src/wav.h"
#include "../src/plat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static wchar_t g_root[256];

/* ---------------------------------------------------------------- helpers */

static SynthParams params(void)
{
    SynthParams p;
    memset(&p, 0, sizeof p);
    wcscpy(p.voice_id, L"HKLM\\Voices\\Tokens\\David");
    p.rate = 0; p.volume = 100;
    p.engine_id = L"sapi5";
    p.wav_rate = 22050;
    p.chunk_sentences = 1;
    p.chunk_max_chars = 700;
    return p;
}

static void make_doc(Document *d, const wchar_t *text)
{
    doc_init(d);
    doc_build(d, text, wcslen(text));
}

static int count_diff(const IR *a, const IR *b)
{
    int diff = 0;
    if (a->n != b->n) return -1;
    for (int i = 0; i < a->n; i++) if (!hash_eq(&a->seg[i].key, &b->seg[i].key)) diff++;
    return diff;
}

/* A made-up book: 4 paragraphs of numbered sentences, a few mention "Machiavelli". */
static wchar_t *book(const wchar_t *swap_word)
{
    wchar_t *b = (wchar_t *)malloc(20000 * sizeof(wchar_t));
    size_t n = 0;
    for (int p = 0; p < 4; p++) {
        for (int s = 0; s < 6; s++) {
            const wchar_t *w = (p == 1 && s == 2) ? swap_word : L"alpha";
            n += (size_t)swprintf(b + n, 20000 - n, L"Sentence %d of part %d mentions %ls %ls here. ",
                                  s + 1, p + 1, w, ((p * 6 + s) % 5 == 0) ? L"Machiavelli" : L"beta");
        }
        n += (size_t)swprintf(b + n, 20000 - n, L"\n\n");
    }
    return b;
}

/* ------------------------------------------------------------------ hash */

static void test_hash(void)
{
    HashState h1, h2;
    hash_init(&h1); hash_wstr(&h1, L"hello", 5); Hash128 a = hash_final(&h1);
    hash_init(&h2); hash_wstr(&h2, L"hello", 5); Hash128 b = hash_final(&h2);
    CHECK(hash_eq(&a, &b), "same input, same hash");
    hash_init(&h2); hash_wstr(&h2, L"hellp", 5); b = hash_final(&h2);
    CHECK(!hash_eq(&a, &b), "different input, different hash");
    hash_init(&h1); hash_wstr(&h1, L"ab", 2); hash_wstr(&h1, L"c", 1); a = hash_final(&h1);
    hash_init(&h2); hash_wstr(&h2, L"a", 1); hash_wstr(&h2, L"bc", 2); b = hash_final(&h2);
    CHECK(!hash_eq(&a, &b), "field boundaries matter (separator)");
    char hex[33]; Hash128 c;
    hash_hex(&a, hex);
    CHECK(strlen(hex) == 32 && hash_from_hex(hex, &c) && hash_eq(&a, &c), "hex round trip");
    CHECK(!hash_from_hex("zz", &c), "bad hex rejected");
    /* no collisions among many similar short strings */
    enum { N = 20000 };
    Hash128 *all = (Hash128 *)malloc(N * sizeof *all);
    for (int i = 0; i < N; i++) { wchar_t t[32]; int l = swprintf(t, 32, L"Sentence %d.", i); hash_init(&h1); hash_wstr(&h1, t, (size_t)l); all[i] = hash_final(&h1); }
    int coll = 0;
    for (int i = 0; i < N && coll == 0; i++) for (int j = i + 1; j < i + 50 && j < N; j++) if (hash_eq(&all[i], &all[j])) coll++;
    CHECK(coll == 0, "no collisions in a similar-string family");
    free(all);
}

/* ------------------------------------------------------------------ IR / invalidation */

static void test_ir_invalidation(void)
{
    Speech sp; memset(&sp, 0, sizeof sp); sp.dict = NULL; speech_default_options(&sp.opts);
    SynthParams p = params();
    wchar_t *t0 = book(L"gamma");
    Document d0; make_doc(&d0, t0);
    IR a, b;
    CHECK(d0.nsent == 24 && d0.npara == 4, "book has 24 sentences / 4 paragraphs (got %d / %d)", d0.nsent, d0.npara);
    CHECK(ir_build(&a, &d0, &sp, 0, d0.nsent - 1, &p, NULL), "ir");
    CHECK(a.n == 24, "one segment per sentence, got %d", a.n);
    int pe = 0;
    for (int i = 0; i < a.n; i++) pe += a.seg[i].para_end;
    CHECK(pe == 4, "four paragraph ends, got %d", pe);

    /* determinism */
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &p, NULL) && count_diff(&a, &b) == 0, "rebuilding gives identical keys");
    ir_free(&b);

    /* A. source change: one word in one sentence -> exactly one segment */
    wchar_t *t1 = book(L"delta");
    Document d1; make_doc(&d1, t1);
    CHECK(ir_build(&b, &d1, &sp, 0, d1.nsent - 1, &p, NULL), "ir");
    CHECK(count_diff(&a, &b) == 1, "editing one sentence changes exactly 1 key (got %d)", count_diff(&a, &b));
    ir_free(&b); doc_free(&d1); free(t1);

    /* B. pronunciation rule: only sentences containing the word change */
    PronDict dict; pron_init(&dict);
    pron_add(&dict, L"Machiavelli", L"Mack ee uh vell ee", 0, 0);
    pron_prepare(&dict);
    Speech sp2 = sp; sp2.dict = &dict;
    int expect = 0;
    for (int i = 0; i < d0.nsent; i++) { wchar_t *x = doc_speech_text(&d0, i); if (wcsstr(x, L"Machiavelli")) expect++; free(x); }
    CHECK(ir_build(&b, &d0, &sp2, 0, d0.nsent - 1, &p, NULL), "ir");
    CHECK(expect > 0 && count_diff(&a, &b) == expect, "dictionary rule invalidates only the %d sentences containing the word (got %d)", expect, count_diff(&a, &b));
    ir_free(&b);
    /* a rule for a word that does not occur invalidates nothing */
    pron_add(&dict, L"Nietzsche", L"Nee chuh", 0, 0);
    pron_prepare(&dict);
    pron_remove(&dict, 0);
    pron_prepare(&dict);
    CHECK(ir_build(&b, &d0, &sp2, 0, d0.nsent - 1, &p, NULL) && count_diff(&a, &b) == 0, "irrelevant rule invalidates nothing");
    ir_free(&b);
    pron_free(&dict);

    /* C. speech-affecting settings invalidate everything */
    SynthParams q;
    q = p; wcscpy(q.voice_id, L"HKLM\\Voices\\Tokens\\Zira");
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &q, NULL) && count_diff(&a, &b) == a.n, "voice change -> all keys"); ir_free(&b);
    q = p; q.rate = -2;
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &q, NULL) && count_diff(&a, &b) == a.n, "rate change -> all keys"); ir_free(&b);
    q = p; q.volume = 80;
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &q, NULL) && count_diff(&a, &b) == a.n, "volume change -> all keys"); ir_free(&b);
    q = p; q.wav_rate = 44100;
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &q, NULL) && count_diff(&a, &b) == a.n, "output format change -> all keys"); ir_free(&b);
    q = p; q.engine_id = L"other";
    CHECK(ir_build(&b, &d0, &sp, 0, d0.nsent - 1, &q, NULL) && count_diff(&a, &b) == a.n, "engine change -> all keys"); ir_free(&b);

    /* D. speech-rule options that alter the text invalidate only affected sentences ("Sentence 1" digits) */
    Speech sp3 = sp; sp3.opts.numbers = 0;
    CHECK(ir_build(&b, &d0, &sp3, 0, d0.nsent - 1, &p, NULL), "ir");
    CHECK(count_diff(&a, &b) == a.n, "digits appear in every sentence, so all change (got %d)", count_diff(&a, &b));
    ir_free(&b);

    /* E. chunking: 3 sentences per segment, edits stay inside their paragraph */
    SynthParams c3 = p; c3.chunk_sentences = 3;
    IR ca, cb;
    wchar_t *t2 = book(L"delta");
    Document d2; make_doc(&d2, t2);
    CHECK(ir_build(&ca, &d0, &sp, 0, d0.nsent - 1, &c3, NULL) && ca.n == 8, "24 sentences / 3 = 8 chunks (got %d)", ca.n);
    CHECK(ir_build(&cb, &d2, &sp, 0, d2.nsent - 1, &c3, NULL) && count_diff(&ca, &cb) == 1, "edit changes exactly one 3-sentence chunk (got %d)", count_diff(&ca, &cb));
    for (int i = 0; i < ca.n; i++) CHECK(ca.seg[i].last_sent - ca.seg[i].first_sent < 3, "chunk size bound");
    int cross = 0;
    for (int i = 0; i < ca.n; i++) if (d0.sents[ca.seg[i].first_sent].para != d0.sents[ca.seg[i].last_sent].para) cross++;
    CHECK(cross == 0, "chunks never span paragraphs");
    ir_free(&ca); ir_free(&cb); doc_free(&d2); free(t2);

    /* F. a partial range builds a subset with the same keys */
    CHECK(ir_build(&b, &d0, &sp, 6, 11, &p, NULL) && b.n == 6, "range build");
    for (int i = 0; i < b.n; i++) CHECK(hash_eq(&b.seg[i].key, &a.seg[6 + i].key), "range keys equal full-build keys");
    ir_free(&b);

    ir_free(&a); doc_free(&d0); free(t0);
}

/* ------------------------------------------------------------------ cache */

static void make_wav(const wchar_t *path, uint32_t rate, int channels, size_t nsamp, double freq, int amp, size_t lead_silence)
{
    FILE *f = plat_fopen(path, "wb");
    wav_write_header(f, rate, (uint16_t)channels, 16, (uint32_t)(nsamp * 2 * (size_t)channels));
    for (size_t i = 0; i < nsamp; i++) {
        int16_t v = i < lead_silence ? 0 : (int16_t)(amp * sin(2 * 3.14159265358979 * freq * (double)i / rate));
        for (int c = 0; c < channels; c++) { unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) }; fwrite(b, 1, 2, f); }
    }
    fclose(f);
}

static Hash128 K(const char *name)
{
    HashState h; hash_init(&h); hash_bytes(&h, name, strlen(name)); return hash_final(&h);
}

static void test_cache(void)
{
    wchar_t err[256], tmp[600], seg[600];
    Cache c;
    CHECK(cache_open(&c, g_root, err, 256), "open: %ls", err);
    Hash128 k1 = K("one"), k2 = K("two"), k3 = K("three");
    CacheEntry e;
    CHECK(!cache_lookup(&c, &k1, &e), "empty cache misses");

    cache_tmp_path(&c, &k1, tmp, 600);
    make_wav(tmp, 22050, 1, 1000, 440, 8000, 0);
    CHECK(cache_commit(&c, &k1, tmp, 22050, err, 256), "commit: %ls", err);
    CHECK(cache_lookup(&c, &k1, &e) && e.samples == 1000 && e.rate == 22050, "hit with metadata");
    unsigned long long sz;
    CHECK(!plat_file_size(tmp, &sz), "tmp file was moved away");

    /* wrong format is rejected and cleaned up */
    cache_tmp_path(&c, &k2, tmp, 600);
    make_wav(tmp, 16000, 1, 500, 440, 8000, 0);
    CHECK(!cache_commit(&c, &k2, tmp, 22050, err, 256), "wrong sample rate rejected");
    CHECK(!plat_file_size(tmp, &sz) && !cache_lookup(&c, &k2, NULL), "rejected file not stored");
    make_wav(tmp, 22050, 2, 500, 440, 8000, 0);
    CHECK(!cache_commit(&c, &k2, tmp, 22050, err, 256), "stereo rejected");
    FILE *g = plat_fopen(tmp, "wb"); fwrite("not a wav file at all, just text........................", 1, 56, g); fclose(g);
    CHECK(!cache_commit(&c, &k2, tmp, 22050, err, 256), "garbage rejected");

    cache_tmp_path(&c, &k2, tmp, 600);
    make_wav(tmp, 22050, 1, 2000, 330, 8000, 0);
    CHECK(cache_commit(&c, &k2, tmp, 22050, err, 256), "second commit");
    unsigned long files; unsigned long long bytes;
    cache_stats(&c, &files, &bytes);
    CHECK(files == 2 && bytes > 4000, "stats: %lu files %llu bytes", files, bytes);
    cache_close(&c);

    /* persistence + torn index line */
    wchar_t lp[600];
    swprintf(lp, 600, L"%ls/index.log", g_root);
    FILE *lf = plat_fopen(lp, "ab"); fputs("deadbeefdeadbeef", lf); fclose(lf);            /* torn, no newline */
    CHECK(cache_open(&c, g_root, err, 256), "reopen");
    CHECK(cache_lookup(&c, &k1, &e) && cache_lookup(&c, &k2, &e) && e.samples == 2000, "entries survive restart");

    /* truncated file -> miss (size check) */
    cache_segment_path(&c, &k2, seg, 600);
    FILE *tf = plat_fopen(seg, "rb"); unsigned char head[100]; size_t hn = fread(head, 1, 100, tf); fclose(tf);
    tf = plat_fopen(seg, "wb"); fwrite(head, 1, hn, tf); fclose(tf);
    CHECK(!cache_lookup(&c, &k2, &e), "truncated file is not a hit");
    cache_remove(&c, &k2);
    CHECK(!cache_lookup(&c, &k2, NULL), "removed");
    cache_close(&c);
    CHECK(cache_open(&c, g_root, err, 256) && !cache_lookup(&c, &k2, NULL) && cache_lookup(&c, &k1, NULL), "tombstone persists, other entry intact");

    /* an index entry whose file vanished is a miss */
    cache_segment_path(&c, &k1, seg, 600);
    plat_remove(seg);
    CHECK(!cache_lookup(&c, &k1, NULL), "missing file is a miss");

    /* many entries: table growth */
    for (int i = 0; i < 3000; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "bulk%d", i);
        Hash128 k = K(nm);
        cache_tmp_path(&c, &k, tmp, 600);
        make_wav(tmp, 22050, 1, 20, 440, 1000, 0);
        if (!cache_commit(&c, &k, tmp, 22050, err, 256)) { CHECK(0, "bulk commit %d: %ls", i, err); break; }
    }
    int hits = 0;
    for (int i = 0; i < 3000; i++) { char nm[32]; snprintf(nm, sizeof nm, "bulk%d", i); Hash128 k = K(nm); hits += cache_lookup(&c, &k, NULL); }
    CHECK(hits == 3000, "3000 entries all found (%d)", hits);
    cache_close(&c);
    CHECK(cache_open(&c, g_root, err, 256), "reopen big");
    hits = 0;
    for (int i = 0; i < 3000; i++) { char nm[32]; snprintf(nm, sizeof nm, "bulk%d", i); Hash128 k = K(nm); hits += cache_lookup(&c, &k, NULL); }
    CHECK(hits == 3000, "3000 entries persisted (%d)", hits);

    CHECK(cache_clear(&c, err, 256), "clear");
    cache_stats(&c, &files, &bytes);
    CHECK(files == 0 && !cache_lookup(&c, &k3, NULL), "cleared");
    cache_close(&c);
}

/* ------------------------------------------------------------------ assemble */

typedef struct { Cache *c; Hash128 *keys; } OpenCtx;
static FILE *open_seg(void *ctx, int i)
{
    OpenCtx *o = (OpenCtx *)ctx;
    wchar_t p[600];
    cache_segment_path(o->c, &o->keys[i], p, 600);
    return plat_fopen(p, "rb");
}
static int cancel_after;
static int progress_cb(void *ctx, int done, int total) { (void)ctx; (void)total; return cancel_after && done >= cancel_after; }

static int peak_of(const wchar_t *path, size_t *nsamp)
{
    FILE *f = plat_fopen(path, "rb");
    WavInfo w;
    int ok = wav_read_info(f, &w), peak = 0;
    fseek(f, w.data_offset, SEEK_SET);
    size_t n = w.data_bytes / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) break;
        int v = (int16_t)(uint16_t)(b[0] | (b[1] << 8));
        if (abs(v) > peak) peak = abs(v);
    }
    fclose(f);
    *nsamp = n;
    return ok ? peak : -1;
}

static void test_assemble(void)
{
    wchar_t err[256], tmp[600], outp[600];
    Cache c;
    CHECK(cache_open(&c, g_root, err, 256), "open");
    enum { N = 3 };
    Hash128 keys[N] = { K("a0"), K("a1"), K("a2") };
    uint32_t crcs[N];
    size_t lens[N] = { 4000, 3000, 5000 };
    int amps[N] = { 6000, 12000, 9000 };
    size_t lead[N] = { 0, 1500, 0 };
    for (int i = 0; i < N; i++) {
        cache_tmp_path(&c, &keys[i], tmp, 600);
        make_wav(tmp, 22050, 1, lens[i], 300 + 100 * i, amps[i], lead[i]);
        CHECK(cache_commit(&c, &keys[i], tmp, 22050, err, 256), "commit seg %d", i);
        CacheEntry e; cache_lookup(&c, &keys[i], &e); crcs[i] = e.crc;
    }
    OpenCtx oc = { &c, keys };
    unsigned char pe[N] = { 0, 1, 1 };                       /* seg 1 ends a paragraph */
    AsmOptions o;
    memset(&o, 0, sizeof o);
    o.rate = 22050; o.sentence_ms = 200; o.paragraph_ms = 500;
    swprintf(outp, 600, L"%ls/out.wav", g_root);
    AsmResult r;

    /* plain concatenation with deterministic pauses */
    FILE *out = plat_fopen(outp, "w+b");
    AsmStatus st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    uint64_t z200 = 22050ULL * 200 / 1000, z500 = 22050ULL * 500 / 1000;
    uint64_t want = 4000 + 3000 + 5000 + z200 /* after seg0: sentence */ + z500 /* after seg1: paragraph */;
    size_t ns;
    int pk = peak_of(outp, &ns);
    CHECK(st == ASM_OK && r.samples == want && ns == want, "assembled length %llu (want %llu, file %zu)", (unsigned long long)r.samples, (unsigned long long)want, ns);
    CHECK(pk > 11000 && pk <= 12000, "no gain applied (peak %d)", pk);

    /* changing pauses changes only the output - the cache is untouched */
    unsigned long f0; unsigned long long b0, b1; unsigned long f1;
    cache_stats(&c, &f0, &b0);
    o.sentence_ms = 0; o.paragraph_ms = 1000;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    want = 4000 + 3000 + 5000 + 0 + 22050;
    CHECK(st == ASM_OK && r.samples == want, "new pauses give new length without touching segments (%llu vs %llu)", (unsigned long long)r.samples, (unsigned long long)want);
    cache_stats(&c, &f1, &b1);
    CHECK(f0 == f1 && b0 == b1, "cache unchanged by assembly");

    /* trimming: segment 1 has 1500 samples of leading silence */
    o.sentence_ms = 0; o.paragraph_ms = 0; o.trim_silence = 1; o.trim_threshold = 300;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    uint64_t margin = 22050 * 5 / 1000;
    CHECK(st == ASM_OK && r.samples < 12000 && r.samples > 12000 - 1500 + margin - 400, "silence trimmed (%llu samples)", (unsigned long long)r.samples);

    /* normalization: peak 12000 -> about 29000 */
    o.trim_silence = 0; o.normalize = 1;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    pk = peak_of(outp, &ns);
    CHECK(st == ASM_OK && pk > 28500 && pk <= 29100 && r.peak > 11000, "normalized peak %d (found %d)", pk, r.peak);

    /* corruption is detected, attributed to the right segment, and not silently assembled */
    cache_segment_path(&c, &keys[2], tmp, 600);
    FILE *cf = plat_fopen(tmp, "r+b");
    fseek(cf, 200, SEEK_SET); unsigned char xb = 0x55; fwrite(&xb, 1, 1, cf); fseek(cf, 201, SEEK_SET); xb = 0xAA; fwrite(&xb, 1, 1, cf); fclose(cf);
    o.normalize = 0;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_BAD_SEGMENT && r.bad_segment == 2, "corrupt segment 2 detected (status %d, segment %d)", st, r.bad_segment);
    /* ... and with normalization the bad segment is found in the first pass too */
    o.normalize = 1;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_BAD_SEGMENT && r.bad_segment == 2, "also found during the normalization pass");
    o.normalize = 0;

    /* repair path: drop the entry, re-commit, assemble again */
    cache_remove(&c, &keys[2]);
    cache_tmp_path(&c, &keys[2], tmp, 600);
    make_wav(tmp, 22050, 1, lens[2], 500, amps[2], lead[2]);
    cache_commit(&c, &keys[2], tmp, 22050, err, 256);
    CacheEntry e2; cache_lookup(&c, &keys[2], &e2); crcs[2] = e2.crc;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_OK, "assembles after regeneration");

    /* a missing segment file */
    Hash128 ghost = K("ghost");
    Hash128 keys2[N] = { keys[0], ghost, keys[2] };
    OpenCtx oc2 = { &c, keys2 };
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, NULL, open_seg, &oc2, NULL, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_BAD_SEGMENT && r.bad_segment == 1, "missing segment reported");

    /* cancellation */
    cancel_after = 2;
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, N, pe, crcs, open_seg, &oc, progress_cb, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_CANCELLED, "cancel honoured");
    cancel_after = 0;

    /* zero segments -> valid empty WAV */
    out = plat_fopen(outp, "w+b");
    st = assemble_wav(out, 0, NULL, NULL, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    CHECK(st == ASM_OK && r.samples == 0, "empty assembly");

    /* header sanity of a normal output */
    out = plat_fopen(outp, "w+b");
    assemble_wav(out, N, pe, crcs, open_seg, &oc, NULL, NULL, &o, &r);
    fclose(out);
    FILE *rf = plat_fopen(outp, "rb");
    WavInfo w;
    CHECK(wav_read_info(rf, &w) && w.rate == 22050 && w.channels == 1 && w.bits == 16 && w.data_bytes == r.samples * 2, "valid RIFF header");
    fclose(rf);
    cache_close(&c);
}

static void test_scenario(void)
{
    /* The whole promise in one test: build, "synthesize" (commit fake audio), edit, and count work. */
    wchar_t err[256], tmp[600];
    Cache c;
    CHECK(cache_open(&c, g_root, err, 256), "open");
    cache_clear(&c, err, 256);
    Speech sp; memset(&sp, 0, sizeof sp); sp.dict = NULL; speech_default_options(&sp.opts);
    SynthParams p = params();
    wchar_t *t0 = book(L"gamma");
    Document d0; make_doc(&d0, t0);
    IR ir;
    ir_build(&ir, &d0, &sp, 0, d0.nsent - 1, &p, NULL);
    int synth = 0;
    for (int i = 0; i < ir.n; i++) {
        if (cache_lookup(&c, &ir.seg[i].key, NULL)) continue;
        cache_tmp_path(&c, &ir.seg[i].key, tmp, 600);
        make_wav(tmp, 22050, 1, 100 + (size_t)i, 400, 3000, 0);
        CHECK(cache_commit(&c, &ir.seg[i].key, tmp, 22050, err, 256), "commit");
        synth++;
    }
    CHECK(synth == 24, "first build synthesizes everything (%d)", synth);

    /* second build with no changes: 0 work */
    int need = 0;
    for (int i = 0; i < ir.n; i++) need += !cache_lookup(&c, &ir.seg[i].key, NULL);
    CHECK(need == 0, "unchanged document needs no synthesis");
    ir_free(&ir);

    /* user fixes one name: only the affected sentences are regenerated */
    PronDict dict; pron_init(&dict);
    pron_add(&dict, L"Machiavelli", L"Mack ee uh vell ee", 0, 0);
    pron_prepare(&dict);
    Speech sp2 = sp; sp2.dict = &dict;
    ir_build(&ir, &d0, &sp2, 0, d0.nsent - 1, &p, NULL);
    need = 0;
    for (int i = 0; i < ir.n; i++) need += !cache_lookup(&c, &ir.seg[i].key, NULL);
    int mentions = 0;
    for (int i = 0; i < d0.nsent; i++) { wchar_t *x = doc_speech_text(&d0, i); mentions += wcsstr(x, L"Machiavelli") != NULL; free(x); }
    CHECK(need == mentions && need < 24, "pronunciation fix regenerates %d of 24 segments (mentions: %d)", need, mentions);
    ir_free(&ir);
    pron_free(&dict);

    /* switching voice: everything */
    SynthParams q = p; wcscpy(q.voice_id, L"other voice");
    ir_build(&ir, &d0, &sp, 0, d0.nsent - 1, &q, NULL);
    need = 0;
    for (int i = 0; i < ir.n; i++) need += !cache_lookup(&c, &ir.seg[i].key, NULL);
    CHECK(need == 24, "voice change regenerates all %d", need);
    ir_free(&ir);
    doc_free(&d0); free(t0);
    cache_close(&c);
}

int main(void)
{
    swprintf(g_root, 256, L"/tmp/ttsforge_cache_test_%d", (int)getpid());
    test_hash();
    test_ir_invalidation();
    test_cache();
    test_assemble();
    test_scenario();
    plat_remove_tree(g_root);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
