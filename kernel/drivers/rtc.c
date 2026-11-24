/*
 * Fobos OS — драйвер RTC (MC146818-совместимые CMOS-часы, порты 0x70/0x71).
 *
 * Слой: drivers. Используется экраном входа/статус-баром/командой `date`.
 *
 * Особенности реализации:
 *  - поддержка BCD и бинарного формата (регистр Status B, бит 2) и 12/24 ч;
 *  - чтение с проверкой на рассинхрон: если значение изменилось между двумя
 *    проходами (UIP-переход), чтение повторяется (до RTC_READ_TRIES);
 *  - век: регистр century (0x32) при наличии; если == 0 — предполагается 20xx
 *    (задокументированное ограничение до ACPI-этапа).
 *
 * Чистые функции (bcd, валидация даты, форматирование) вынесены отдельно и
 * покрываются self-тестами в ядре и на host.
 */
#include "rtc.h"

#include "../lib/log.h"
#include "../lib/vsnprintf.h"
#include "../lib/string.h"

#ifndef FOBOS_HOST_TEST
#include "../lib/io.h"

#define RTC_PORT_CMD   0x70u
#define RTC_PORT_DATA  0x71u

#define RTC_REG_SEC    0x00u
#define RTC_REG_MIN    0x02u
#define RTC_REG_HOUR   0x04u
#define RTC_REG_DOW    0x06u
#define RTC_REG_DAY    0x07u
#define RTC_REG_MONTH  0x08u
#define RTC_REG_YEAR   0x09u
#define RTC_REG_STAT_A 0x0Au
#define RTC_REG_STAT_B 0x0Bu
#define RTC_REG_CENTURY 0x32u

#define RTC_STAT_A_UIP 0x80u
#define RTC_STAT_B_24H 0x02u
#define RTC_STAT_B_BIN 0x04u

#define RTC_READ_TRIES 8u

static const char *const RTC_TAG = "drv/rtc";
static int g_rtc_ready;
#endif /* !FOBOS_HOST_TEST */

/* ------------------------------------------------------------------ */
/* Чистые функции (ядро + host)                                        */
/* ------------------------------------------------------------------ */

u8 rtc_bcd_to_bin(u8 v, int is_bcd)
{
    if (is_bcd == 0) {
        return v;
    }
    return (u8)(((v >> 4) & 0x0Fu) * 10u + (v & 0x0Fu));
}

u8 rtc_bin_to_bcd(u8 v, int is_bcd)
{
    if (is_bcd == 0 || v >= 100u) {
        return v;
    }
    return (u8)(((v / 10u) << 4) | (v % 10u));
}

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) {
        return 0;
    }
    if (m == 2 && is_leap(y)) {
        return 29;
    }
    return d[m - 1];
}

int rtc_time_valid(int year, int month, int day, int hour, int minute, int second)
{
    if (year < 2000 || year > 2099) {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return 0;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return 0;
    }
    return 1;
}

int rtc_format(char *dst, u32 cap, const struct rtc_time *t)
{
    if (dst == NULL || cap < RTC_STR_MIN || t == NULL) {
        return -1;
    }
    if (rtc_time_valid(t->year, t->month, t->day, t->hour, t->minute, t->second) == 0) {
        return -1;
    }
    /* ksprintf-аналог вручную: фиксированный формат YYYY-MM-DD HH:MM:SS (19 симв.). */
    u32 i = 0;
    char tmp[5];
    u32 nums[6] = { (u32)t->year, (u32)t->month, (u32)t->day,
                    (u32)t->hour, (u32)t->minute, (u32)t->second };
    for (u32 k = 0; k < 6u; k++) {
        u32 v = nums[k];
        u32 width = (k == 0) ? 4u : 2u;
        for (u32 d = 0; d < width; d++) {
            tmp[width - 1u - d] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        for (u32 d = 0; d < width; d++) {
            dst[i++] = tmp[d];
        }
        if (k == 0 || k == 1) {
            dst[i++] = '-';
        } else if (k == 2) {
            dst[i++] = ' ';
        } else if (k == 3 || k == 4) {
            dst[i++] = ':';
        }
    }
    dst[i] = '\0';
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* Портовый ввод-вывод (только ядро)                                   */
/* ------------------------------------------------------------------ */

#ifndef FOBOS_HOST_TEST

static u8 rtc_read_reg(u8 reg)
{
    outb(RTC_PORT_CMD, reg);
    io_wait();
    return inb(RTC_PORT_DATA);
}

static void rtc_write_reg(u8 reg, u8 val)
{
    outb(RTC_PORT_CMD, reg);
    io_wait();
    outb(RTC_PORT_DATA, val);
    io_wait();
}

/* Один согласованный проход: читаем набор дважды, пока байты не совпадут. */
static int rtc_read_once(struct rtc_time *out, int *out_bcd, int *out_24h)
{
    u8 sec1, min1, hr1, day1, mon1, yr1, cen1;
    u8 stat_b;

    /* Ждём окончания цикла обновления (UIP=0), ограниченно. */
    for (u32 i = 0; i < 1000000u; i++) {
        if ((rtc_read_reg(RTC_REG_STAT_A) & RTC_STAT_A_UIP) == 0u) {
            break;
        }
    }

    sec1 = rtc_read_reg(RTC_REG_SEC);
    min1 = rtc_read_reg(RTC_REG_MIN);
    hr1  = rtc_read_reg(RTC_REG_HOUR);
    day1 = rtc_read_reg(RTC_REG_DAY);
    mon1 = rtc_read_reg(RTC_REG_MONTH);
    yr1  = rtc_read_reg(RTC_REG_YEAR);
    cen1 = rtc_read_reg(RTC_REG_CENTURY);

    u8 sec2 = rtc_read_reg(RTC_REG_SEC);
    u8 min2 = rtc_read_reg(RTC_REG_MIN);
    u8 hr2  = rtc_read_reg(RTC_REG_HOUR);
    u8 day2 = rtc_read_reg(RTC_REG_DAY);
    u8 mon2 = rtc_read_reg(RTC_REG_MONTH);
    u8 yr2  = rtc_read_reg(RTC_REG_YEAR);
    u8 cen2 = rtc_read_reg(RTC_REG_CENTURY);

    if (sec1 != sec2 || min1 != min2 || hr1 != hr2 ||
        day1 != day2 || mon1 != mon2 || yr1 != yr2 || cen1 != cen2) {
        return 0; /* рассинхрон — повторить */
    }

    stat_b = rtc_read_reg(RTC_REG_STAT_B);
    *out_bcd = (stat_b & RTC_STAT_B_BIN) == 0u;
    *out_24h = (stat_b & RTC_STAT_B_24H) != 0u;

    out->second = sec1;
    out->minute = min1;
    out->hour = hr1;
    out->day = day1;
    out->month = mon1;
    out->year = (u16)yr1;
    out->century = cen1;
    return 1;
}

err_t rtc_read_time(struct rtc_time *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    /* Примечание: rtc_init() вызывает эту функцию ДО установки g_rtc_ready,
    * поэтому готовность здесь не проверяется — чтение CMOS безопасно всегда. */

    struct rtc_time t;
    int bcd = 0, h24 = 1;

    for (u32 attempt = 0; attempt < RTC_READ_TRIES; attempt++) {
        if (rtc_read_once(&t, &bcd, &h24) != 0) {
            u16 year = rtc_bcd_to_bin((u8)t.year, bcd);
            u8 century = rtc_bcd_to_bin(t.century, bcd);
            int full_year;
            if (century >= 19u && century <= 20u) {
                full_year = (int)century * 100 + (int)year;
            } else {
                full_year = 2000 + (int)year; /* century-регистра нет: 20xx */
            }

            int hour = rtc_bcd_to_bin((u8)t.hour, bcd);
            if (h24 == 0) {
                int pm = (hour & 0x80) != 0;
                hour &= 0x7F;
                hour = hour % 12;
                if (pm) {
                    hour += 12;
                }
            }

            struct rtc_time norm = { 0 };
            norm.year = (u16)full_year;
            norm.month = rtc_bcd_to_bin((u8)t.month, bcd);
            norm.day = rtc_bcd_to_bin((u8)t.day, bcd);
            norm.hour = (u8)hour;
            norm.minute = rtc_bcd_to_bin((u8)t.minute, bcd);
            norm.second = rtc_bcd_to_bin((u8)t.second, bcd);

            if (rtc_time_valid(norm.year, norm.month, norm.day,
                               norm.hour, norm.minute, norm.second) == 0) {
                return K_EIO; /* RTC вернул бессмыслицу — не гоняем зря */
            }
            *out = norm;
            return K_OK;
        }
    }
    return K_EIO; /* RTC постоянно в обновлении */
}

err_t rtc_write_time(const struct rtc_time *t)
{
    if (t == NULL) {
        return K_EINVAL;
    }
    if (rtc_time_valid(t->year, t->month, t->day,
                       t->hour, t->minute, t->second) == 0) {
        return K_EINVAL;
    }
    if (rtc_ready() == 0) {
        return K_ESTATE;   /* CMOS не инициализирован — писать нельзя */
    }

    /* Формат хранимых значений читаем из Status B ТЕКУЩИЙ: менять режим
     * часов целиком — отдельное решение, не побочный эффект настройки. */
    u8 stat_b = rtc_read_reg(RTC_REG_STAT_B);
    int bcd = (stat_b & RTC_STAT_B_BIN) == 0u;

    /* Окно записи: UIP=0 означает, что цикл обновления не идёт; после
     * его окончания до 244 мкс остаётся «tail» — для наших полей его
     * хватает (классическая схема, без запрета обновлений Set=1). */
    for (u32 i = 0; i < 1000000u; i++) {
        if ((rtc_read_reg(RTC_REG_STAT_A) & RTC_STAT_A_UIP) == 0u) {
            break;
        }
    }

    rtc_write_reg(RTC_REG_SEC,   rtc_bin_to_bcd(t->second, bcd));
    rtc_write_reg(RTC_REG_MIN,   rtc_bin_to_bcd(t->minute, bcd));
    rtc_write_reg(RTC_REG_HOUR,  rtc_bin_to_bcd(t->hour, bcd));
    rtc_write_reg(RTC_REG_DAY,   rtc_bin_to_bcd(t->day, bcd));
    rtc_write_reg(RTC_REG_MONTH, rtc_bin_to_bcd(t->month, bcd));
    rtc_write_reg(RTC_REG_YEAR,  rtc_bin_to_bcd((u8)(t->year % 100u), bcd));
    if (t->century != 0u) {
        rtc_write_reg(RTC_REG_CENTURY, rtc_bin_to_bcd(t->century, bcd));
    }

    /* Контрольное чтение: убедиться, что значения легли. */
    struct rtc_time chk;
    err_t rc = rtc_read_time(&chk);
    if (rc != K_OK) {
        return rc;
    }
    if (chk.year != t->year || chk.month != t->month || chk.day != t->day ||
        chk.hour != t->hour || chk.minute != t->minute ||
        chk.second != t->second) {
        /* Секунда могла тикнуть между записью и чтением — допустимо
         * расхождение только по секундам в пределах одной тика. */
        if (!(chk.year == t->year && chk.month == t->month &&
              chk.day == t->day && chk.hour == t->hour &&
              chk.minute == t->minute &&
              (chk.second == t->second || chk.second == t->second + 1u ||
               (t->second == 59u && chk.second == 0u)))) {
            return K_EIO;
        }
    }
    return K_OK;
}

err_t rtc_init(void)
{
    struct rtc_time t;
    err_t rc = rtc_read_time(&t);
    if (rc != K_OK) {
        /* RTC может отсутствовать/отказать — не блокируем загрузку. */
        klog(KLOG_WARN, RTC_TAG, "rtc_init: первичное чтение не удалось (%s) — часы недоступны",
             k_strerror(rc));
        return rc;
    }
    g_rtc_ready = 1;

    char buf[RTC_STR_MIN];
    if (rtc_format(buf, sizeof(buf), &t) > 0) {
        klog(KLOG_INFO, RTC_TAG, "RTC: %s (CMOS, BCD-совместимый доступ)", buf);
    }
    return K_OK;
}

int rtc_ready(void)
{
    return g_rtc_ready;
}

#endif /* !FOBOS_HOST_TEST */

/* ------------------------------------------------------------------ */
/* Самопроверка чистых функций                                         */
/* ------------------------------------------------------------------ */

int rtc_selftest(void)
{
    int fails = 0;

    if (rtc_bcd_to_bin(0x00u, 1) != 0 || rtc_bcd_to_bin(0x59u, 1) != 59 ||
        rtc_bcd_to_bin(0x12u, 1) != 12 || rtc_bcd_to_bin(59u, 0) != 59) {
        fails++;
    }
    if (rtc_bin_to_bcd(0u, 1) != 0 || rtc_bin_to_bcd(59u, 1) != 0x59u ||
        rtc_bin_to_bcd(12u, 1) != 0x12u || rtc_bin_to_bcd(59u, 0) != 59u ||
        rtc_bin_to_bcd(100u, 1) != 100u) {
        fails++;
    }

    if (rtc_time_valid(2024, 2, 29, 0, 0, 0) != 1) {
        fails++;
    }
    if (rtc_time_valid(2023, 2, 29, 0, 0, 0) != 0) {
        fails++;
    }
    if (rtc_time_valid(2026, 13, 1, 0, 0, 0) != 0 ||
        rtc_time_valid(2026, 0, 10, 0, 0, 0) != 0 ||
        rtc_time_valid(2026, 4, 31, 0, 0, 0) != 0 ||
        rtc_time_valid(2026, 4, 0, 0, 0, 0) != 0 ||
        rtc_time_valid(2026, 1, 1, 24, 0, 0) != 0 ||
        rtc_time_valid(2026, 1, 1, 0, 60, 0) != 0 ||
        rtc_time_valid(2026, 1, 1, 0, 0, 60) != 0 ||
        rtc_time_valid(1999, 12, 31, 23, 59, 59) != 0 ||
        rtc_time_valid(2100, 1, 1, 0, 0, 0) != 0) {
        fails++;
    }

    struct rtc_time t = { 2026, 10, 28, 7, 5, 9, 20 };
    char buf[RTC_STR_MIN];
    if (rtc_format(buf, sizeof(buf), &t) != 19 ||
        buf[0] != '2' || buf[4] != '-' || buf[7] != '-' ||
        buf[10] != ' ' || buf[13] != ':' || buf[16] != ':' ||
        buf[19] != '\0') {
        fails++;
    }
    if (strcmp(buf, "2026-10-28 07:05:09") != 0) {
        fails++;
    }

    struct rtc_time bad = { 2026, 10, 28, 24, 0, 0, 20 };
    if (rtc_format(buf, sizeof(buf), &bad) != -1) {
        fails++;
    }
    if (rtc_format(buf, 8, &t) != -1) { /* тесный буфер */
        fails++;
    }

    return fails;
}
