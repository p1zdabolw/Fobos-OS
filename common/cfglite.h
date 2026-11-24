/*
 * Fobos OS — мини-парсер key=value конфигурации (cfglite).
 *
 * Общий модуль ядра и UEFI-загрузчика: \\fobos\\fobos.cfg хранит
 * настройки системы (режим рабочего стола, разрешение, тема, закрепления).
 * Формат текстовый, построчный:
 *
 *     # комментарий до конца строки
 *     key=value
 *     key = value        (пробелы вокруг '=' и по краям допустимы)
 *
 * Правила:
 *  - значение — до конца строки, обрезано по краям; внутренние пробелы
 *    сохраняются (нужно для pins=cmd, explorer, ...);
 *  - строки без '=' и пустые пропускаются;
 *  - переполнение буферов key/val НЕ обрезается молча: такая запись
 *    пропускается целиком (детерминированное поведение для CI);
 *  - без аллокаций, без зависимостей от окружения (freestanding).
 */
#ifndef FOBOS_COMMON_CFGLITE_H
#define FOBOS_COMMON_CFGLITE_H

#include <fobos/types.h>

#define CFGLITE_KEY_MAX 16u
#define CFGLITE_VAL_MAX 48u

struct cfglite_kv {
    char key[CFGLITE_KEY_MAX];
    char val[CFGLITE_VAL_MAX];
};

/* Разбирает буфер (len байт; NUL в конце не обязателен) в таблицу out.
 * Возвращает число разобранных записей (<= max). Комментарии '#', пустые
 * и некорректные строки пропускаются. */
u32 cfglite_parse(const char *buf, u32 len,
                  struct cfglite_kv *out, u32 max);

/* Значение по ключу или NULL. Сравнение точное (ключи в fobos.cfg —
 * нижний регистр по соглашению). */
const char *cfglite_get(const struct cfglite_kv *kvs, u32 n, const char *key);

#endif /* FOBOS_COMMON_CFGLITE_H */
