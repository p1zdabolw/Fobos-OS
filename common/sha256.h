/*
 * Fobos OS — SHA-256 (FIPS 180-4).
 *
 * Общий модуль: используется загрузчиком (OOBE — хеширование паролей),
 * ядром (проверка паролей, целостность блога настроек) и host-тестами.
 * Свободна от зависимостей, кроме fobos/types.h; работает в freestanding.
 *
 * Реализация посимвольная (без SIMD — сборка ядра -mgeneral-regs-only).
 */
#ifndef FOBOS_COMMON_SHA256_H
#define FOBOS_COMMON_SHA256_H

#include <fobos/types.h>

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u

struct sha256_ctx {
    u32 h[8];
    u64 bits;        /* всего байт на входе (mod 2^64) */
    u8  buf[SHA256_BLOCK_LEN];
    u32 buf_len;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, u64 len);
void sha256_final(struct sha256_ctx *c, u8 out[SHA256_DIGEST_LEN]);

/* Одноразовый вызов: hash(data[0..len)) -> out. */
void sha256(const void *data, u64 len, u8 out[SHA256_DIGEST_LEN]);

#endif /* FOBOS_COMMON_SHA256_H */
