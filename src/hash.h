/* hash.h - fast non-cryptographic 128-bit hash for cache keys. PURE C.
 * Text is hashed as UTF-8, so a key is identical for the same text on any platform. */
#ifndef HASH_H
#define HASH_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct Hash128 { uint64_t a, b; } Hash128;
typedef struct HashState { uint64_t a, b, len; } HashState;

void    hash_init(HashState *h);
void    hash_bytes(HashState *h, const void *p, size_t n);
void    hash_u32(HashState *h, uint32_t v);
void    hash_wstr(HashState *h, const wchar_t *s, size_t n);     /* UTF-8 of s, then a 0x00 separator */
Hash128 hash_final(const HashState *h);
int     hash_eq(const Hash128 *x, const Hash128 *y);
void    hash_hex(const Hash128 *h, char out[33]);                /* 32 lower-case hex chars + NUL */
int     hash_from_hex(const char *s, Hash128 *h);                /* 1 on success */
#endif
