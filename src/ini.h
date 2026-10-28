/*
 * ini.h - tiny UTF-8 key/value file format ("[Section]" / "Key=Value"). PURE C.
 *
 * Used for the project file (.ttsp) and the application settings. Chosen over the Win32
 * GetPrivateProfile* API because that one is ANSI unless the file is UTF-16, which would
 * corrupt non-ASCII titles, paths and snippets. Values escape backslash, CR and LF
 * (\\ \r \n); the file is plain text that can be read and diffed by hand.
 * Sections and keys keep their first-appearance order.
 */
#ifndef INI_H
#define INI_H
#include <stddef.h>
#include <wchar.h>

typedef struct IniKV { wchar_t *section, *key, *value; } IniKV;
typedef struct Ini { IniKV *kv; int n, cap; } Ini;

void ini_init(Ini *d);
void ini_free(Ini *d);
int  ini_set(Ini *d, const wchar_t *section, const wchar_t *key, const wchar_t *value);   /* replace or add; 1 ok */
int  ini_set_int(Ini *d, const wchar_t *section, const wchar_t *key, long v);
const wchar_t *ini_get(const Ini *d, const wchar_t *section, const wchar_t *key);          /* NULL if absent */
long ini_get_int(const Ini *d, const wchar_t *section, const wchar_t *key, long def);
void ini_remove_section(Ini *d, const wchar_t *section);
char *ini_to_utf8(const Ini *d, size_t *len);                    /* malloc'd */
int   ini_from_utf8(Ini *d, const char *s, size_t len);          /* replaces contents; returns entries read */
#endif
