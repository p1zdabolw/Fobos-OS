/*
 * Fobos OS — реализация logging-подсистемы.
 *
 * Стратегия памяти: кольцевой буфер статический (24 КиБ в .bss), стратегии
 * освобождения не требует; при переполнении вытесняются самые старые строки.
 * Вывод в serial — синхронный, busy-wait на LSR.THRE (на FND-1 прерываний нет).
 */
#include "log.h"
#include "vsnprintf.h"
#include "spinlock.h"
#include "string.h"
#include "io.h"

#define KLOG_LINE_MAX    192u
#define KLOG_RING_LINES  128u

static const char *const lvl_names[6] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "PANIC"
};

struct log_state {
    struct spinlock lock;
    enum klog_level min_level;
    int ready;
    /* Кольцевой буфер последних строк (crash-dump, FND-2+). */
    u32 ring_head;
    u32 ring_count;
    char ring[KLOG_RING_LINES][KLOG_LINE_MAX];
};

/* Дополнительный синк (fbcon): полная строка с '\n'. Вызывается в
 * main-контексте; в IRQ klog не используется (правило FND-2a).
 * klog_set_sink_level ограничивает уровень строк синка (PANIC — всегда). */
static void (*g_sink)(const char *line);
static enum klog_level g_sink_min = KLOG_TRACE;

static struct log_state g_log = {
    .lock = SPINLOCK_INIT("log"),
    .min_level = KLOG_INFO,
    .ready = 0,
    .ring_head = 0,
    .ring_count = 0,
    .ring = { { 0 } }
};

/* Слабая зависимость от драйвера serial: символ резолвится линкером.
 * До инициализации serial вывод глушится (serial_write сам проверяет ready). */
extern void serial_write(const char *data, u64 len);

void klog_init(void)
{
    u64 flags = spin_lock_irqsave(&g_log.lock);
    g_log.ready = 1;
    spin_unlock_irqrestore(&g_log.lock, flags);
}

void klog_set_sink(void (*fn)(const char *line))
{
    u64 flags = spin_lock_irqsave(&g_log.lock);
    g_sink = fn;
    spin_unlock_irqrestore(&g_log.lock, flags);
}

void klog_set_sink_level(enum klog_level lvl)
{
    if (lvl > KLOG_PANIC) {
        return;
    }
    u64 flags = spin_lock_irqsave(&g_log.lock);
    g_sink_min = lvl;
    spin_unlock_irqrestore(&g_log.lock, flags);
}

void klog_set_level(enum klog_level lvl)
{
    if (lvl > KLOG_PANIC) {
        return;
    }
    u64 flags = spin_lock_irqsave(&g_log.lock);
    g_log.min_level = lvl;
    spin_unlock_irqrestore(&g_log.lock, flags);
}

enum klog_level klog_get_level(void)
{
    return g_log.min_level;
}

static void ring_push(const char *line, u64 len)
{
    if (len >= KLOG_LINE_MAX) {
        len = KLOG_LINE_MAX - 1u;
    }
    char *slot = g_log.ring[g_log.ring_head];
    for (u64 i = 0; i < len; i++) {
        slot[i] = line[i];
    }
    slot[len] = '\0';
    g_log.ring_head = (g_log.ring_head + 1u) % KLOG_RING_LINES;
    if (g_log.ring_count < KLOG_RING_LINES) {
        g_log.ring_count++;
    }
}

static void emit_line(char *line, u64 len, int to_sink)
{
    if (len == 0u || len >= sizeof(((struct log_state *)0)->ring[0])) {
        return;
    }
    if (line[len - 1u] != '\n') {
        line[len++] = '\n';
    }
    serial_write(line, len);
    ring_push(line, len);
    if (to_sink != 0 && g_sink != NULL) {
        g_sink(line);
    }
}

void klog(enum klog_level lvl, const char *subsys, const char *fmt, ...)
{
    if (lvl > KLOG_PANIC || subsys == NULL) {
        return;
    }
    u64 flags = spin_lock_irqsave(&g_log.lock);
    int show = (g_log.ready != 0) && (lvl >= g_log.min_level);
    int sink_show = (lvl >= g_sink_min);
    spin_unlock_irqrestore(&g_log.lock, flags);

    /* Форматируем вне блокировки: serial_write сам лочится на запись кольца. */
    if (show) {
        char line[KLOG_LINE_MAX];
        int n = ksnprintf(line, sizeof(line), "[%013llu] %s [%s] ", rdtsc(),
                          lvl_names[lvl], subsys);
        if (n > 0) {
            va_list ap;
            va_start(ap, fmt);
            int m = kvsnprintf(line + n, (u64)sizeof(line) - (u64)n, fmt, ap);
            va_end(ap);
            if (m > 0) {
                emit_line(line, (u64)n + (u64)m, sink_show);
            }
        }
    } else if (lvl == KLOG_PANIC) {
        /* PANIC обязан попадать в кольцо даже до klog_init/ниже уровня. */
        char line[KLOG_LINE_MAX];
        va_list ap;
        va_start(ap, fmt);
        int m = kvsnprintf(line, sizeof(line) - 1u, fmt, ap);
        va_end(ap);
        if (m > 0) {
            ring_push(line, (u64)m);
        }
    }
}

void klog_raw(const char *s)
{
    if (s == NULL) {
        return;
    }
    u64 flags = spin_lock_irqsave(&g_log.lock);
    if (g_log.ready != 0) {
        serial_write(s, strlen(s));
    }
    spin_unlock_irqrestore(&g_log.lock, flags);
}

void klog_panic(const char *subsys, const char *fmt, ...)
{
    char line[KLOG_LINE_MAX];

    u64 flags = spin_lock_irqsave(&g_log.lock);
    g_log.ready = 1;
    spin_unlock_irqrestore(&g_log.lock, flags);

    int n = ksnprintf(line, sizeof(line), "[%013llu] PANIC [%s] ", rdtsc(), subsys);
    va_list ap;
    va_start(ap, fmt);
    int m = kvsnprintf(line + n, (u64)sizeof(line) - (u64)n - 1u, fmt, ap);
    va_end(ap);

    u64 iflags = spin_lock_irqsave(&g_log.lock);
    serial_write(line, (u64)n + (u64)m);
    serial_write("\n", 1u);
    ring_push(line, (u64)n + (u64)m);
    spin_unlock_irqrestore(&g_log.lock, iflags);

    if (g_sink != NULL) {
        g_sink(line);
        g_sink("\n");
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

u32 klog_ring_lines(void)
{
    return g_log.ring_count;
}
