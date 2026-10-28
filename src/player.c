#include "player.h"
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include "navmath.h"

void player_init(Player *p, TTSEngine *engine)
{
    p->engine = engine;
    p->doc = NULL;
    p->active = 0;
    p->qn = 0;
    p->progress_ok = 0;
    p->progress_sent = -1;
}

void player_stop(Player *p)
{
    tts_stop(p->engine);
    p->active = 0;
    p->qn = 0;
    p->progress_ok = 0;
}

/* Keep PLAYER_LOOKAHEAD sentences in the engine's queue. */
static TTSStatus refill(Player *p, wchar_t *err, size_t errcap)
{
    while (p->qn < PLAYER_LOOKAHEAD && p->next <= p->last) {
        int s = p->next++;
        wchar_t *text = speech_sentence_text(p->speech, p->doc, s);
        if (!text) { _snwprintf(err, errcap, L"Out of memory."); return TTS_FAIL; }
        if (text[0] == 0) { free(text); continue; }       /* e.g. only zero-width characters */
        const wchar_t *spoken = text;
        if (s == p->first && p->first_frac > 0) {
            size_t off = text_resume_offset(text, wcslen(text), p->first_frac);
            p->first_frac = 0;
            if (off >= wcslen(text)) { free(text); continue; }     /* it was practically over: start with the next one */
            spoken = text + off;
        }
        unsigned long id = 0;
        size_t full = wcslen(text), off = (size_t)(spoken - text);
        TTSStatus st = tts_enqueue(p->engine, spoken, &id, err, errcap);
        free(text);
        if (st != TTS_SUCCESS) return st;
        p->q[p->qn].stream = id;
        p->q[p->qn].sentence = s;
        p->q[p->qn].off = off;
        p->q[p->qn].full = full;
        p->qn++;
    }
    return TTS_SUCCESS;
}

TTSStatus player_start(Player *p, const Document *doc, const Speech *speech, int first, int last,
                       wchar_t *err, size_t errcap)
{
    return player_start_at(p, doc, speech, first, last, 0.0, err, errcap);
}

TTSStatus player_start_at(Player *p, const Document *doc, const Speech *speech, int first, int last, double frac,
                          wchar_t *err, size_t errcap)
{
    player_stop(p);
    p->first_frac = frac;
    if (first < 0) first = 0;
    if (last >= doc->nsent) last = doc->nsent - 1;
    if (doc->nsent == 0 || first > last) { _snwprintf(err, errcap, L"Nothing to speak."); return TTS_FAIL; }

    p->doc = doc;
    p->speech = speech;
    p->first = first; p->last = last;
    p->next = first; p->current = first;
    p->done_ticks = 0;
    p->active = 1;
    if (refill(p, err, errcap) != TTS_SUCCESS) { player_stop(p); return TTS_FAIL; }
    if (p->qn == 0) { player_stop(p); _snwprintf(err, errcap, L"Nothing to speak."); return TTS_FAIL; }
    return TTS_SUCCESS;
}

PlayerState player_poll(Player *p, int *current_sentence, wchar_t *err, size_t errcap)
{
    if (!p->active) return PLAYER_IDLE;

    unsigned long cur = 0;
    int done = 0;
    if (tts_queue_status(p->engine, &cur, &done, err, errcap) != TTS_SUCCESS) {
        player_stop(p);
        return PLAYER_ERROR;
    }

    /* Drop sentences whose streams are behind the one now speaking. */
    if (cur) {
        int w = 0;
        for (int r = 0; r < p->qn; r++)
            if (p->q[r].stream >= cur) p->q[w++] = p->q[r];
        p->qn = w;
        if (p->qn && p->q[0].stream == cur) {
            p->current = p->q[0].sentence;
            if (p->progress_sent != p->current) { p->progress_sent = p->current; p->progress_ok = 0; }
            unsigned long wpos = 0;
            if (p->q[0].full && tts_word_position(p->engine, &wpos)) {
                /* it only counts once the voice has actually reached a word: a voice that never reports one stays on estimates */
                if (wpos > 0) p->progress_ok = 1;
                if (p->progress_ok) {
                    double f = ((double)p->q[0].off + (double)wpos) / (double)p->q[0].full;
                    p->progress = f < 0 ? 0 : f > 1 ? 1 : f;
                }
            }
        }
    }

    if (refill(p, err, errcap) != TTS_SUCCESS) { player_stop(p); return PLAYER_ERROR; }

    /* "done" must hold on two consecutive polls with nothing left to enqueue,
     * so a not-yet-started stream can't be mistaken for the end of playback. */
    if (done && p->next > p->last) {
        if (++p->done_ticks >= 2) {
            p->active = 0;
            p->qn = 0;
            p->current = p->last;
            *current_sentence = p->current;
            return PLAYER_FINISHED;
        }
    } else {
        p->done_ticks = 0;
    }
    *current_sentence = p->current;
    return PLAYER_RUNNING;
}

int player_progress(const Player *p, int *sentence, double *frac)
{
    if (!p->active || !p->progress_ok) return 0;
    *sentence = p->progress_sent;
    *frac = p->progress;
    return 1;
}
