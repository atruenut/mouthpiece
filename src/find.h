/* find.h - text search and replace (plain, whole-word or regular expression). PURE C. */
#ifndef FIND_H
#define FIND_H
#include <stddef.h>
#include <wchar.h>

typedef struct FindOpts { int match_case, whole_word, regex; } FindOpts;

/* Search for `needle` in text[0..n). dir > 0: first match starting at or after `from`; dir < 0: last match
 * starting before `from`. Returns 1 found (start/len set), 0 not found, -1 invalid regex (message in err). */
int find_next(const wchar_t *text, size_t n, const wchar_t *needle, const FindOpts *o, size_t from, int dir,
              size_t *start, size_t *len, wchar_t *err, size_t errcap);

/* Replace every match. Returns a malloc'd NUL-terminated string (NULL on error / OOM; err set for a bad regex);
 * *count = number of replacements. Regex replacements may use $1..$9, $0, $$. Empty matches are skipped. */
wchar_t *find_replace_all(const wchar_t *text, size_t n, const wchar_t *needle, const wchar_t *repl, const FindOpts *o,
                          size_t *count, wchar_t *err, size_t errcap);

/* Expansion of the replacement for the match at text[start..start+len) (what Replace should insert). malloc'd. */
wchar_t *find_replacement_for(const wchar_t *text, size_t n, const wchar_t *needle, const wchar_t *repl, const FindOpts *o,
                              size_t start, size_t len);
#endif
