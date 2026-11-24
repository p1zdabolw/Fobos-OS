/*
 * Fobos OS — реализация сессии и параметров системы (см. session.h).
 */
#include "session.h"

#include "users.h"
#include "../lib/string.h"

#define SESSION_HOST_DEFAULT "fobos"

static struct {
    u32 user_index;
    char user[USERS_NAME_MAX];
    u8 is_admin;
    char hostname[SESSION_HOSTNAME_MAX];
} g_sess;

static void set_hostname_safe(const char *hostname)
{
    u32 i = 0;
    if (hostname != NULL) {
        while (hostname[i] != '\0' && i + 1u < SESSION_HOSTNAME_MAX) {
            g_sess.hostname[i] = hostname[i];
            i++;
        }
    }
    if (i == 0u) {
        const char *def = SESSION_HOST_DEFAULT;
        while (*def != '\0') {
            g_sess.hostname[i++] = *def++;
        }
    }
    g_sess.hostname[i] = '\0';
}

void session_reset(void)
{
    g_sess.user_index = 0xFFFFFFFFu;
    g_sess.user[0] = '\0';
    g_sess.is_admin = 0;
    set_hostname_safe(NULL);
}

void session_start(u32 user_index)
{
    const char *name = users_name(user_index);
    if (name == NULL) {
        return; /* некорректный индекс: сессию не открываем */
    }
    g_sess.user_index = user_index;
    u32 i = 0;
    while (name[i] != '\0' && i + 1u < USERS_NAME_MAX) {
        g_sess.user[i] = name[i];
        i++;
    }
    g_sess.user[i] = '\0';
    g_sess.is_admin = (users_is_admin(user_index) == 1) ? 1u : 0u;
}

void session_end(void)
{
    g_sess.user_index = 0xFFFFFFFFu;
    g_sess.user[0] = '\0';
    g_sess.is_admin = 0;
}

int session_active(void)
{
    return (g_sess.user_index != 0xFFFFFFFFu) ? 1 : 0;
}

const char *session_user(void)
{
    return g_sess.user;
}

u32 session_user_index(void)
{
    return g_sess.user_index;
}

int session_is_admin(void)
{
    return (session_active() != 0 && g_sess.is_admin != 0u) ? 1 : 0;
}

void session_set_hostname(const char *hostname)
{
    set_hostname_safe(hostname);
}

const char *session_hostname(void)
{
    return g_sess.hostname;
}

void session_snapshot_save(struct session_snapshot *out)
{
    if (out == NULL) {
        return;
    }
    out->user_index = g_sess.user_index;
    u32 i = 0;
    while (g_sess.user[i] != '\0' && i + 1u < SESSION_USER_MAX) {
        out->user[i] = g_sess.user[i];
        i++;
    }
    out->user[i] = '\0';
    out->is_admin = g_sess.is_admin;
    i = 0;
    while (g_sess.hostname[i] != '\0' && i + 1u < SESSION_HOSTNAME_MAX) {
        out->hostname[i] = g_sess.hostname[i];
        i++;
    }
    out->hostname[i] = '\0';
}

void session_snapshot_restore(const struct session_snapshot *s)
{
    if (s == NULL) {
        return;
    }
    g_sess.user_index = s->user_index;
    u32 i = 0;
    while (s->user[i] != '\0' && i + 1u < SESSION_USER_MAX) {
        g_sess.user[i] = s->user[i];
        i++;
    }
    g_sess.user[i] = '\0';
    g_sess.is_admin = s->is_admin;
    i = 0;
    while (s->hostname[i] != '\0' && i + 1u < SESSION_HOSTNAME_MAX) {
        g_sess.hostname[i] = s->hostname[i];
        i++;
    }
    g_sess.hostname[i] = '\0';
}
