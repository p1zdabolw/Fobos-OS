#include "rtc.h"
#include "../lib/printf.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static int g_offset_min = 0;

static inline void outb_r(u16 p, u8 v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static inline u8 inb_r(u16 p) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static u8 cmos_read(u8 reg) {
    outb_r(CMOS_ADDR, reg);
    return inb_r(CMOS_DATA);
}

static int rtc_updating(void) {
    outb_r(CMOS_ADDR, 0x0A);
    return (inb_r(CMOS_DATA) & 0x80) != 0;
}

static int bcd_to_bin(u8 v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 30;
    if (m == 2 && is_leap(y)) return 29;
    return dim[m - 1];
}

static int days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void apply_offset(struct rtc_time *t) {
    if (g_offset_min == 0) return;

    int total = t->hour * 60 + t->minute + g_offset_min;
    int day_delta = 0;
    while (total < 0)      { total += 1440; day_delta--; }
    while (total >= 1440)  { total -= 1440; day_delta++; }
    t->hour   = total / 60;
    t->minute = total % 60;

    if (day_delta == 0) return;

    t->day += day_delta;
    while (t->day < 1) {
        t->month--;
        if (t->month < 1) { t->month = 12; t->year--; }
        t->day += days_in_month(t->year, t->month);
    }
    while (t->day > days_in_month(t->year, t->month)) {
        t->day -= days_in_month(t->year, t->month);
        t->month++;
        if (t->month > 12) { t->month = 1; t->year++; }
    }
    int days = days_from_civil(t->year, t->month, t->day);
    t->weekday = ((days + 4) % 7 + 7) % 7;
}

void rtc_init(void) {
    while (rtc_updating()) { }
    u8 s = cmos_read(0x00);
    u8 m = cmos_read(0x02);
    u8 h = cmos_read(0x04);
    u8 d = cmos_read(0x07);
    u8 mo = cmos_read(0x08);
    u8 y = cmos_read(0x09);
    u8 regB = cmos_read(0x0B);
    int bcd = !(regB & 0x04);
    kprintf("RTC: raw %02x:%02x:%02x %02x/%02x/%02x bcd=%d\n",
            h, m, s, mo, d, y, bcd);
}

void rtc_read(struct rtc_time *t) {
    u8 sec = 0, min = 0, hour = 0, day = 0, mon = 0, year = 0, wday = 0;
    u8 regB = 0;
    u8 cty = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        while (rtc_updating()) { }
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
        wday = cmos_read(0x06);
        day  = cmos_read(0x07);
        mon  = cmos_read(0x08);
        year = cmos_read(0x09);
        regB = cmos_read(0x0B);
        cty  = cmos_read(0x32);
        if (!rtc_updating()) break;
    }

    int bcd_mode = !(regB & 0x04);
    int hour12 = !(regB & 0x02);
    int pm = 0;

    if (hour12) {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
    }

    if (bcd_mode) {
        sec  = (u8)bcd_to_bin(sec);
        min  = (u8)bcd_to_bin(min);
        hour = (u8)bcd_to_bin(hour);
        day  = (u8)bcd_to_bin(day);
        mon  = (u8)bcd_to_bin(mon);
        year = (u8)bcd_to_bin(year);
        wday = (u8)bcd_to_bin(wday);
        if (cty) cty = (u8)bcd_to_bin(cty);
    }

    if (hour12) {
        if (pm && hour != 12) hour = (u8)(hour + 12);
        if (!pm && hour == 12) hour = 0;
    }

    int century = 20;
    if (cty >= 19 && cty <= 21) century = cty;

    t->second = sec;
    t->minute = min;
    t->hour = hour;
    t->day = day;
    t->month = mon;
    t->year = century * 100 + year;
    t->weekday = wday & 0x07;

    apply_offset(t);
}

void rtc_set_offset(int minutes) {
    if (minutes < -12 * 60) minutes = -12 * 60;
    if (minutes >  14 * 60) minutes =  14 * 60;
    g_offset_min = minutes;
}

int rtc_get_offset(void) {
    return g_offset_min;
}