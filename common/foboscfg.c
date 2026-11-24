/*
 * Fobos OS — реализация блоба настроек (см. foboscfg.h).
 * Слой: общий для загрузчика/ядра/host-тестов; без зависимостей от железа.
 */
#include "foboscfg.h"

#include "sha256.h"
#include "../kernel/lib/string.h"

#include <stddef.h>

_Static_assert(offsetof(struct foboscfg_user, salt) == 0x24, "foboscfg user ABI");
_Static_assert(sizeof(struct foboscfg_user) <= FOBOSCFG_USER_SIZE, "foboscfg user ABI");

u32 foboscfg_encoded_size(u32 user_count)
{
    if (user_count > FOBOSCFG_MAX_USERS) {
        user_count = FOBOSCFG_MAX_USERS;
    }
    return FOBOSCFG_HEADER_SIZE + user_count * FOBOSCFG_USER_SIZE + FOBOSCFG_TAG_LEN;
}

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u16 get_u16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

/* Копирует строку с NUL и обнулением остатка. Возвращает длину без NUL. */
static u32 copy_str(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src != NULL && src[i] != '\0' && i + 1u < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    for (u32 j = i + 1u; j < cap; j++) {
        dst[j] = '\0';
    }
    return i;
}

int foboscfg_valid_username(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    u32 n = 0;
    while (name[n] != '\0') {
        char c = name[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok) {
            return 0;
        }
        n++;
        if (n >= FOBOSCFG_NAME_MAX) {
            return 0;
        }
    }
    if (n == 0u) {
        return 0;
    }
    if (!(name[0] >= 'a' && name[0] <= 'z') && name[0] != '_') {
        return 0;
    }
    return 1;
}

int foboscfg_valid_hostname(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    u32 n = 0;
    while (name[n] != '\0') {
        char c = name[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return 0;
        }
        n++;
        if (n >= FOBOSCFG_HOSTNAME_MAX) {
            return 0;
        }
    }
    if (n == 0u) {
        return 0;
    }
    return 1;
}

void foboscfg_kdf(const u8 salt[FOBOSCFG_SALT_LEN], const char *pw,
                  u32 rounds, u8 out[FOBOSCFG_HASH_LEN])
{
    struct sha256_ctx c;
    u8 acc[FOBOSCFG_HASH_LEN];

    /* Раунд 0: SHA-256(salt || pw). */
    sha256_init(&c);
    sha256_update(&c, salt, FOBOSCFG_SALT_LEN);
    if (pw != NULL) {
        sha256_update(&c, pw, strlen(pw));
    }
    sha256_final(&c, acc);

    /* Раунды 1..N-1: acc = SHA-256(acc || round_index_be32). */
    for (u32 r = 1u; r < rounds; r++) {
        u8 idx_be[4] = {
            (u8)(r >> 24), (u8)(r >> 16), (u8)(r >> 8), (u8)r
        };
        sha256_init(&c);
        sha256_update(&c, acc, sizeof(acc));
        sha256_update(&c, idx_be, sizeof(idx_be));
        sha256_final(&c, acc);
    }

    for (u32 i = 0; i < FOBOSCFG_HASH_LEN; i++) {
        out[i] = acc[i];
    }
}

err_t foboscfg_encode(const struct fobos_config *cfg, void *buf, u32 cap,
                      u32 *out_size)
{
    if (cfg == NULL || buf == NULL || out_size == NULL) {
        return K_EINVAL;
    }
    if (!foboscfg_valid_hostname(cfg->hostname)) {
        return K_EINVAL;
    }
    if (cfg->user_count > FOBOSCFG_MAX_USERS) {
        return K_EINVAL;
    }
    for (u32 i = 0; i < cfg->user_count; i++) {
        if (!foboscfg_valid_username(cfg->users[i].name)) {
            return K_EINVAL;
        }
        for (u32 j = 0; j < i; j++) {
            if (strcmp(cfg->users[i].name, cfg->users[j].name) == 0) {
                return K_EINVAL; /* дубликаты запрещены */
            }
        }
    }

    u32 size = foboscfg_encoded_size(cfg->user_count);
    if (cap < size) {
        return K_ENOMEM;
    }

    u8 *p = (u8 *)buf;
    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }

    put_u32(p + 0x00u, FOBOSCFG_MAGIC);
    put_u16(p + 0x04u, (u16)FOBOSCFG_VERSION);
    put_u16(p + 0x06u, (u16)FOBOSCFG_HEADER_SIZE);
    put_u32(p + 0x08u, cfg->flags);
    put_u32(p + 0x0Cu, cfg->user_count);
    put_u32(p + 0x10u, 0u);
    put_u32(p + 0x14u, 0u);
    (void)copy_str((char *)(p + 0x18u), FOBOSCFG_HOSTNAME_MAX, cfg->hostname);
    put_u32(p + 0x38u, FOBOSCFG_HEADER_SIZE);
    put_u32(p + 0x3Cu, FOBOSCFG_USER_SIZE);

    for (u32 i = 0; i < cfg->user_count; i++) {
        u8 *u = p + FOBOSCFG_HEADER_SIZE + i * FOBOSCFG_USER_SIZE;
        (void)copy_str((char *)(u + 0x00u), FOBOSCFG_NAME_MAX, cfg->users[i].name);
        u[0x20u] = (cfg->users[i].is_admin != 0u) ? 1u : 0u;
        u[0x21u] = 0u;
        u[0x22u] = 0u;
        u[0x23u] = 0u;
        for (u32 k = 0; k < FOBOSCFG_SALT_LEN; k++) {
            u[0x24u + k] = cfg->users[i].salt[k];
        }
        for (u32 k = 0; k < FOBOSCFG_HASH_LEN; k++) {
            u[0x34u + k] = cfg->users[i].hash[k];
        }
    }

    u32 users_end = FOBOSCFG_HEADER_SIZE + cfg->user_count * FOBOSCFG_USER_SIZE;
    sha256(p, users_end, p + users_end);

    *out_size = size;
    return K_OK;
}

err_t foboscfg_decode(const void *buf, u32 size, struct fobos_config *out)
{
    if (buf == NULL || out == NULL) {
        return K_EINVAL;
    }
    if (size < FOBOSCFG_HEADER_SIZE + FOBOSCFG_TAG_LEN) {
        return K_EINVAL;
    }

    const u8 *p = (const u8 *)buf;
    if (get_u32(p + 0x00u) != FOBOSCFG_MAGIC) {
        return K_EINVAL;
    }
    if (get_u16(p + 0x04u) != FOBOSCFG_VERSION ||
        get_u16(p + 0x06u) != FOBOSCFG_HEADER_SIZE) {
        return K_EINVAL;
    }
    if (get_u32(p + 0x38u) != FOBOSCFG_HEADER_SIZE ||
        get_u32(p + 0x3Cu) != FOBOSCFG_USER_SIZE) {
        return K_EINVAL;
    }

    u32 user_count = get_u32(p + 0x0Cu);
    if (user_count > FOBOSCFG_MAX_USERS) {
        return K_EINVAL;
    }
    u32 users_end = FOBOSCFG_HEADER_SIZE + user_count * FOBOSCFG_USER_SIZE;
    u32 want_size = users_end + FOBOSCFG_TAG_LEN;
    if (size < want_size) {
        return K_EINVAL;
    }

    /* Целостность: тег SHA-256 по телу блога. */
    u8 tag[FOBOSCFG_TAG_LEN];
    sha256(p, users_end, tag);
    if (memcmp(tag, p + users_end, FOBOSCFG_TAG_LEN) != 0) {
        return K_EINVAL;
    }

    struct fobos_config tmp;
    tmp.flags = get_u32(p + 0x08u);
    tmp.user_count = user_count;
    for (u32 i = 0; i < FOBOSCFG_HOSTNAME_MAX; i++) {
        tmp.hostname[i] = (char)p[0x18u + i];
    }
    tmp.hostname[FOBOSCFG_HOSTNAME_MAX - 1u] = '\0';
    if (!foboscfg_valid_hostname(tmp.hostname)) {
        return K_EINVAL;
    }

    for (u32 i = 0; i < user_count; i++) {
        const u8 *u = p + FOBOSCFG_HEADER_SIZE + i * FOBOSCFG_USER_SIZE;
        for (u32 k = 0; k < FOBOSCFG_NAME_MAX; k++) {
            tmp.users[i].name[k] = (char)u[0x00u + k];
        }
        tmp.users[i].name[FOBOSCFG_NAME_MAX - 1u] = '\0';
        if (!foboscfg_valid_username(tmp.users[i].name)) {
            return K_EINVAL;
        }
        tmp.users[i].is_admin = (u[0x20u] == 1u) ? 1u : 0u;
        for (u32 k = 0; k < FOBOSCFG_SALT_LEN; k++) {
            tmp.users[i].salt[k] = u[0x24u + k];
        }
        for (u32 k = 0; k < FOBOSCFG_HASH_LEN; k++) {
            tmp.users[i].hash[k] = u[0x34u + k];
        }
    }
    /* Уникальность имён. */
    for (u32 i = 0; i < user_count; i++) {
        for (u32 j = i + 1u; j < user_count; j++) {
            if (strcmp(tmp.users[i].name, tmp.users[j].name) == 0) {
                return K_EINVAL;
            }
        }
    }

    *out = tmp;
    return K_OK;
}
