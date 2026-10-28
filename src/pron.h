/*
 * pron.h - user pronunciation dictionary (Milestone 4). PURE C.
 *
 * An entry maps written text to the text the speech engine should be given
 * ("Machiavelli" -> "Mack ee uh vell ee"). Replacements are plain text, so they
 * work with any backend and can be tested without a voice. Matching is
 * deterministic, left to right, longest entry first:
 *   - case-insensitive unless match_case is set
 *   - whole-word unless `partial` is set (a word boundary is only required on
 *     a side where the entry itself begins/ends with a letter or digit, so
 *     "C++" and ".NET" work)
 * Regex rules (flag r): `word` is a regular expression (rx.h) tried at each position when no plain entry
 * matches there; the replacement may use $1..$9 / $0 (see rx_expand). Case-insensitive unless `c`.
 * Scopes: an entry may be limited to one chapter ("chapter:<title>") or to a range of sentences anchored by
 * their opening words ("range:..."), see pronscope.c. Scoped entries only apply to a sentence inside their
 * resolved range and win over global entries of equal length. Unresolvable scopes disable the entry.
 *
 * Persistence is a small UTF-8 text file, one entry per line:
 *     flags <TAB> word <TAB> replacement [<TAB> scope]     flags: letters c (case) p (partial) r (regex), or '-'
 * Lines starting with '#' are comments. Files without the scope column still load.
 */
#ifndef PRON_H
#define PRON_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define PRON_CASE 1
#define PRON_PARTIAL 2
#define PRON_REGEX 4

typedef struct PronEntry {
    wchar_t *word, *repl;
    int match_case, partial;
    int regex;
    wchar_t *scope;          /* NULL or "" = global */
    int s_first, s_last;     /* resolved sentence range of a scoped entry (first > last = disabled) */
    void *rx;                /* compiled regex (owned), NULL if invalid or not a regex entry */
} PronEntry;

typedef struct PronDict {
    PronEntry *e; int n, cap;
    int *rxlist; int nrx;  /* indexes of valid regex entries, in order */
    int *order;            /* index: entries grouped by first character, longest first (plain entries only) */
    int bucket[130];       /* start offsets into order[] for 129 buckets (+ end) */
    int index_valid;
} PronDict;

void pron_init(PronDict *d);
void pron_free(PronDict *d);
void pron_clear(PronDict *d);

/* Find an entry with the same word (same case rules). -1 if none. */
int  pron_find(const PronDict *d, const wchar_t *word);
/* Add; returns the new index, -1 if the word already exists (use pron_set), -2 on bad input/out of memory.
 * Tabs and line breaks in word/repl are not allowed (bad input). Empty word is bad input. */
int  pron_add(PronDict *d, const wchar_t *word, const wchar_t *repl, int match_case, int partial);
/* Replace entry i. Returns 1 or 0 (bad input / duplicate of another entry / OOM). */
int  pron_set(PronDict *d, int i, const wchar_t *word, const wchar_t *repl, int match_case, int partial);
void pron_remove(PronDict *d, int i);

/* Build the lookup index (call after edits; matching still works without it, only slower). */
void pron_prepare(PronDict *d);

/* Extended add/set/find with flags (PRON_*) and an optional scope string. Duplicate = same word, regex flag and scope. */
int  pron_add_ex(PronDict *d, const wchar_t *word, const wchar_t *repl, int flags, const wchar_t *scope);
int  pron_set_ex(PronDict *d, int i, const wchar_t *word, const wchar_t *repl, int flags, const wchar_t *scope);
int  pron_find_ex(const PronDict *d, const wchar_t *word, int regex, const wchar_t *scope);
/* Syntax check for a regex rule. 1 ok; 0 with a message in err. */
int  pron_regex_check(const wchar_t *pattern, int match_case, wchar_t *err, size_t errcap);

/* Does an entry match text t[i..n)? On success returns the entry index and sets *matched. -1 otherwise.
 * Global entries only. */
int  pron_match(const PronDict *d, const wchar_t *t, size_t n, size_t i, size_t *matched);
/* Same, also applying scoped entries for document sentence `sent` (-1 = not in a document: global only). */
int  pron_match_ex(const PronDict *d, const wchar_t *t, size_t n, size_t i, int sent, size_t *matched);
/* Replacement text for the match of entry e at t[i..i+matched). Returns its length; if out != NULL it must have
 * room for length + 1. Plain entries return `repl` verbatim. */
size_t pron_replacement(const PronDict *d, int e, const wchar_t *t, size_t n, size_t i, wchar_t *out);

/* Fingerprint of the whole dictionary content (for cache keys later). */
uint64_t pron_signature(const PronDict *d);

/* Serialization. to_utf8 returns malloc'd bytes; from_utf8 replaces the dictionary
 * contents and returns the number of entries loaded (bad lines are skipped, *bad counts them). */
char *pron_to_utf8(const PronDict *d, size_t *len);
int   pron_from_utf8(PronDict *d, const char *s, size_t len, int *bad);

#endif
