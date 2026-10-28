#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "textutil.h"
#include "plat.h"

void gen_default_settings(GenSettings *s)
{
    s->sentence_ms = 250; s->paragraph_ms = 700; s->chapter_ms = 1500; s->scale_pauses = 0;
    s->trim_silence = 1; s->normalize = 0; s->chunk_sentences = 1; s->per_chapter = 0;
}

static const wchar_t *const CLEAN[] = { L"Enabled", L"RepeatedLines", L"PageNumbers", L"JoinLines",
    L"FixHyphens", L"NormalizeWhitespace", L"NormalizeUnicode", L"RemoveUrls", L"RemoveCitations",
    L"RemoveFootnotes", L"RemoveTables", L"TablesByRow" };
static const wchar_t *const RULES[] = { L"Abbreviations", L"Numbers", L"Urls", L"Technical" };

static int *clean_f(PreprocOptions *o, int i)
{
    int *f[12] = { &o->enabled, &o->repeated_lines, &o->page_numbers, &o->join_lines, &o->fix_hyphens,
                  &o->normalize_ws, &o->normalize_unicode, &o->remove_urls, &o->remove_citations,
                  &o->remove_footnotes, &o->remove_tables, &o->table_rows };
    return f[i];
}
static int *rule_f(SpeechOptions *o, int i)
{
    int *f[4] = { &o->abbreviations, &o->numbers, &o->urls, &o->technical };
    return f[i];
}

void config_options_from_ini(const Ini *d, PreprocOptions *p, SpeechOptions *s, GenSettings *g)
{
    if (p) {
        preproc_default_options(p);
        for (int i = 0; i < 12; i++) *clean_f(p, i) = ini_get_int(d, L"Cleanup", CLEAN[i], *clean_f(p, i)) != 0;
    }
    if (s) {
        speech_default_options(s);
        for (int i = 0; i < 4; i++) *rule_f(s, i) = ini_get_int(d, L"SpeechRules", RULES[i], *rule_f(s, i)) != 0;
    }
    if (g) {
        gen_default_settings(g);
        g->sentence_ms = (int)ini_get_int(d, L"Audio", L"SentencePauseMs", g->sentence_ms);
        g->paragraph_ms = (int)ini_get_int(d, L"Audio", L"ParagraphPauseMs", g->paragraph_ms);
        g->chapter_ms = (int)ini_get_int(d, L"Audio", L"ChapterPauseMs", g->chapter_ms);
        g->scale_pauses = ini_get_int(d, L"Audio", L"ScalePauses", g->scale_pauses) != 0;
        g->trim_silence = ini_get_int(d, L"Audio", L"TrimSilence", g->trim_silence) != 0;
        g->normalize = ini_get_int(d, L"Audio", L"Normalize", g->normalize) != 0;
        g->chunk_sentences = (int)ini_get_int(d, L"Audio", L"ChunkSentences", g->chunk_sentences);
        g->per_chapter = ini_get_int(d, L"Audio", L"PerChapter", g->per_chapter) != 0;
        if (g->chunk_sentences < 1 || g->chunk_sentences > 8) g->chunk_sentences = 1;
    }
}

void config_options_to_ini(Ini *d, const PreprocOptions *p, const SpeechOptions *s, const GenSettings *g)
{
    if (p) for (int i = 0; i < 12; i++) ini_set_int(d, L"Cleanup", CLEAN[i], *clean_f((PreprocOptions *)p, i) ? 1 : 0);
    if (s) for (int i = 0; i < 4; i++) ini_set_int(d, L"SpeechRules", RULES[i], *rule_f((SpeechOptions *)s, i) ? 1 : 0);
    if (g) {
        ini_set_int(d, L"Audio", L"SentencePauseMs", g->sentence_ms);
        ini_set_int(d, L"Audio", L"ParagraphPauseMs", g->paragraph_ms);
        ini_set_int(d, L"Audio", L"ChapterPauseMs", g->chapter_ms);
        ini_set_int(d, L"Audio", L"ScalePauses", g->scale_pauses);
        ini_set_int(d, L"Audio", L"TrimSilence", g->trim_silence);
        ini_set_int(d, L"Audio", L"Normalize", g->normalize);
        ini_set_int(d, L"Audio", L"ChunkSentences", g->chunk_sentences);
        ini_set_int(d, L"Audio", L"PerChapter", g->per_chapter);
    }
}

double config_pause_scale(const GenSettings *s, int rate)
{
    if (!s->scale_pauses) return 1.0;
    double f = pow(2.0, -rate / 10.0);
    return f < 0.5 ? 0.5 : (f > 2.0 ? 2.0 : f);
}

/* ----------------------------------------------------------------- profiles */

void profiles_free(ProfileList *l) { free(l->p); l->p = NULL; l->n = 0; }

int profiles_find(const ProfileList *l, const wchar_t *name)
{
    for (int i = 0; i < l->n; i++) {
        const wchar_t *a = l->p[i].name, *b = name;
        while (*a && *b && fold_char(*a) == fold_char(*b)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

int profiles_set(ProfileList *l, const wchar_t *name, const wchar_t *voice_id, int rate, int volume)
{
    if (!name || !*name || wcslen(name) >= 64 || wcschr(name, L'\n') || wcschr(name, L'=')) return 0;
    int i = profiles_find(l, name);
    if (i < 0) {
        VoiceProfile *p = (VoiceProfile *)realloc(l->p, (size_t)(l->n + 1) * sizeof *p);
        if (!p) return 0;
        l->p = p;
        i = l->n++;
        memset(&l->p[i], 0, sizeof l->p[i]);
    }
    wcsncpy(l->p[i].name, name, 63);
    wcsncpy(l->p[i].voice_id, voice_id ? voice_id : L"", 255);
    l->p[i].rate = rate < -10 ? -10 : (rate > 10 ? 10 : rate);
    l->p[i].volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    return 1;
}

void profiles_remove(ProfileList *l, int i)
{
    if (i < 0 || i >= l->n) return;
    memmove(&l->p[i], &l->p[i + 1], (size_t)(l->n - i - 1) * sizeof *l->p);
    l->n--;
}

void profiles_to_ini(const ProfileList *l, Ini *d)
{
    ini_set_int(d, L"Profiles", L"Count", l->n);
    for (int i = 0; i < l->n; i++) {
        wchar_t k[24];
        _snwprintf(k, 24, L"Name%d", i + 1);  ini_set(d, L"Profiles", k, l->p[i].name);
        _snwprintf(k, 24, L"Voice%d", i + 1); ini_set(d, L"Profiles", k, l->p[i].voice_id);
        _snwprintf(k, 24, L"Rate%d", i + 1);  ini_set_int(d, L"Profiles", k, l->p[i].rate);
        _snwprintf(k, 24, L"Volume%d", i + 1); ini_set_int(d, L"Profiles", k, l->p[i].volume);
    }
}

void profiles_from_ini(ProfileList *l, const Ini *d)
{
    profiles_free(l);
    long n = ini_get_int(d, L"Profiles", L"Count", 0);
    if (n < 0 || n > 200) n = 0;
    for (long i = 1; i <= n; i++) {
        wchar_t k[24];
        _snwprintf(k, 24, L"Name%ld", i);
        const wchar_t *name = ini_get(d, L"Profiles", k);
        _snwprintf(k, 24, L"Voice%ld", i);
        const wchar_t *voice = ini_get(d, L"Profiles", k);
        _snwprintf(k, 24, L"Rate%ld", i);
        int rate = (int)ini_get_int(d, L"Profiles", k, 0);
        _snwprintf(k, 24, L"Volume%ld", i);
        int vol = (int)ini_get_int(d, L"Profiles", k, 100);
        if (name) profiles_set(l, name, voice, rate, vol);
    }
}
