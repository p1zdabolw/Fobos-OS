/*
 * Fobos OS — драйвер RTC (CMOS-часы) (прототипы).
 */
#ifndef FOBOS_DRV_RTC_H
#define FOBOS_DRV_RTC_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Минимальная ёмкость буфера под строку "YYYY-MM-DD HH:MM:SS" (19 + NUL). */
#define RTC_STR_MIN 20u

struct rtc_time {
    u16 year;    /* полный, напр. 2026 */
    u8  month;   /* 1..12 */
    u8  day;     /* 1..31 */
    u8  hour;    /* 0..23 */
    u8  minute;  /* 0..59 */
    u8  second;  /* 0..59 */
    u8  century; /* сырой регистр century (диагностика) */
};

/* Инициализация: пробное чтение и лог времени. RTC не обязателен для
 * загрузки: ошибка не паникует, rtc_ready() вернёт 0. */
err_t rtc_init(void);

/* Текущее время. K_EIO при устойчивом рассинхроне/невалидных данных.
 * До rtc_init() чтение безопасно (используется rtc_init'ом). */
err_t rtc_read_time(struct rtc_time *out);

/* Запись времени в CMOS (FND-2e, Settings -> Date & Time).
 * Требует rtc_ready() и валидного времени; формат (BCD/bin) берётся
 * ТЕКУЩИЙ из Status B (режим часов не меняется неявно). После записи —
 * контрольное чтение. Не вызывать из IRQ. */
err_t rtc_write_time(const struct rtc_time *t);

/* 1, если RTC инициализирован и читается. */
int rtc_ready(void);

/* --- Чистые функции (ядро + host) --- */

/* BCD -> binary (или passthrough при is_bcd == 0). */
u8 rtc_bcd_to_bin(u8 v, int is_bcd);

/* binary -> BCD (или passthrough; v >= 100 возвращается как есть). */
u8 rtc_bin_to_bcd(u8 v, int is_bcd);

/* Валидация календарной даты/времени (2000..2099, учёт високосных). */
int rtc_time_valid(int year, int month, int day, int hour, int minute, int second);

/* "YYYY-MM-DD HH:MM:SS" (19 символов + NUL). Возвращает длину или -1. */
int rtc_format(char *dst, u32 cap, const struct rtc_time *t);

/* Самопроверка чистых функций; возвращает число упавших проверок. */
int rtc_selftest(void);

#endif /* FOBOS_DRV_RTC_H */
