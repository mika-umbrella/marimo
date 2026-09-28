/* md5.h — RFC 1321 MD5 (public domain reference implementation style).
 * Used for Last.fm API signatures. */
#ifndef MIKA_MD5_H
#define MIKA_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t bits;
    unsigned char in[64];
    int hashi;
} MD5_CTX;

void md5_init(MD5_CTX *ctx);
void md5_update(MD5_CTX *ctx, const unsigned char *buf, size_t len);
void md5_final(MD5_CTX *ctx, unsigned char digest[16]);
/* convenience: hex digest of a string */
void md5_hex(const char *data, size_t len, char out[33]);

#endif
