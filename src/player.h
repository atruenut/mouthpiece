/*
 * player.h - sentence sequencer. Feeds a Document's sentences to a TTSEngine a
 * few at a time (so speech is gapless but a Stop/Next never has hours of text
 * queued) and tells the UI which sentence is being spoken right now.
 *
 * Not thread-safe; lives on the thread that owns the engine (the UI thread).
 * The Document must outlive playback and must not be rebuilt while running
 * (the UI stops playback when the text is edited).
 */
#ifndef PLAYER_H
#define PLAYER_H
#include "doc.h"
#include "tts_engine.h"
#include "speech.h"

#define PLAYER_LOOKAHEAD 3     /* current sentence + up to 2 queued behind it */

typedef enum { PLAYER_IDLE, PLAYER_RUNNING, PLAYER_FINISHED, PLAYER_ERROR } PlayerState;

typedef struct Player {
    TTSEngine *engine;                 /* borrowed */
    const Document *doc;               /* borrowed */
    const Speech *speech;              /* borrowed: dictionary + normalization applied to each sentence */
    int active, first, last, next, current, done_ticks;
    double first_frac;                 /* resume the first sentence after this fraction of it (0 = from its start) */
    struct { unsigned long stream; int sentence; size_t off, full; } q[PLAYER_LOOKAHEAD];   /* off: where in the full text this stream starts */
    int qn;
    double progress;                   /* how far into the spoken sentence the voice is (0..1), valid when progress_ok */
    int progress_ok;                   /* the voice reports word positions and has reached its first word of this sentence */
    int progress_sent;
} Player;

void      player_init(Player *p, TTSEngine *engine);
/* Speak sentences first..last (inclusive), replacing anything already playing. */
TTSStatus player_start(Player *p, const Document *doc, const Speech *speech, int first, int last,
                       wchar_t *err, size_t errcap);
/* Call every ~100 ms. Refills the queue and reports the sentence being spoken. */
PlayerState player_poll(Player *p, int *current_sentence, wchar_t *err, size_t errcap);
/* Like player_start, but the first sentence resumes after `frac` (0..1) of it. */
TTSStatus player_start_at(Player *p, const Document *doc, const Speech *speech, int first, int last, double frac,
                          wchar_t *err, size_t errcap);
void      player_stop(Player *p);
/* Real position of the voice inside the sentence now spoken (from the voice's own word events), 0..1 of the sentence. Returns 0
 * if the voice does not report it (then callers fall back on estimates). Updated by player_poll. */
int       player_progress(const Player *p, int *sentence, double *frac);

#endif
