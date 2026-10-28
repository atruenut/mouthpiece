/* util.h - small helpers shared by the UI and the engine. */
#ifndef UTIL_H
#define UTIL_H
#include <windows.h>

/* "<context>: <system message> (0x........)" into buf. */
void util_format_hresult(wchar_t *buf, size_t cap, const wchar_t *context, HRESULT hr);
void util_format_win32(wchar_t *buf, size_t cap, const wchar_t *context, DWORD code);

/* Read a text file (UTF-8 / UTF-8+BOM / UTF-16LE / fallback ANSI) into a
 * newly malloc'd, NUL-terminated wide string with CRLF line endings (what an
 * EDIT control expects). Caller frees with free(). */
int util_load_text_file(const wchar_t *path, wchar_t **out_text,
                        wchar_t *err, size_t errcap);

/* Whole-file helpers. util_read_file: malloc'd bytes (+4 zero bytes of slack). Returns 0 and fills err on
 * failure; *missing is set to 1 when the file simply does not exist (not an error worth reporting).
 * util_write_file writes atomically (temp file + replace) so a crash never leaves a half-written file. */
int util_read_file(const wchar_t *path, unsigned char **buf, size_t *len, int *missing, wchar_t *err, size_t errcap);
int util_write_file(const wchar_t *path, const void *data, size_t len, wchar_t *err, size_t errcap);
#endif
