/*
 * Fobos OS — реализация SHA-256 (FIPS 180-4, раздел 6.2).
 *
 * Тестовое покрытие: NIST KAT (пустая строка, "abc", 56 байт, двухблочный
 * вектор) — в kselftest и host-тестах (см. kernel/selftest/selftest.c).
 */
#include "sha256.h"

static const u32 K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline u32 rotr(u32 x, u32 n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(struct sha256_ctx *c, const u8 *p)
{
    u32 w[64];
    u32 a, b, cc, d, e, f, g, h;

    for (u32 i = 0; i < 16u; i++) {
        w[i] = ((u32)p[i * 4u] << 24) | ((u32)p[i * 4u + 1u] << 16) |
               ((u32)p[i * 4u + 2u] << 8) | (u32)p[i * 4u + 3u];
    }
    for (u32 i = 16u; i < 64u; i++) {
        u32 s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        u32 s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (u32 i = 0; i < 64u; i++) {
        u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 maj = (a & b) ^ (a & cc) ^ (b & cc);
        u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
    static const u32 H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    for (u32 i = 0; i < 8u; i++) {
        c->h[i] = H0[i];
    }
    c->bits = 0;
    c->buf_len = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, u64 len)
{
    const u8 *p = (const u8 *)data;

    c->bits += len * 8ull;

    if (c->buf_len > 0u) {
        u32 take = (u32)(SHA256_BLOCK_LEN - c->buf_len);
        if (take > len) {
            take = (u32)len;
        }
        for (u32 i = 0; i < take; i++) {
            c->buf[c->buf_len + i] = p[i];
        }
        c->buf_len += take;
        p += take;
        len -= take;
        if (c->buf_len == SHA256_BLOCK_LEN) {
            sha256_block(c, c->buf);
            c->buf_len = 0;
        }
    }

    while (len >= SHA256_BLOCK_LEN) {
        sha256_block(c, p);
        p += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }

    for (u64 i = 0; i < len; i++) {
        c->buf[c->buf_len + i] = p[i];
    }
    c->buf_len += (u32)len;
}

void sha256_final(struct sha256_ctx *c, u8 out[SHA256_DIGEST_LEN])
{
    u64 bit_len = c->bits;
    u8 pad = 0x80u;

    sha256_update(c, &pad, 1u);
    pad = 0x00u;
    while (c->buf_len != 56u) {
        sha256_update(c, &pad, 1u);
    }

    u8 len_be[8];
    for (u32 i = 0; i < 8u; i++) {
        len_be[i] = (u8)(bit_len >> (56u - i * 8u));
    }
    /* Добавляем длину напрямую в буфер (c->buf_len == 56 здесь). */
    for (u32 i = 0; i < 8u; i++) {
        c->buf[56u + i] = len_be[i];
    }
    c->buf_len = 64u;
    sha256_block(c, c->buf);
    c->buf_len = 0;

    for (u32 i = 0; i < 8u; i++) {
        out[i * 4u]      = (u8)(c->h[i] >> 24);
        out[i * 4u + 1u] = (u8)(c->h[i] >> 16);
        out[i * 4u + 2u] = (u8)(c->h[i] >> 8);
        out[i * 4u + 3u] = (u8)(c->h[i]);
    }
}

void sha256(const void *data, u64 len, u8 out[SHA256_DIGEST_LEN])
{
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
