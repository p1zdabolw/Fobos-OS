/*
 * Fobos OS — учётные записи пользователей (RAM-база FND-2b).
 *
 * Слой: sys. Пользователи приходят из блога настроек (foboscfg, создан
 * OOBE-мастером в UEFI-загрузчике) и могут добавляться в рантайме
 * (useradd — только администратору). ПЕРСИСТЕНТНОСТЬ рантайм-изменений
 * появится с дисковой подсистемой (FND-2c): пока источник истины —
 * UEFI-переменная FobosSetup, записываемая загрузчиком.
 *
 * Пароли не хранятся: соль (16 байт) + KDF-хеш (SHA-256, FOBOSCFG_KDF_ROUNDS).
 * Сравнение хешей — в постоянном времени (constant-time).
 */
#ifndef FOBOS_SYS_USERS_H
#define FOBOS_SYS_USERS_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "../../common/foboscfg.h"

#define USERS_NAME_MAX  FOBOSCFG_NAME_MAX
#define USERS_PW_MAX    64u
#define USERS_SALT_LEN  FOBOSCFG_SALT_LEN
#define USERS_HASH_LEN  FOBOSCFG_HASH_LEN

struct user_entry {
    char name[USERS_NAME_MAX];
    u8   is_admin;
    u8   salt[USERS_SALT_LEN];
    u8   hash[USERS_HASH_LEN];
};

/* Снапшот базы для kselftest (тесты мутируют глобальное состояние). */
struct users_snapshot {
    struct user_entry e[FOBOSCFG_MAX_USERS];
    u32 count;
};

/* Очистка базы (используется тестами и загрузкой блога). */
void users_reset(void);

/* Загрузка базы из блога настроек: decode + валидация + замена содержимого.
 * При ошибке база остаётся прежней. */
err_t users_load_blob(const void *buf, u32 size);

/* Применение готовой распарсенной конфигурации (без decode). */
err_t users_apply(const struct fobos_config *cfg);

/* Создание пользователя: имя валидно/уникально, пароль 1..63 символа.
 * Соль генерируется из TSC+PIT-энтропии (на host — детерминированная). */
err_t users_add(const char *name, const char *password, int is_admin);

/* Добавление уже захешированной записи (загрузка из блога). */
err_t users_add_hashed(const char *name, const u8 salt[USERS_SALT_LEN],
                       const u8 hash[USERS_HASH_LEN], int is_admin);

/* Смена пароля (новая соль + KDF). */
err_t users_set_password(const char *name, const char *new_password);

u32  users_count(void);
int  users_find(const char *name);          /* индекс или -1 */
const char *users_name(u32 index);          /* NULL вне диапазона */
int  users_is_admin(u32 index);             /* -1 вне диапазона */

/* Сохранение/восстановление базы (для kselftest). */
void users_snapshot_save(struct users_snapshot *out);
void users_snapshot_restore(const struct users_snapshot *s);

/* 1 = пароль верен; 0 = неверен/пользователя нет. */
int users_verify(const char *name, const char *password);

/* Выгрузка базы в структуру конфига (для персистентности). */
void users_export(struct fobos_config *out);

/* Запись базы в \fobos\setup.cfg (FAT32). K_ENODEV если хранилища нет.
 * Вызывается после useradd/passwd; host-сборка: K_EUNSUPPORTED. */
err_t users_persist(void);

/* Самопроверка чистой логики (ядро + host); возвращает число упавших. */
int users_selftest(void);

#endif /* FOBOS_SYS_USERS_H */
