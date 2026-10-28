#include "plat.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

FILE *plat_fopen(const wchar_t *path, const char *mode)
{
    wchar_t m[8];
    size_t i = 0;
    for (; mode[i] && i < 7; i++) m[i] = (wchar_t)mode[i];
    m[i] = 0;
    return _wfopen(path, m);
}

int plat_mkdir_p(const wchar_t *path)
{
    wchar_t tmp[1024];
    size_t n = wcslen(path);
    if (n == 0 || n >= 1024) return 0;
    wcscpy(tmp, path);
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'/' || tmp[i] == 0) {
            wchar_t c = tmp[i];
            tmp[i] = 0;
            if (!(i == 2 && tmp[1] == L':') && !CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
            tmp[i] = c;
        }
    }
    return 1;
}

int plat_rename_replace(const wchar_t *from, const wchar_t *to)
{
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int plat_remove(const wchar_t *path)
{
    if (DeleteFileW(path)) return 1;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

int plat_file_size(const wchar_t *path, unsigned long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 1;
}

static void walk(const wchar_t *dir, unsigned long *files, unsigned long long *bytes, int remove_all)
{
    wchar_t pat[1100];
    _snwprintf(pat, 1100, L"%ls\\*", dir);
    pat[1099] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t full[1100];
        _snwprintf(full, 1100, L"%ls\\%ls", dir, fd.cFileName);
        full[1099] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk(full, files, bytes, remove_all);
            if (remove_all) RemoveDirectoryW(full);
        } else {
            if (files) (*files)++;
            if (bytes) *bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (remove_all) DeleteFileW(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void plat_tree_stats(const wchar_t *root, unsigned long *files, unsigned long long *bytes)
{
    *files = 0; *bytes = 0;
    walk(root, files, bytes, 0);
}

int plat_remove_tree(const wchar_t *root)
{
    walk(root, NULL, NULL, 1);
    RemoveDirectoryW(root);
    return 1;
}

#else  /* POSIX: used for the host-side unit tests */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "utf8.h"

static char *u8(const wchar_t *w)
{
    size_t n = wcslen(w), b = utf8_from_wide(w, n, NULL);
    char *s = (char *)malloc(b + 1);
    if (s) { utf8_from_wide(w, n, s); s[b] = 0; }
    return s;
}

FILE *plat_fopen(const wchar_t *path, const char *mode) { char *p = u8(path); FILE *f = p ? fopen(p, mode) : NULL; free(p); return f; }

int plat_mkdir_p(const wchar_t *path)
{
    char *p = u8(path);
    if (!p) return 0;
    for (char *c = p + 1; ; c++) {
        if (*c == '/' || *c == 0) {
            char save = *c;
            *c = 0;
            if (mkdir(p, 0777) != 0) { struct stat st; if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) { free(p); return 0; } }
            *c = save;
            if (!save) break;
        }
    }
    free(p);
    return 1;
}

int plat_rename_replace(const wchar_t *from, const wchar_t *to)
{
    char *a = u8(from), *b = u8(to);
    int ok = a && b && rename(a, b) == 0;
    free(a); free(b);
    return ok;
}

int plat_remove(const wchar_t *path) { char *p = u8(path); int ok = p && (unlink(p) == 0 || access(p, F_OK) != 0); free(p); return ok; }

int plat_file_size(const wchar_t *path, unsigned long long *size)
{
    char *p = u8(path);
    struct stat st;
    int ok = p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
    if (ok) *size = (unsigned long long)st.st_size;
    free(p);
    return ok;
}

static void walk(const char *dir, unsigned long *files, unsigned long long *bytes, int remove_all)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(full, files, bytes, remove_all); if (remove_all) rmdir(full); }
        else { if (files) (*files)++; if (bytes) *bytes += (unsigned long long)st.st_size; if (remove_all) unlink(full); }
    }
    closedir(d);
}

void plat_tree_stats(const wchar_t *root, unsigned long *files, unsigned long long *bytes)
{
    *files = 0; *bytes = 0;
    char *p = u8(root);
    if (p) walk(p, files, bytes, 0);
    free(p);
}

int plat_remove_tree(const wchar_t *root)
{
    char *p = u8(root);
    if (p) { walk(p, NULL, NULL, 1); rmdir(p); }
    free(p);
    return 1;
}
#endif
