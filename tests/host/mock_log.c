/*
 * Fobos OS — мок logging-подсистемы для host-тестов.
 * ЗАМЕЩАЕТ kernel/lib/log.c ТОЛЬКО в host-сборке (Makefile, target host-tests):
 * вывод идёт в stdout с тем же форматом уровней. Производственный код не
 * использует этот файл.
 */
#include "../../kernel/lib/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *const lvl_names[6] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "PANIC"
};

void klog_init(void) {}

void klog_set_sink(void (*fn)(const char *line)) { (void)fn; }

void klog_set_level(enum klog_level lvl) { (void)lvl; }
enum klog_level klog_get_level(void) { return KLOG_DEBUG; }

static void emit(enum klog_level lvl, const char *subsys, const char *msg)
{
    fprintf(stdout, "[%013llu] %s [%s] %s\n",
            (unsigned long long)0u, lvl_names[lvl], subsys, msg);
    fflush(stdout);
}

void klog(enum klog_level lvl, const char *subsys, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(lvl, subsys, buf);
}

void klog_raw(const char *s)
{
    fputs(s, stdout);
}

void klog_panic(const char *subsys, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(KLOG_PANIC, subsys, buf);
    abort();
}

u32 klog_ring_lines(void) { return 0u; }
