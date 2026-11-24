/*
 * Fobos OS — реализация RAM-базы пользователей (см. users.h).
 */
#include "users.h"

#include "../lib/log.h"
#include "../lib/string.h"

#ifndef FOBOS_HOST_TEST
#include "../fs/blk.h"
#include "../fs/fat32.h"
#endif

#ifndef FOBOS_HOST_TEST
#include "../lib/io.h"
#include "../drivers/timer.h"
#endif

#include "../../common/sha256.h"

#define USERS_MAX FOBOSCFG_MAX_USERS

static struct user_entry g_users[USERS_MAX];
static u32 g_count;

static u32 g_salt_counter; /* детерминизм на host + доп. развязка в ядре */

/* Соль: SHA-256(tsc || ticks || counter || name) — соль не секретна,
 * но должна быть уникальной на запись. На host rdtsc/PIT отсутствуют,
 * детерминированная соль допустима (тесты). */
static void generate_salt(const char *name, u8 salt[USERS_SALT_LEN])
{
    struct sha256_ctx c;
    u8 buf[32];
    u64 tsc = 0;
    u64 ticks = 0;

#ifndef FOBOS_HOST_TEST
    tsc = rdtsc();
    ticks = timer_get_ticks();
#endif

    sha256_init(&c);
    sha256_update(&c, &tsc, sizeof(tsc));
    sha256_update(&c, &ticks, sizeof(ticks));
    sha256_update(&c, &g_salt_counter, sizeof(g_salt_counter));
    if (name != NULL) {
        sha256_update(&c, name, strlen(name));
    }
    sha256_final(&c, buf);

    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        salt[i] = buf[i];
    }
    g_salt_counter++;
}

void users_reset(void)
{
    for (u32 i = 0; i < USERS_MAX; i++) {
        for (u32 k = 0; k < USERS_NAME_MAX; k++) {
            g_users[i].name[k] = 0;
        }
        g_users[i].is_admin = 0;
        for (u32 k = 0; k < USERS_SALT_LEN; k++) {
            g_users[i].salt[k] = 0;
        }
        for (u32 k = 0; k < USERS_HASH_LEN; k++) {
            g_users[i].hash[k] = 0;
        }
    }
    g_count = 0;
}

static int pw_len_ok(const char *pw)
{
    u32 n = 0;
    if (pw == NULL) {
        return 0;
    }
    while (pw[n] != '\0') {
        n++;
        if (n >= USERS_PW_MAX) {
            return 0;
        }
    }
    return n > 0u;
}

err_t users_add_hashed(const char *name, const u8 salt[USERS_SALT_LEN],
                       const u8 hash[USERS_HASH_LEN], int is_admin)
{
    if (name == NULL || salt == NULL || hash == NULL) {
        return K_EINVAL;
    }
    if (foboscfg_valid_username(name) == 0) {
        return K_EINVAL;
    }
    if (users_find(name) >= 0) {
        return K_EINVAL; /* уже есть */
    }
    if (g_count >= USERS_MAX) {
        return K_ENOMEM;
    }

    struct user_entry *u = &g_users[g_count];
    (void)memcpy(u->name, name, strlen(name) + 1u);
    u->is_admin = (is_admin != 0) ? 1u : 0u;
    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        u->salt[i] = salt[i];
    }
    for (u32 i = 0; i < USERS_HASH_LEN; i++) {
        u->hash[i] = hash[i];
    }
    g_count++;
    return K_OK;
}

err_t users_add(const char *name, const char *password, int is_admin)
{
    if (pw_len_ok(password) == 0) {
        return K_EINVAL;
    }
    if (foboscfg_valid_username(name) == 0 || users_find(name) >= 0 ||
        g_count >= USERS_MAX) {
        return K_EINVAL;
    }

    u8 salt[USERS_SALT_LEN];
    u8 hash[USERS_HASH_LEN];
    generate_salt(name, salt);
    foboscfg_kdf(salt, password, FOBOSCFG_KDF_ROUNDS, hash);

    return users_add_hashed(name, salt, hash, is_admin);
}

err_t users_set_password(const char *name, const char *new_password)
{
    if (pw_len_ok(new_password) == 0) {
        return K_EINVAL;
    }
    int idx = users_find(name);
    if (idx < 0) {
        return K_ENODEV;
    }

    u8 salt[USERS_SALT_LEN];
    u8 hash[USERS_HASH_LEN];
    generate_salt(name, salt);
    foboscfg_kdf(salt, new_password, FOBOSCFG_KDF_ROUNDS, hash);

    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        g_users[idx].salt[i] = salt[i];
    }
    for (u32 i = 0; i < USERS_HASH_LEN; i++) {
        g_users[idx].hash[i] = hash[i];
    }
    return K_OK;
}

err_t users_apply(const struct fobos_config *cfg)
{
    if (cfg == NULL) {
        return K_EINVAL;
    }
    struct user_entry saved[USERS_MAX];
    u32 saved_count = g_count;
    for (u32 i = 0; i < g_count && i < USERS_MAX; i++) {
        saved[i] = g_users[i];
    }

    users_reset();
    for (u32 i = 0; i < cfg->user_count; i++) {
        err_t rc = users_add_hashed(cfg->users[i].name, cfg->users[i].salt,
                                    cfg->users[i].hash, cfg->users[i].is_admin);
        if (rc != K_OK) {
            /* Откат: частично применённая база недопустима. */
            users_reset();
            for (u32 j = 0; j < saved_count && j < USERS_MAX; j++) {
                g_users[j] = saved[j];
            }
            g_count = saved_count;
            return rc;
        }
    }
    return K_OK;
}

err_t users_load_blob(const void *buf, u32 size)
{
    struct fobos_config cfg;
    err_t rc = foboscfg_decode(buf, size, &cfg);
    if (rc != K_OK) {
        return rc;
    }
    return users_apply(&cfg);
}

u32 users_count(void)
{
    return g_count;
}

int users_find(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (u32 i = 0; i < g_count; i++) {
        if (strcmp(g_users[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *users_name(u32 index)
{
    if (index >= g_count) {
        return NULL;
    }
    return g_users[index].name;
}

int users_is_admin(u32 index)
{
    if (index >= g_count) {
        return -1;
    }
    return (g_users[index].is_admin != 0u) ? 1 : 0;
}

/* Constant-time сравнение 32-байтовых хешей. */
static int hash_equal(const u8 a[USERS_HASH_LEN], const u8 b[USERS_HASH_LEN])
{
    u8 diff = 0;
    for (u32 i = 0; i < USERS_HASH_LEN; i++) {
        diff |= (u8)(a[i] ^ b[i]);
    }
    return diff == 0u;
}

void users_snapshot_save(struct users_snapshot *out)
{
    if (out == NULL) {
        return;
    }
    for (u32 i = 0; i < g_count && i < USERS_MAX; i++) {
        out->e[i] = g_users[i];
    }
    out->count = g_count;
}

void users_snapshot_restore(const struct users_snapshot *s)
{
    if (s == NULL) {
        return;
    }
    users_reset();
    for (u32 i = 0; i < s->count && i < USERS_MAX; i++) {
        g_users[i] = s->e[i];
    }
    g_count = s->count;
}

int users_verify(const char *name, const char *password)
{
    int idx = users_find(name);
    if (idx < 0 || pw_len_ok(password) == 0) {
        return 0;
    }

    u8 hash[USERS_HASH_LEN];
    foboscfg_kdf(g_users[idx].salt, password, FOBOSCFG_KDF_ROUNDS, hash);
    return hash_equal(hash, g_users[idx].hash);
}

/* ------------------------------------------------------------------ */
/* Самопроверка (ядро + host). KDF-раунды снижены для скорости теста.  */
/* ------------------------------------------------------------------ */

int users_selftest(void)
{
    int fails = 0;
    u8 salt[USERS_SALT_LEN];
    u8 h1[USERS_HASH_LEN], h2[USERS_HASH_LEN];

#define US_FAIL(msg) do { klog(KLOG_ERROR, "sys/users", "selftest: %s", msg); } while (0)

    /* KDF: детерминированность и чувствительность к паролю. */
    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        salt[i] = (u8)(i * 7u + 1u);
    }
    foboscfg_kdf(salt, "test", 16u, h1);
    foboscfg_kdf(salt, "test", 16u, h2);
    if (memcmp(h1, h2, USERS_HASH_LEN) != 0) {
        fails++; US_FAIL("kdf nondeterministic");
    }
    foboscfg_kdf(salt, "tost", 16u, h2);
    if (memcmp(h1, h2, USERS_HASH_LEN) == 0) {
        fails++; US_FAIL("kdf password-insensitive");
    }
    foboscfg_kdf(salt, "test", 17u, h2);
    if (memcmp(h1, h2, USERS_HASH_LEN) == 0) {
        fails++; US_FAIL("kdf rounds-insensitive");
    }

    /* KAT: kdf(salt=0x00..0x0F, pw="fobos", rounds=4) — опорное значение,
     * зафиксированное при внедрении (пересчитывается scripts/kdf_vector.py). */
    u8 zsalt[USERS_SALT_LEN] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    static const u8 kdf_kat[USERS_HASH_LEN] = {
        0x01, 0x35, 0x61, 0x74, 0x8e, 0xc6, 0x62, 0x31,
        0x71, 0xe8, 0xea, 0xd2, 0x71, 0x81, 0x8f, 0xd6,
        0xc1, 0xce, 0x80, 0xf0, 0x22, 0xfb, 0x3a, 0x2a,
        0x4d, 0x07, 0xb7, 0xa9, 0x7e, 0xfc, 0xc9, 0xaa
    };
    u8 kat_out[USERS_HASH_LEN];
    foboscfg_kdf(zsalt, "fobos", 4u, kat_out);
    if (memcmp(kat_out, kdf_kat, USERS_HASH_LEN) != 0) {
        fails++; US_FAIL("kdf KAT mismatch");
    }

    /* CRUD: добавление, дубликат, поиск, verify, смена пароля. */
    users_reset();
    if (users_add("admin", "secret1", 1) != K_OK) {
        fails++; US_FAIL("users_add admin failed");
    }
    if (users_add("admin", "other", 0) != K_EINVAL) {
        fails++; US_FAIL("duplicate accepted");
    }
    if (users_add("Admin", "other", 0) != K_EINVAL) {
        fails++; US_FAIL("uppercase accepted");
    }
    if (users_add("1abc", "other", 0) != K_EINVAL) {
        fails++; US_FAIL("digit-start accepted");
    }
    if (users_add("user2", "", 0) != K_EINVAL) {
        fails++; US_FAIL("empty password accepted");
    }
    if (users_count() != 1u) {
        fails++; US_FAIL("count != 1 after add");
    }

    if (users_verify("admin", "secret1") != 1) {
        fails++; US_FAIL("verify correct pw failed");
    }
    if (users_verify("admin", "secret2") != 0) {
        fails++; US_FAIL("verify wrong pw accepted");
    }
    if (users_verify("nobody", "secret1") != 0) {
        fails++; US_FAIL("verify unknown user accepted");
    }

    if (users_set_password("admin", "newpass5") != K_OK) {
        fails++; US_FAIL("set_password failed");
    }
    if (users_verify("admin", "secret1") != 0 ||
        users_verify("admin", "newpass5") != 1) {
        fails++; US_FAIL("password change not effective");
    }

    if (users_add("user2", "pw1234", 0) != K_OK) {
        fails++; US_FAIL("add user2 failed");
    }
    if (users_is_admin(0) != 1 || users_is_admin(1) != 0) {
        fails++; US_FAIL("admin flags wrong");
    }
    if (strcmp(users_name(1), "user2") != 0) {
        fails++; US_FAIL("users_name wrong");
    }
    if (users_find("user2") != 1 || users_find("ghost") != -1) {
        fails++; US_FAIL("users_find wrong");
    }

    /* Блоб: encode -> decode -> apply -> verify. */
    struct fobos_config cfg;
    u8 blob[512];
    u32 blob_size = 0;
    for (u32 i = 0; i < sizeof(cfg); i++) {
        ((u8 *)&cfg)[i] = 0;
    }
    cfg.flags = FOBOSCFG_FLAG_OOBE_DONE;
    (void)memcpy(cfg.hostname, "host-test", 10u);
    cfg.user_count = 2;
    u8 s1[USERS_SALT_LEN], s2[USERS_SALT_LEN];
    u8 p1[USERS_HASH_LEN], p2[USERS_HASH_LEN];
    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        s1[i] = (u8)(0xA0u + i);
        s2[i] = (u8)(0xB0u + i);
    }
    foboscfg_kdf(s1, "pass-one", FOBOSCFG_KDF_ROUNDS, p1);
    foboscfg_kdf(s2, "pass-two", FOBOSCFG_KDF_ROUNDS, p2);
    (void)memcpy(cfg.users[0].name, "alice", 6u);
    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        cfg.users[0].salt[i] = s1[i];
    }
    for (u32 i = 0; i < USERS_HASH_LEN; i++) {
        cfg.users[0].hash[i] = p1[i];
    }
    cfg.users[0].is_admin = 1;
    (void)memcpy(cfg.users[1].name, "bob", 4u);
    for (u32 i = 0; i < USERS_SALT_LEN; i++) {
        cfg.users[1].salt[i] = s2[i];
    }
    for (u32 i = 0; i < USERS_HASH_LEN; i++) {
        cfg.users[1].hash[i] = p2[i];
    }
    cfg.users[1].is_admin = 0;

    if (foboscfg_encode(&cfg, blob, sizeof(blob), &blob_size) != K_OK) {
        fails++; US_FAIL("blob encode failed");
    }
    struct fobos_config dec;
    if (foboscfg_decode(blob, blob_size, &dec) != K_OK) {
        fails++; US_FAIL("blob decode failed");
    }
    if (users_apply(&dec) != K_OK) {
        fails++; US_FAIL("users_apply failed");
    }
    if (users_count() != 2u ||
        users_verify("alice", "pass-one") != 1 ||
        users_verify("bob", "pass-two") != 1 ||
        users_verify("alice", "wrong") != 0) {
        fails++; US_FAIL("blob-applied users verify wrong");
    }
    if (users_is_admin(0) != 1 || users_is_admin(1) != 0) {
        fails++; US_FAIL("blob admin flags wrong");
    }

    /* Порча блога -> decode обязан отказать; база не меняется. */
    blob[16] ^= 0xFFu;
    if (foboscfg_decode(blob, blob_size, &dec) == K_OK) {
        fails++; US_FAIL("corrupted blob accepted");
    }
    blob[16] ^= 0xFFu;

    users_reset();
    return fails;
}

/* ------------------------------------------------------------------ */
/* Персистентность (FND-2c): база -> \fobos\setup.cfg                  */
/* ------------------------------------------------------------------ */

void users_export(struct fobos_config *out)
{
    for (u32 i = 0; i < sizeof(*out); i++) {
        ((u8 *)out)[i] = 0u;
    }
    out->flags = FOBOSCFG_FLAG_OOBE_DONE;
    out->user_count = g_count;
    for (u32 i = 0; i < g_count; i++) {
        for (u32 k = 0; k < FOBOSCFG_HOSTNAME_MAX - 1u; k++) {
            /* hostname вне зоны ответственности users: не трогаем */
        }
        struct foboscfg_user *du = &out->users[i];
        for (u32 k = 0; k < USERS_NAME_MAX; k++) {
            du->name[k] = g_users[i].name[k];
        }
        du->name[USERS_NAME_MAX - 1u] = '\0';
        du->is_admin = g_users[i].is_admin;
        for (u32 k = 0; k < USERS_SALT_LEN; k++) {
            du->salt[k] = g_users[i].salt[k];
        }
        for (u32 k = 0; k < USERS_HASH_LEN; k++) {
            du->hash[k] = g_users[i].hash[k];
        }
    }
}

err_t users_persist(void)
{
#ifndef FOBOS_HOST_TEST
    if (g_count == 0u) {
        return K_ESTATE;
    }
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return K_ENODEV;
    }
    struct fobos_config cfg;
    users_export(&cfg);
    static u8 blob[FOBOSCFG_HEADER_SIZE +
                   FOBOSCFG_MAX_USERS * FOBOSCFG_USER_SIZE +
                   FOBOSCFG_TAG_LEN];
    u32 size = 0;
    err_t rc = foboscfg_encode(&cfg, blob, sizeof(blob), &size);
    if (rc != K_OK) {
        return rc;
    }
    rc = fat_file_write(v, "\\fobos\\setup.cfg", blob, size);
    if (rc == K_OK) {
        klog(KLOG_INFO, "users",
             "setup.cfg: persisted users=%u", (unsigned int)g_count);
    }
    return rc;
#else
    return K_EUNSUPPORTED;
#endif
}
