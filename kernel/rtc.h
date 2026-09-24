#ifndef FOS_RTC_H
#define FOS_RTC_H

#include "types.h"

struct rtc_time {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int weekday;
};

void rtc_init(void);
void rtc_read(struct rtc_time *t);
void rtc_set_offset(int minutes);
int  rtc_get_offset(void);

#endif