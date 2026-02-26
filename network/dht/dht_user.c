/*
 * User-provided implementations for jech/dht.
 * - dht_sendto: wrap sendto()
 * - dht_blacklisted: no blacklist (return 0)
 * - dht_hash: SHA-1 of concatenated inputs (v1||v2||v3)
 * - dht_random_bytes: libsodium randombytes_buf
 *
 * SHA-1 is a minimal C implementation (FIPS 180-1), used only for
 * DHT token generation as required by the BitTorrent DHT protocol.
 */

#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sodium.h>

#include "dht.h"

/* ========== dht_sendto ========== */
int dht_sendto(int sockfd, const void *buf, int len, int flags,
               const struct sockaddr *to, int tolen)
{
    return (int)sendto(sockfd, buf, (size_t)len, flags, to, (socklen_t)tolen);
}

/* ========== dht_blacklisted ========== */
int dht_blacklisted(const struct sockaddr *sa, int salen)
{
    (void)sa;
    (void)salen;
    return 0;
}

/* ========== Minimal SHA-1 for dht_hash ========== */
#define SHA1_DIGEST_SIZE 20

static const unsigned int sha1_k[] = {
    0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u
};

static unsigned int sha1_rol32(unsigned int x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_process_block(unsigned int state[5], const unsigned char block[64])
{
    unsigned int w[80];
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    int t;

    for (t = 0; t < 16; t++)
        w[t] = ((unsigned int)block[t*4] << 24) | ((unsigned int)block[t*4+1] << 16)
             | ((unsigned int)block[t*4+2] << 8) | (unsigned int)block[t*4+3];
    for (t = 16; t < 80; t++)
        w[t] = sha1_rol32(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);

    for (t = 0; t < 80; t++) {
        unsigned int f, k, tmp;
        if (t < 20) {
            f = (b & c) | ((~b) & d);
            k = sha1_k[0];
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = sha1_k[1];
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = sha1_k[2];
        } else {
            f = b ^ c ^ d;
            k = sha1_k[3];
        }
        tmp = sha1_rol32(a, 5) + f + e + k + w[t];
        e = d; d = c; c = sha1_rol32(b, 30); b = a; a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

/* Hash one contiguous buffer of length total_len. */
static void sha1_buffer(unsigned char *out, const unsigned char *data, size_t total_len)
{
    unsigned int state[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    unsigned char block[64];
    size_t i;
    int n;

    for (i = 0; i + 64 <= total_len; i += 64)
        sha1_process_block(state, data + i);

    n = (int)(total_len - i);
    memcpy(block, data + i, (size_t)n);
    block[n++] = 0x80;
    if (n > 56) {
        memset(block + n, 0, 64 - n);
        sha1_process_block(state, block);
        memset(block, 0, 56);
    } else {
        memset(block + n, 0, 56 - n);
    }
    /* Append length in bits (big-endian) in last 8 bytes. */
    {
        unsigned long long bits = total_len * 8ULL;
        block[56] = (unsigned char)(bits >> 56);
        block[57] = (unsigned char)(bits >> 48);
        block[58] = (unsigned char)(bits >> 40);
        block[59] = (unsigned char)(bits >> 32);
        block[60] = (unsigned char)(bits >> 24);
        block[61] = (unsigned char)(bits >> 16);
        block[62] = (unsigned char)(bits >> 8);
        block[63] = (unsigned char)bits;
    }
    sha1_process_block(state, block);

    for (i = 0; i < 5; i++) {
        out[i*4 + 0] = (unsigned char)(state[i] >> 24);
        out[i*4 + 1] = (unsigned char)(state[i] >> 16);
        out[i*4 + 2] = (unsigned char)(state[i] >> 8);
        out[i*4 + 3] = (unsigned char)(state[i]);
    }
}

/* ========== dht_hash ========== */
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3)
{
    size_t total = (size_t)len1 + (size_t)len2 + (size_t)len3;
    unsigned char *buf;
    unsigned char digest[SHA1_DIGEST_SIZE];

    if (hash_size <= 0)
        return;
    if (total == 0) {
        sha1_buffer(digest, (const unsigned char *)"", 0);
        memcpy(hash_return, digest, (size_t)(hash_size < SHA1_DIGEST_SIZE ? hash_size : SHA1_DIGEST_SIZE));
        return;
    }

    buf = (unsigned char *)malloc(total);
    if (!buf) {
        memset(hash_return, 0, (size_t)hash_size);
        return;
    }
    memcpy(buf, v1, (size_t)len1);
    memcpy(buf + len1, v2, (size_t)len2);
    memcpy(buf + len1 + len2, v3, (size_t)len3);
    sha1_buffer(digest, buf, total);
    free(buf);
    memcpy(hash_return, digest, (size_t)(hash_size < SHA1_DIGEST_SIZE ? hash_size : SHA1_DIGEST_SIZE));
}

/* ========== dht_random_bytes ========== */
int dht_random_bytes(void *buf, size_t size)
{
    randombytes_buf(buf, size);
    return 0;
}
