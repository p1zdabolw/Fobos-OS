/*
 * Fobos OS — реализация cfglite (см. cfglite.h).
 */
#include "cfglite.h"

#include <stddef.h>

/* Копирует src[..len) с обрезкой пробелов по краям в dst (ёмкость cap).
 * Возвращает 1, если уместилось (с NUL), 0 — если нет. */
static int cl_trim_copy(const char *src, u32 len, char *dst, u32 cap)
{
    u32 b = 0;
    u32 e = len;
    while (b < e && (src[b] == ' ' || src[b] == '\t')) {
        b++;
    }
    while (e > b && (src[e - 1u] == ' ' || src[e - 1u] == '\t')) {
        e--;
    }
    if (e - b + 1u > cap) {   /* +1 под NUL */
        return 0;
    }
    for (u32 i = 0; i < e - b; i++) {
        dst[i] = src[b + i];
    }
    dst[e - b] = '\0';
    return 1;
}

u32 cfglite_parse(const char *buf, u32 len,
                  struct cfglite_kv *out, u32 max)
{
    if (buf == NULL || out == NULL || max == 0u) {
        return 0;
    }
    u32 count = 0;
    u32 i = 0;
    while (i < len && count < max) {
        /* Начало строки i; ищем конец строки. */
        u32 ls = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        u32 le = i;           /* '\n' или len */
        if (i < len) {
            i++;              /* шаг за '\n' */
        }
        /* Срезаем '\r' в конце строки (файлы могут прийти с CRLF). */
        while (le > ls && buf[le - 1u] == '\r') {
            le--;
        }

        /* Поиск '=' вне комментария. Комментарий: '#' до '='. */
        u32 eq = ls;
        int bad = 0;
        while (eq < le) {
            char c = buf[eq];
            if (c == '#') {
                bad = 1;      /* строка-комментарий */
                break;
            }
            if (c == '=') {
                break;
            }
            eq++;
        }
        if (bad != 0 || eq == le) {
            continue;         /* комментарий, пустая строка или без '=' */
        }

        if (cl_trim_copy(buf + ls, eq - ls, out[count].key,
                         CFGLITE_KEY_MAX) == 0) {
            continue;         /* ключ не уместился — запись пропускается */
        }
        if (out[count].key[0] == '\0') {
            continue;         /* пустой ключ */
        }
        if (cl_trim_copy(buf + eq + 1u, le - eq - 1u, out[count].val,
                         CFGLITE_VAL_MAX) == 0) {
            continue;         /* значение не уместилось — запись пропускается */
        }
        count++;
    }
    return count;
}

const char *cfglite_get(const struct cfglite_kv *kvs, u32 n, const char *key)
{
    if (kvs == NULL || key == NULL) {
        return NULL;
    }
    for (u32 i = 0; i < n; i++) {
        const char *a = kvs[i].key;
        const char *b = key;
        u32 j = 0;
        int eq = 1;
        for (;;) {
            if (a[j] != b[j]) {
                eq = 0;
                break;
            }
            if (a[j] == '\0') {
                break;
            }
            j++;
            if (j >= CFGLITE_KEY_MAX) {
                eq = 0;   /* страховка от незавершённого ключа */
                break;
            }
        }
        if (eq != 0) {
            return kvs[i].val;
        }
    }
    return NULL;
}
