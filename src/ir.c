#include "ir.h"
#include <stdlib.h>
#include <string.h>

Hash128 ir_key(const wchar_t *text, size_t n, const SynthParams *p)
{
    HashState h;
    hash_init(&h);
    hash_wstr(&h, L"tts-forge-seg-v1", 16);                 /* key schema version */
    hash_wstr(&h, text, n);
    hash_wstr(&h, p->voice_id, wcslen(p->voice_id));
    hash_u32(&h, (uint32_t)p->rate);
    hash_u32(&h, (uint32_t)p->volume);
    hash_u32(&h, (uint32_t)p->pitch);
    hash_wstr(&h, p->engine_id, wcslen(p->engine_id));
    hash_u32(&h, p->wav_rate);
    return hash_final(&h);
}

void ir_free(IR *ir)
{
    for (int i = 0; i < ir->n; i++) free(ir->seg[i].text);
    free(ir->seg);
    memset(ir, 0, sizeof *ir);
}

static int push(IR *ir, const IRSegment *s)
{
    if (ir->n == ir->cap) {
        int nc = ir->cap ? ir->cap * 2 : 256;
        IRSegment *p = (IRSegment *)realloc(ir->seg, (size_t)nc * sizeof *p);
        if (!p) return 0;
        ir->seg = p; ir->cap = nc;
    }
    ir->seg[ir->n] = *s;
    ir->seg[ir->n].id = ir->n;
    ir->n++;
    return 1;
}

int ir_build(IR *ir, const Document *doc, const Speech *sp, int first, int last, const SynthParams *p, const ChapterList *chs)
{
    memset(ir, 0, sizeof *ir);
    int per = p->chunk_sentences < 1 ? 1 : (p->chunk_sentences > 8 ? 8 : p->chunk_sentences);
    size_t maxc = p->chunk_max_chars > 0 ? (size_t)p->chunk_max_chars : 700;
    if (first < 0) first = 0;
    if (last >= doc->nsent) last = doc->nsent - 1;

    wchar_t *cur = NULL;                                     /* chunk text being built */
    size_t cur_len = 0, cur_cap = 0;
    int cur_n = 0, cur_first = 0, cur_last = 0, cur_para = -1;
    size_t cur_src0 = 0, cur_src1 = 0;

    for (int s = first; s <= last + 1; s++) {
        wchar_t *t = NULL;
        size_t tl = 0;
        int para = -1;
        if (s <= last) {
            t = speech_sentence_text(sp, doc, s);
            if (!t) { free(cur); ir_free(ir); return 0; }
            tl = wcslen(t);
            if (tl == 0) { free(t); continue; }                 /* nothing audible */
            para = doc->sents[s].para;
        }
        int flush = cur_n > 0 && (s > last || para != cur_para || cur_n >= per || cur_len + 1 + tl > maxc);
        if (flush) {
            IRSegment seg;
            memset(&seg, 0, sizeof seg);
            seg.first_sent = cur_first; seg.last_sent = cur_last; seg.para = cur_para;
            seg.src_start = cur_src0; seg.src_len = cur_src1 - cur_src0;
            seg.text = cur;
            seg.key = ir_key(cur, cur_len, p);
            if (!push(ir, &seg)) { free(cur); free(t); ir_free(ir); return 0; }
            cur = NULL; cur_len = cur_cap = 0; cur_n = 0;
        }
        if (s > last) break;
        if (cur_n == 0) {
            cur_first = s; cur_para = para; cur_src0 = doc->sents[s].start;
            cur_len = 0;
        }
        if (cur_len + tl + 2 > cur_cap) {
            cur_cap = (cur_len + tl + 2) * 2;
            wchar_t *np = (wchar_t *)realloc(cur, cur_cap * sizeof(wchar_t));
            if (!np) { free(cur); free(t); ir_free(ir); return 0; }
            cur = np;
        }
        if (cur_n > 0) cur[cur_len++] = L' ';
        memcpy(cur + cur_len, t, tl * sizeof(wchar_t));
        cur_len += tl;
        cur[cur_len] = 0;
        cur_last = s;
        cur_src1 = doc->sents[s].start + doc->sents[s].len;
        cur_n++;
        free(t);
    }
    for (int i = 0; i < ir->n; i++) {
        ir->seg[i].chapter = chs ? chapter_of_sentence(chs, ir->seg[i].first_sent) : -1;
    }
    for (int i = 0; i < ir->n; i++) {
        ir->seg[i].para_end = (i + 1 >= ir->n) || ir->seg[i + 1].para != ir->seg[i].para;
        ir->seg[i].chapter_end = (i + 1 >= ir->n) || ir->seg[i + 1].chapter != ir->seg[i].chapter;
    }
    return 1;
}

void ir_pause_kinds(const IR *ir, unsigned char *kinds)
{
    for (int i = 0; i < ir->n; i++)
        kinds[i] = (unsigned char)(ir->seg[i].chapter_end ? 2 : ir->seg[i].para_end ? 1 : 0);
}
