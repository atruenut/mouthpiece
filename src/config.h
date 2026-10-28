/*
 * config.h - settings shared by the GUI (Mouthpiece.exe) and the command line (MouthpieceCLI.exe).
 * PURE C: only the key names and defaults, so both programs read and write the
 * same [Cleanup] / [SpeechRules] / [Audio] sections of settings.txt and book.ttsp.
 */
#ifndef CONFIG_H
#define CONFIG_H
#include "ini.h"
#include "preproc.h"
#include "speech.h"

typedef struct GenSettings {
    int sentence_ms, paragraph_ms, chapter_ms;   /* deterministic pauses (assembly only) */
    int scale_pauses;                   /* scale pauses with speech rate */
    int trim_silence, normalize;
    int chunk_sentences;                /* sentences per synthesis chunk (speech-affecting: part of the cache key) */
    int per_chapter;                    /* one WAV per chapter (into a folder) instead of a single WAV */
} GenSettings;

void gen_default_settings(GenSettings *s);
/* Defaults first, then whatever the ini holds. Any pointer may be NULL. */
void config_options_from_ini(const Ini *d, PreprocOptions *p, SpeechOptions *s, GenSettings *g);
void config_options_to_ini(Ini *d, const PreprocOptions *p, const SpeechOptions *s, const GenSettings *g);
/* Pause lengths scaled with the speech rate when scale_pauses is on (rate -10..10 -> x2 .. x0.5). */
double config_pause_scale(const GenSettings *s, int rate);

/* Voice profiles: named presets of voice + rate + volume, kept in settings.txt (shared by the GUI and the CLI). */
typedef struct VoiceProfile { wchar_t name[64]; wchar_t voice_id[256]; int rate, volume; } VoiceProfile;
typedef struct ProfileList { VoiceProfile *p; int n; } ProfileList;
void profiles_free(ProfileList *l);
void profiles_from_ini(ProfileList *l, const Ini *d);
void profiles_to_ini(const ProfileList *l, Ini *d);
int  profiles_find(const ProfileList *l, const wchar_t *name);          /* case-insensitive; -1 if none */
int  profiles_set(ProfileList *l, const wchar_t *name, const wchar_t *voice_id, int rate, int volume);   /* add or replace; 1 ok */
void profiles_remove(ProfileList *l, int i);
#endif
