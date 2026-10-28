/* plat.h - the few file-system operations the cache needs, behind one tiny interface.
 * Windows: wide Win32 calls. Elsewhere: POSIX with UTF-8 paths (used by the unit tests). */
#ifndef PLAT_H
#define PLAT_H
#include <stdio.h>
#include <wchar.h>
#ifndef _WIN32
#define _snwprintf swprintf          /* same signature; lets the pure-C core run on any host */
#endif

FILE *plat_fopen(const wchar_t *path, const char *mode);
int   plat_mkdir_p(const wchar_t *path);                              /* 1 ok (also if it exists) */
int   plat_rename_replace(const wchar_t *from, const wchar_t *to);    /* atomic replace, 1 ok */
int   plat_remove(const wchar_t *path);                               /* 1 if removed or absent */
int   plat_file_size(const wchar_t *path, unsigned long long *size);  /* 1 if the file exists */
void  plat_tree_stats(const wchar_t *root, unsigned long *files, unsigned long long *bytes);
int   plat_remove_tree(const wchar_t *root);                          /* delete everything below root (and root) */
#endif
