/*
 * Fobos OS — сессия пользователя и параметры системы (FND-2b).
 *
 * Хранит контекст активного сеанса (кто вошёл) и hostname. Существование
 * сессии управляет приглашением kshell, доступом к useradd/passwd и выходом
 * на экран входа (logout).
 */
#ifndef FOBOS_SYS_SESSION_H
#define FOBOS_SYS_SESSION_H

#include <fobos/types.h>

/* Границы совпадают с foboscfg (имена/hostname копируются целиком). */
#define SESSION_HOSTNAME_MAX 32u
#define SESSION_USER_MAX     32u

/* Снапшот сессии для тестов (kselftest мутирует глобальное состояние). */
struct session_snapshot {
    u32  user_index;
    char user[SESSION_USER_MAX];
    u8   is_admin;
    char hostname[SESSION_HOSTNAME_MAX];
};

/* Сброс: нет сессии, hostname по умолчанию ("fobos"). */
void session_reset(void);

/* Открыть сессию пользователя (индекс в базе users). */
void session_start(u32 user_index);

/* Закрыть сессию (logout). */
void session_end(void);

int  session_active(void);            /* 1 = есть сессия */
const char *session_user(void);       /* имя или "" */
u32  session_user_index(void);        /* индекс в users или 0xFFFFFFFF */
int  session_is_admin(void);          /* 1/0; вне сессии — 0 */

/* hostname: пустую строку заменяет умолчанием "fobos". */
void session_set_hostname(const char *hostname);
const char *session_hostname(void);

/* Сохранение/восстановление состояния (для kselftest). */
void session_snapshot_save(struct session_snapshot *out);
void session_snapshot_restore(const struct session_snapshot *s);

#endif /* FOBOS_SYS_SESSION_H */
