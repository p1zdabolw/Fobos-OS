/*
 * Fobos OS — блоб настроек системы (Fobos Setup Config), общий для
 * UEFI-загрузчика (создание в OOBE) и ядра (чтение через FBPI).
 *
 * Формат (little-endian, все поля с фиксированными смещениями):
 *   0x00  u32  magic "FBS2"
 *   0x04  u16  version (FOBOSCFG_VERSION)
 *   0x06  u16  header_size (64)
 *   0x08  u32  flags (FOBOSCFG_FLAG_*)
 *   0x0C  u32  user_count (0..FOBOSCFG_MAX_USERS)
 *   0x10  u32  reserved0 = 0
 *   0x14  u32  reserved1 = 0
 *   0x18  char hostname[32] (NUL-terminated)
 *   0x38  u32  users_offset (= 64)
 *   0x3C  u32  user_size  (= FOBOSCFG_USER_SIZE)
 *   0x40  пользователи: FOBOSCFG_USER_SIZE байт каждый
 *   далее: 32-байтовый тег SHA-256 по всем байтам [0, 64 + user_count*USER_SIZE)
 *
 * Запись пользователя (FOBOSCFG_USER_SIZE = 88 байт):
 *   +0x00 char name[32] (NUL-terminated)
 *   +0x20 u8   is_admin (0/1)
 *   +0x21 u8   reserved[3] = 0
 *   +0x24 u8   salt[16]
 *   +0x34 u8   hash[32] (KDF пароля, foboscfg_kdf)
 *
 * Пароли в блобе НЕ хранятся: только соль и KDF-хеш.
 */
#ifndef FOBOS_COMMON_FOBOSCFG_H
#define FOBOS_COMMON_FOBOSCFG_H

#include <fobos/types.h>
#include <fobos/errors.h>

#define FOBOSCFG_MAGIC        0x32534246u  /* "FBS2" little-endian */
#define FOBOSCFG_VERSION      1u
#define FOBOSCFG_HEADER_SIZE  64u
#define FOBOSCFG_HOSTNAME_MAX 32u
#define FOBOSCFG_NAME_MAX     32u
#define FOBOSCFG_SALT_LEN     16u
#define FOBOSCFG_HASH_LEN     32u
#define FOBOSCFG_MAX_USERS    8u
#define FOBOSCFG_USER_SIZE    88u
#define FOBOSCFG_TAG_LEN      32u

/* Стойкость KDF: количество итераций SHA-256. Значение выбирается как
 * компромисс "задержка логина < 0.5 с даже под TCG" / стойкость; заменяется
 * на PBKDF2-HMAC/Argon2id в FND-3 (см. docs/boot-protocol.md). */
#define FOBOSCFG_KDF_ROUNDS   4096u

#define FOBOSCFG_FLAG_OOBE_DONE 0x1u

struct foboscfg_user {
    char name[FOBOSCFG_NAME_MAX];
    u8   is_admin;
    u8   reserved[3];     /* соответствует паддингу в блобе (смещения как в блобе) */
    u8   salt[FOBOSCFG_SALT_LEN];
    u8   hash[FOBOSCFG_HASH_LEN];
};

struct fobos_config {
    u32  flags;
    char hostname[FOBOSCFG_HOSTNAME_MAX];
    u32  user_count;
    struct foboscfg_user users[FOBOSCFG_MAX_USERS];
};

/* Размер закодированного блога для user_count пользователей (с тегом). */
u32 foboscfg_encoded_size(u32 user_count);

/* Кодирует cfg в buf (вместимость cap). 0 = успех. */
err_t foboscfg_encode(const struct fobos_config *cfg, void *buf, u32 cap,
                      u32 *out_size);

/* Декодирует и ПОЛНОСТЬЮ валидирует блог (magic/version/размер/имена/тег). */
err_t foboscfg_decode(const void *buf, u32 size, struct fobos_config *out);

/* KDF пароля: out = SHA-256^(rounds)(salt[0..16) || pw). */
void foboscfg_kdf(const u8 salt[FOBOSCFG_SALT_LEN], const char *pw,
                  u32 rounds, u8 out[FOBOSCFG_HASH_LEN]);

/* Имя пользователя: 1..31 символа, [a-z0-9_-], первый — буква или '_'. */
int foboscfg_valid_username(const char *name);

/* Имя хоста: 1..31 символ, [a-z0-9-]. */
int foboscfg_valid_hostname(const char *name);

#endif /* FOBOS_COMMON_FOBOSCFG_H */
