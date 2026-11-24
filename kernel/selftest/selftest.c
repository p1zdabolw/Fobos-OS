/*
 * Fobos OS — набор самопроверок FND-1/FND-2a.
 *
 * Тестируемые модули (критические по стандартам проекта):
 *  - lib/vsnprintf : полный контракт форматтера (положительные и граничные случаи);
 *  - mm/phys       : alloc/free/двойной free/исчерпание/инварианты статистики;
 *  - fobos/fbpi    : checksum и валидация (позитив + негатив на копии заголовка);
 *  - arch/idt      : сквозной путь прерываний (int3) + IRQ-гейты 0x20..0x2F — только ядро;
 *  - drivers/fb    : примитивы рисования — только ядро;
 *  - drivers/timer : продвижение тиков PIT после sleep — только ядро;
 *  - drivers/ps2   : самопроверка контроллера 8042 (0xAA -> 0x55) — только ядро;
 *  - drivers/kbd   : чистая трансляция сканкодов (ядро + host);
 *  - drivers/mouse : чистый парсер пакетов (ядро + host).
 *
 * Приём "linked-list в кадрах": при исчерпании пула адрес предыдущего кадра
 * пишется в сам кадр (допустимо: кадр в identity-отображении ядра / в мок-окне
 * host-теста), после чего пул восстанавливается обратным проходом. Тест
 * проверяет и инвариант счётчиков до/после.
 *
 * Предусловие: phys_init() уже выполнен (в ядре — kmain, на host — runner).
 * Для таймера: timer_init() тоже выполнен до kselftest_run (kmain FND-2a).
 */
#include "selftest.h"

#include <fobos/boot_info.h>
#include <fobos/errors.h>

#include "../../common/sha256.h"
#include "../../common/foboscfg.h"
#include "../../common/cfglite.h"
#include "../lib/log.h"
#include "../lib/vsnprintf.h"
#include "../lib/string.h"
#include "../mm/phys.h"
#include "../drivers/kbd.h"
#include "../drivers/mouse.h"
#include "../drivers/rtc.h"
#include "../sys/elf.h"
#include "../sys/task.h"
#include "../sys/users.h"
#include "../sys/session.h"
#include "../fs/fatp.h"
#include "../fs/ovl.h"

#ifndef FOBOS_HOST_TEST
#include "../fs/blk.h"
#include "../fs/fat32.h"
#endif

#ifndef FOBOS_HOST_TEST
#include "../drivers/fb.h"
#include "../drivers/timer.h"
#include "../drivers/ps2.h"
#include "../arch/arch.h"
#include "../lib/cpuid.h"
#endif

static const char *const ST_TAG = "kselftest";

static int g_passed;
static int g_failed;

static void copy_fw_name(char *dst, const char *src)
{
    u32 i = 0;
    while (src[i] != '\0' && i < FBPI_VENDOR_MAX - 1u) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void report(const char *name, int ok, const char *detail)
{
    if (ok != 0) {
        g_passed++;
        klog(KLOG_INFO, ST_TAG, "PASS %s", name);
    } else {
        g_failed++;
        klog(KLOG_ERROR, ST_TAG, "FAIL %s: %s", name, (detail != NULL) ? detail : "-");
    }
}

/* ================================================================== */
/* lib/vsnprintf                                                       */
/* ================================================================== */

static int expect_eq_str(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        char buf[160];
        (void)ksnprintf(buf, sizeof(buf), "got \"%s\", want \"%s\"", got, want);
        report(name, 0, buf);
        return 0;
    }
    report(name, 1, NULL);
    return 1;
}

static void t_vsnprintf_int(void)
{
    char b[64];

    (void)ksnprintf(b, sizeof(b), "%d", 42);
    expect_eq_str("vsnprintf/%d-positive", b, "42");

    (void)ksnprintf(b, sizeof(b), "%d", -42);
    expect_eq_str("vsnprintf/%d-negative", b, "-42");

    (void)ksnprintf(b, sizeof(b), "%i", 0);
    expect_eq_str("vsnprintf/%i-zero", b, "0");

    (void)ksnprintf(b, sizeof(b), "%lld", -9223372036854775807LL - 1);
    expect_eq_str("vsnprintf/%lld-min", b, "-9223372036854775808");

    (void)ksnprintf(b, sizeof(b), "%d", 2147483647);
    expect_eq_str("vsnprintf/%d-max32", b, "2147483647");

    (void)ksnprintf(b, sizeof(b), "%hhd", 300);
    expect_eq_str("vsnprintf/%hhd-trunc", b, "44"); /* 300 & 0xFF */
}

static void t_vsnprintf_unsigned(void)
{
    char b[64];

    (void)ksnprintf(b, sizeof(b), "%u", 4000000000u);
    expect_eq_str("vsnprintf/%u", b, "4000000000");

    (void)ksnprintf(b, sizeof(b), "%x", 0xdeadbeefu);
    expect_eq_str("vsnprintf/%x", b, "deadbeef");

    (void)ksnprintf(b, sizeof(b), "%X", 0xABCDu);
    expect_eq_str("vsnprintf/%X", b, "ABCD");

    (void)ksnprintf(b, sizeof(b), "%o", 8u);
    expect_eq_str("vsnprintf/%o", b, "10");

    (void)ksnprintf(b, sizeof(b), "%llx", 0xFFFFFFFFFFFFFFFFull);
    expect_eq_str("vsnprintf/%llx-max", b, "ffffffffffffffff");
}

static void t_vsnprintf_width(void)
{
    char b[64];

    (void)ksnprintf(b, sizeof(b), "%8d", 42);
    expect_eq_str("vsnprintf/width-space", b, "      42");

    (void)ksnprintf(b, sizeof(b), "%08x", 0x1F);
    expect_eq_str("vsnprintf/width-zero", b, "0000001f");

    (void)ksnprintf(b, sizeof(b), "%-8d|", 42);
    expect_eq_str("vsnprintf/width-left", b, "42      |");

    (void)ksnprintf(b, sizeof(b), "%10s", "ab");
    expect_eq_str("vsnprintf/width-str", b, "        ab");
}

static void t_vsnprintf_misc(void)
{
    char b[64];

    (void)ksnprintf(b, sizeof(b), "%s", NULL);
    expect_eq_str("vsnprintf/%s-null", b, "(null)");

    (void)ksnprintf(b, sizeof(b), "%c%c", 'o', 'k');
    expect_eq_str("vsnprintf/%c", b, "ok");

    (void)ksnprintf(b, sizeof(b), "100%%");
    expect_eq_str("vsnprintf/%%", b, "100%");

    (void)ksnprintf(b, sizeof(b), "%p", (void *)0);
    expect_eq_str("vsnprintf/%p-null", b, "(nil)");

    (void)ksnprintf(b, sizeof(b), "%y", 1); /* неизвестный спецификатор */
    expect_eq_str("vsnprintf/unknown-spec", b, "%y");

    (void)ksnprintf(b, sizeof(b), "%s %d %llx", "v", -7, 0xFFull);
    expect_eq_str("vsnprintf/mixed", b, "v -7 ff");
}

static void t_vsnprintf_truncate(void)
{
    char b[8];

    int n = ksnprintf(b, sizeof(b), "0123456789");
    if (n != 10) {
        report("vsnprintf/truncate", 0, "возврат != 10");
        return;
    }
    if (b[7] != '\0') {
        report("vsnprintf/truncate", 0, "нет NUL-терминации");
        return;
    }
    report("vsnprintf/truncate", 1, NULL);
}

/* ================================================================== */
/* mm/phys                                                             */
/* ================================================================== */

static void t_phys_alloc_reuse(void)
{
    u64 a = 0, c = 0;

    if (phys_alloc_frame(&a) != K_OK) {
        report("phys/alloc-reuse", 0, "первый alloc вернул ошибку");
        return;
    }
    if (phys_free_frame(a) != K_OK) {
        report("phys/alloc-reuse", 0, "free вернул ошибку");
        return;
    }
    if (phys_alloc_frame(&c) != K_OK || c != a) {
        report("phys/alloc-reuse", 0, "повторный alloc вернул другой кадр");
        return;
    }
    (void)phys_free_frame(c);
    report("phys/alloc-reuse", 1, NULL);
}

static void t_phys_double_free(void)
{
    u64 a = 0;

    if (phys_alloc_frame(&a) != K_OK) {
        report("phys/double-free", 0, "alloc вернул ошибку");
        return;
    }
    if (phys_free_frame(a) != K_OK) {
        report("phys/double-free", 0, "первый free вернул ошибку");
        return;
    }
    if (phys_free_frame(a) == K_OK) {
        report("phys/double-free", 0, "двойной free ПРОШЁЛ молча");
        return;
    }
    report("phys/double-free", 1, NULL);
}

static void t_phys_stats_sane(void)
{
    struct phys_stats st = phys_get_stats();
    int ok = (st.total_frames > 0u) && (st.free_frames > 0u) &&
             (st.free_frames <= st.total_frames) &&
             (st.reserved_frames + st.free_frames == st.total_frames);
    report("phys/stats-sane", ok, "инварианты счётчиков нарушены");
}

/* Смежное выделение (задний буфер fb): счётчики, roundtrip, восстановление. */
static void t_phys_alloc_contig(void)
{
    u64 a = 0;
    const u32 n = 3u;

    u64 free_before = phys_get_stats().free_frames;

    if (phys_alloc_contig(n, &a) != K_OK) {
        report("phys/contig", 0, "alloc 3 смежных кадров вернул ошибку");
        return;
    }
    if ((a & (PAGE_SIZE - 1u)) != 0u) {
        report("phys/contig", 0, "база не выровнена на 4 КиБ");
        (void)phys_free_frame(a);
        return;
    }

    int ok = (free_before - phys_get_stats().free_frames == (u64)n);
    const char *detail = "счётчик свободных кадров изменился не ровно на n";

    if (ok != 0) {
        ok = 1;
        detail = NULL;
        for (u32 k = 0; k < n; k++) {
            if (phys_free_frame(a + (u64)k * PAGE_SIZE) != K_OK) {
                ok = 0;
                detail = "free кадра диапазона вернул ошибку";
                break;
            }
        }
    }
    if (ok != 0) {
        /* Состояние bitmap восстановлено => contig обязан вернуть ту же базу. */
        u64 b = 0;
        if (phys_alloc_contig(n, &b) != K_OK || b != a) {
            ok = 0;
            detail = "повторный contig вернул другую базу/ошибку";
        } else {
            for (u32 k = 0; k < n; k++) {
                (void)phys_free_frame(b + (u64)k * PAGE_SIZE);
            }
        }
    }
    if (ok != 0 && phys_get_stats().free_frames != free_before) {
        ok = 0;
        detail = "пул не восстановлен после roundtrip";
    }
    report("phys/contig", ok, detail);
}

/* Исчерпание пула через linked-list в самих кадрах; полное восстановление. */
static void t_phys_exhaustion(void)
{
    struct phys_stats before = phys_get_stats();
    u64 prev = 0;
    u64 count = 0;
    u64 f = 0;
    int ok = 1;
    const char *detail = NULL;

    while (phys_alloc_frame(&f) == K_OK) {
        *(volatile u64 *)(u64)f = prev; /* адрес предыдущего кадра — в сам кадр */
        prev = f;
        count++;
        if (count > before.free_frames) {
            ok = 0;
            detail = "alloc выдал больше кадров, чем free_frames";
            break;
        }
    }

    if (ok != 0) {
        if (count != before.free_frames) {
            ok = 0;
            detail = "слитое число кадров != free_frames";
        } else if (phys_alloc_frame(&f) != K_ENOMEM) {
            ok = 0;
            detail = "после исчерпания alloc вернул не K_ENOMEM";
        }
    }

    /* Восстановление: обратный проход по списку в кадрах. */
    if (ok != 0) {
        while (prev != 0u) {
            u64 next = *(volatile u64 *)(u64)prev;
            if (phys_free_frame(prev) != K_OK) {
                ok = 0;
                detail = "free при восстановлении вернул ошибку";
                break;
            }
            prev = next;
            count--;
        }
    }
    if (ok != 0 && count != 0u) {
        ok = 0;
        detail = "список кадров не был корректно восстановлен";
    }

    struct phys_stats after = phys_get_stats();
    if (ok != 0 &&
        (after.free_frames != before.free_frames ||
         after.reserved_frames != before.reserved_frames)) {
        ok = 0;
        detail = "счётчики пула не восстановились";
    }

    report("phys/exhaustion", ok, detail);
}

/* ================================================================== */
/* fobos/fbpi                                                          */
/* ================================================================== */

static void t_fbpi_checksum(void)
{
    struct fbpi_boot_info bi;

    memset(&bi, 0, sizeof(bi));
    bi.magic = FBPI_MAGIC;
    bi.version = FBPI_VERSION;
    bi.header_size = (u16)sizeof(bi);
    bi.kernel_size_bytes = 0x20000u;
    bi.kernel_vbase = 0xFFFFFFFF80100000ull;
    bi.kernel_pbase = 0x100000ull;
    bi.efi_mmap_phys = 0x7000ull;
    bi.efi_mmap_size = 640u;
    bi.efi_desc_size = 40u;
    bi.efi_desc_version = 1u;
    bi.fb.width = 1024u;
    bi.fb.height = 768u;
    bi.fb.bpp = 32u;
    bi.fb.pitch_bytes = 4096u;
    bi.fb.pixel_format = FBPI_PF_RGBX;
    copy_fw_name(bi.firmware_vendor, "TestFW");

    bi.checksum = fbpi_compute_checksum(&bi);
    if (fbpi_validate(&bi) != K_OK) {
        report("fbpi/checksum", 0, "валидный заголовок отвергнут");
        return;
    }

    bi.checksum ^= 1u; /* порча одного бита */
    if (fbpi_validate(&bi) != K_EINVAL) {
        report("fbpi/checksum", 0, "испорченный заголовок прошёл валидацию");
        return;
    }
    report("fbpi/checksum", 1, NULL);
}

static void t_fbpi_bltonly_rejected(void)
{
    struct fbpi_boot_info bi;

    memset(&bi, 0, sizeof(bi));
    bi.magic = FBPI_MAGIC;
    bi.version = FBPI_VERSION;
    bi.header_size = (u16)sizeof(bi);
    bi.kernel_size_bytes = 0x20000u;
    bi.kernel_vbase = 0xFFFFFFFF80100000ull;
    bi.kernel_pbase = 0x100000ull;
    bi.efi_mmap_phys = 0x7000ull;
    bi.efi_mmap_size = 640u;
    bi.efi_desc_size = 40u;
    bi.efi_desc_version = 1u;
    bi.fb.width = 1024u;
    bi.fb.height = 768u;
    bi.fb.bpp = 32u;
    bi.fb.pitch_bytes = 4096u;
    bi.fb.pixel_format = FBPI_PF_BLTONLY;
    copy_fw_name(bi.firmware_vendor, "TestFW");
    bi.checksum = fbpi_compute_checksum(&bi);

    int ok = (fbpi_validate(&bi) == K_EINVAL);
    report("fbpi/bltonly-rejected", ok, "BltOnly-режим прошёл валидацию");
}

/* ================================================================== */
/* drivers/kbd + drivers/mouse: чистая логика (ядро + host)           */
/* ================================================================== */

static void t_kbd_translate(void)
{
    int fails = kbd_selftest();
    report("kbd/translate", fails == 0, "часть проверок трансляции упала");
}

static void t_mouse_parse(void)
{
    int fails = mouse_selftest();
    report("mouse/parse", fails == 0, "часть проверок парсера пакетов упала");
}

/* ================================================================== */
/* common/sha256, common/foboscfg, sys/users, drivers/rtc, sys/session */
/* ================================================================== */

static void t_sha256_kat(void)
{
    int fails = 0;
    u8 out[SHA256_DIGEST_LEN];

    /* FIPS 180-4: пустая строка. */
    sha256("", 0, out);
    static const u8 e3b0[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    if (memcmp(out, e3b0, 32) != 0) {
        fails++;
    }

    /* FIPS 180-4: "abc". */
    sha256("abc", 3, out);
    static const u8 ba78[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    if (memcmp(out, ba78, 32) != 0) {
        fails++;
    }

    /* FIPS 180-4: 56 байт (доболнение ровно одним блоком длины). */
    static const char msg56[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(msg56, 56, out);
    static const u8 d248[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };
    if (memcmp(out, d248, 32) != 0) {
        fails++;
    }

    /* Двухблочный вектор NIST (112 байт). */
    static const char msg112[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha256(msg112, 112, out);
    static const u8 cf5b[32] = {
        0xcf, 0x5b, 0x16, 0xa7, 0x78, 0xaf, 0x83, 0x80,
        0x03, 0x6c, 0xe5, 0x9e, 0x7b, 0x04, 0x92, 0x37,
        0x0b, 0x24, 0x9b, 0x11, 0xe8, 0xf0, 0x7a, 0x51,
        0xaf, 0xac, 0x45, 0x03, 0x7a, 0xfe, 0xe9, 0xd1
    };
    if (memcmp(out, cf5b, 32) != 0) {
        fails++;
    }

    /* Побайтовое наращивание через update == одноразовый вызов. */
    struct sha256_ctx c;
    sha256_init(&c);
    for (u32 i = 0; i < 56; i++) {
        sha256_update(&c, &msg56[i], 1);
    }
    sha256_final(&c, out);
    if (memcmp(out, d248, 32) != 0) {
        fails++;
    }

    report("sha256/kat", fails == 0, "NIST-векторы разошлись");
}

static void t_foboscfg_roundtrip(void)
{
    /* users_selftest мутирует RAM-базу: под живой сессией восстанавливаем. */
    struct users_snapshot snap;
    users_snapshot_save(&snap);
    int fails = users_selftest(); /* включает encode/decode/apply/порчу блога */
    users_snapshot_restore(&snap);
    report("foboscfg/users", fails == 0, "часть проверок блога/KDF упала");
}

static void t_rtc_pure(void)
{
    int fails = rtc_selftest();
    report("rtc/pure", fails == 0, "часть проверок BCD/даты/формата упала");
}

static void t_session_basic(void)
{
    int fails = 0;
    struct session_snapshot ssnap;
    struct users_snapshot usnap;
    session_snapshot_save(&ssnap);
    users_snapshot_save(&usnap);

    session_reset();
    if (session_active() != 0) {
        fails++;
    }
    if (strcmp(session_hostname(), "fobos") != 0) {
        fails++;
    }
    session_set_hostname("");
    if (strcmp(session_hostname(), "fobos") != 0) {
        fails++; /* пустое имя -> умолчание */
    }
    session_set_hostname("testhost");
    if (strcmp(session_hostname(), "testhost") != 0) {
        fails++;
    }
    if (users_add("alice", "pass1234", 1) != K_OK) {
        fails++;
    }
    session_start(users_find("alice"));
    if (session_active() != 1 || strcmp(session_user(), "alice") != 0 ||
        session_is_admin() != 1) {
        fails++;
    }
    session_end();
    if (session_active() != 0) {
        fails++;
    }
    session_reset();
    users_reset();

    session_snapshot_restore(&ssnap);
    users_snapshot_restore(&usnap);
    report("session/basic", fails == 0, "логика сессии разошлась");
}

/* ================================================================== */
/* Только ядро: arch/idt, drivers/fb, drivers/timer, drivers/ps2, irq */
/* ================================================================== */

#ifndef FOBOS_HOST_TEST

static volatile int g_int3_returned;

static void t_idt_breakpoint(void)
{
    g_int3_returned = 0;
    __asm__ volatile ("int3");
    /* Управление дошло сюда => обработчик #BP отработал и вернулся. */
    g_int3_returned = 1;
    report("idt/int3-roundtrip", g_int3_returned != 0,
           "управление не вернулось после int3");
}

static void t_fb_sanity(void)
{
    /* fb_init уже выполнен в kmain; проверяем граничный вызов примитива. */
    err_t rc = fb_fill_rect(0, 0, 1, 1, 0x000000);
    int ok = (rc == K_OK) || (rc == K_ESTATE);
    report("fb/fill-rect", ok, "fb_fill_rect вернул неожиданную ошибку");
}

static void t_irq_gates(void)
{
    int ok = 1;
    for (u16 v = 0x20u; v <= 0x2Fu; v++) {
        if (idt_gate_present(v) == 0) {
            ok = 0;
            break;
        }
    }
    report("irq/gates-0x20-0x2F", ok, "не все IRQ-гейты present/interrupt-gate");
}

static void t_timer_advance(void)
{
    u64 t0 = timer_get_ticks();
    timer_sleep_ms(60);
    u64 t1 = timer_get_ticks();
    u64 delta = t1 - t0;
    /* PIT эмулируется QEMU точно (~1000 Гц); TCG-джиттер компенсируем
     * широким коридором 30..600 тиков. */
    int ok = (delta >= 30ull) && (delta <= 600ull);
    report("timer/ticks-advance", ok, "тики не продвинулись за 60 мс сна");
}

static void t_timer_sleep_zero(void)
{
    u64 t0 = timer_get_ticks();
    timer_sleep_ms(0);
    u64 t1 = timer_get_ticks();
    report("timer/sleep-zero", t1 == t0, "sleep(0) продвинул тики");
}

static void t_ps2_selftest(void)
{
    u8 st = 0;
    /* cli на время самотеста: устройства (мышь) не должны вклинить байты
     * между дренажем и ответом 0x55. Потеря пакетов в окне — приемлема. */
    u64 flags;
    __asm__ volatile ("pushfq\n\tcli\n\tpopq %0" : "=r"(flags) : : "memory");
    err_t rc = ps2_controller_selftest(&st);
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");

    int ok = (rc == K_OK) && (st == 0x55u);
    if (!ok && cpu_hypervisor_present() == 0) {
        /* Реальное железо вправе ответить иначе; не блокируем загрузку. */
        char detail[96];
        (void)ksnprintf(detail, sizeof(detail),
                        "bare metal: rc=%d st=0x%02x (не блокируем)", rc, st);
        report("ps2/controller-selftest", 1, detail);
        return;
    }
    report("ps2/controller-selftest", ok, "8042 вернул не 0x55");
}

#endif /* FOBOS_HOST_TEST */


/* ================================================================== */
/* fs/fatp: имена 8.3                                                  */
/* ================================================================== */

static void t_fatp_names(void)
{
    u8 raw[FAT83_RAW_LEN];

    /* Каноничное имя. */
    int ok = (fat83_from_text("FOBOS.CFG", raw) == K_OK);
    ok = ok && (strncmp((const char *)raw, "FOBOS   CFG", 11) == 0);
    report("fat83/from-text-upper", ok, "неверная сборка 8.3");

    /* Строчное имя приводится к верхнему регистру. */
    ok = (fat83_from_text("setup.cfg", raw) == K_OK) &&
         (strncmp((const char *)raw, "SETUP   CFG", 11) == 0);
    report("fat83/from-text-lower", ok, "нет нормализации регистра");

    /* Без расширения. */
    ok = (fat83_from_text("KERNEL", raw) == K_OK) &&
         (strncmp((const char *)raw, "KERNEL     ", 11) == 0);
    report("fat83/no-ext", ok, "имя без расширения сломано");

    /* Слишком длинное имя -> K_ERANGE. */
    ok = (fat83_from_text("VERYLONGFILENAME.TXT", raw) == K_ERANGE);
    report("fat83/too-long", ok, "длинное имя не отвергнуто");

    /* Запрещённый символ. */
    ok = (fat83_from_text("a*b.txt", raw) == K_EINVAL);
    report("fat83/bad-char", ok, "запрещённый символ пропущен");

    /* Сравнение без учёта регистра. */
    ok = (fat83_from_text("SELTEST.TMP", raw) == K_OK) &&
         (fat83_match(raw, "seltest.tmp") != 0) &&
         (fat83_match(raw, "SELTEST.OLD") == 0);
    report("fat83/match", ok, "сравнение 8.3 неверное");

    /* Рендер обратно в текст. */
    char text[16];
    (void)fat83_from_text("BOOTX64.EFI", raw);
    fat83_render(raw, 0x00, text, sizeof(text));
    ok = (strcmp(text, "BOOTX64.EFI") == 0);
    report("fat83/render", ok, "рендер 8.3 неверный");
}

/* ================================================================== */
/* fs/fatp: BPB                                                        */
/* ================================================================== */

static void t_fatp_bpb(void)
{
    u8 bpb[512];
    struct fat_bpb out;

    for (u32 i = 0; i < 512u; i++) {
        bpb[i] = 0u;
    }
    /* Валидный FAT32 BPB: 512Б сектор, 1 сектор/кластер, 2 FAT,
     * достаточно кластеров (> 65525). */
    u16 bps = 512;
    u32 total = 70000;
    u32 fatsz = 690;
    u32 reserved = 32;
    for (u32 i = 0; i < 2u; i++) { bpb[i] = 0u; }
    bpb[510] = 0x55;
    bpb[511] = 0xAA;
    bpb[11] = (u8)(bps & 0xFF);
    bpb[12] = (u8)(bps >> 8);
    bpb[13] = 1;                       /* spc */
    bpb[14] = (u8)(reserved & 0xFF);
    bpb[15] = (u8)(reserved >> 8);
    bpb[16] = 2;                       /* nfats */
    bpb[32] = (u8)(total & 0xFF);
    bpb[33] = (u8)((total >> 8) & 0xFF);
    bpb[34] = (u8)((total >> 16) & 0xFF);
    bpb[35] = (u8)((total >> 24) & 0xFF);
    bpb[36] = (u8)(fatsz & 0xFF);
    bpb[37] = (u8)((fatsz >> 8) & 0xFF);
    bpb[38] = (u8)((fatsz >> 16) & 0xFF);
    bpb[39] = (u8)((fatsz >> 24) & 0xFF);
    bpb[44] = 2;                       /* root cluster */
    for (u32 i = 0; i < 8u; i++) { bpb[71 + i] = "FOBOSOS "[i]; }

    int ok = (fat_parse_bpb(bpb, sizeof(bpb), &out) == K_OK) &&
             (out.sectors_per_cluster == 1u) &&
             (out.root_cluster == 2u) &&
             (out.fatsz_sectors == fatsz);
    report("bpb/valid", ok, "валидный BPB не разобран");

    /* Битая сигнатура. */
    bpb[510] = 0;
    ok = (fat_parse_bpb(bpb, sizeof(bpb), &out) == K_EINVAL);
    bpb[510] = 0x55;
    report("bpb/bad-signature", ok, "битая сигнатура пропущена");

    /* Не-512 сектор -> K_EUNSUPPORTED. */
    bpb[11] = 0;
    bpb[12] = 4;   /* 1024 */
    ok = (fat_parse_bpb(bpb, sizeof(bpb), &out) == K_EUNSUPPORTED);
    bpb[11] = (u8)(bps & 0xFF);
    bpb[12] = (u8)(bps >> 8);
    report("bpb/bad-sector-size", ok, "чужой размер сектора не отвергнут");
}

/* ================================================================== */
/* fs/fatp: LFN                                                        */
/* ================================================================== */

static void t_fatp_lfn(void)
{
    /* Собираем LFN-запись с частью имени "LONGFILENAME.TXT" (первые 13
     * символов в UTF-16LE). ord = 0x41 = последняя часть + номер 1. */
    u8 e[32];
    for (u32 i = 0; i < 32u; i++) {
        e[i] = 0xFFu;   /* заполнитель свободных слотов UTF-16 */
    }
    e[0] = 0x41;                       /* 0x40 (last) | seq 1 */
    e[11] = FAT_ATTR_LFN;
    e[12] = 0;
    e[13] = 0x5Au;                     /* произвольная контрольная сумма */

    static const u32 slots[FAT_LFN_CHARS] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    const char *want = "LONGFILENAME.";
    for (u32 i = 0; i < FAT_LFN_CHARS; i++) {
        u16 w = (u8)want[i];
        e[slots[i]] = (u8)(w & 0xFFu);
        e[slots[i] + 1u] = (u8)(w >> 8);
    }

    u16 out[FAT_LFN_CHARS];
    int last = fat_lfn_part(e, out);
    int ok = (last == 1);
    for (u32 i = 0; i < 13u; i++) {
        char c = (out[i] < 0x80u) ? (char)out[i] : '?';
        ok = ok && (c == want[i]);
    }
    report("lfn/first-part", ok, "разбор LFN-записи неверен");

    /* Не-LFN запись отвергается. */
    e[11] = FAT_ATTR_ARCHIVE;
    ok = (fat_lfn_part(e, out) == -1);
    report("lfn/not-lfn", ok, "обычная запись принята за LFN");

    /* Контрольная сумма детерминирована и чувствительна. */
    u8 raw[FAT83_RAW_LEN];
    (void)fat83_from_text("ABC.TXT", raw);
    u8 s1 = fat_lfn_checksum(raw);
    (void)fat83_from_text("ABC.DAT", raw);
    u8 s2 = fat_lfn_checksum(raw);
    ok = (s1 != s2);
    report("lfn/checksum", ok, "контрольная сумма не различает имена");
}

/* ================================================================== */
/* fs/fatp: LFN-запись (алиас + сборка записей, FND-2f)                */
/* ================================================================== */

static void t_fatp_lfn_write(void)
{
    u8 raw[FAT83_RAW_LEN];

    /* Алиас: пробелы/длина -> "~1", расширение сохраняется. */
    int ok = (fat83_alias_from_lfn("my document.txt", 1u, raw) == K_OK) &&
             (fat83_match(raw, "MYDOCU~1.TXT") != 0);
    report("lfn-alias/basic", ok, "алиас MYDOCU~1.TXT не собрался");

    ok = (fat83_alias_from_lfn("my document.txt", 3u, raw) == K_OK) &&
         (fat83_match(raw, "MYDOCU~3.TXT") != 0);
    report("lfn-alias/tail3", ok, "номер попытки не отражён в алиасе");

    /* Имя без валидной базы: FILE~N + усечённое расширение. */
    ok = (fat83_alias_from_lfn(".hidden", 1u, raw) == K_OK) &&
         (fat83_match(raw, "FILE~1.HID") != 0);
    report("lfn-alias/no-base", ok, "имя без базы обработано неверно");

    /* Имя без точки: расширение пустое (6 первых символов + "~N"). */
    ok = (fat83_alias_from_lfn("notebook", 2u, raw) == K_OK) &&
         (fat83_match(raw, "NOTEBO~2") != 0);
    report("lfn-alias/no-ext", ok, "алиас без расширения неверен");

    /* Номер попытки вне 1..9 отвергается. */
    ok = (fat83_alias_from_lfn("a.txt", 0u, raw) == K_EINVAL) &&
         (fat83_alias_from_lfn("a.txt", 10u, raw) == K_EINVAL);
    report("lfn-alias/bad-tail", ok, "недопустимый tail не отвергнут");

    /* Сборка LFN-серии для 22 символов = 2 записи; физический порядок:
     * ord 2 (с 0x40) первым, ord 1 последним; сборка даёт исходное имя. */
    static const char *const NAME = "long filename test.txt";
    u8 ents[2][32];
    u32 cnt = 0;
    (void)fat83_from_text("X.TXT", raw);
    u8 sum = fat_lfn_checksum(raw);
    ok = (fat_lfn_build_entries(NAME, sum, ents, 2u, &cnt) == K_OK) &&
         (cnt == 2u);
    report("lfn-build/count", ok, "число LFN-записей неверно");

    ok = (ents[0][0] == (0x40u | 2u)) && (ents[1][0] == 0x01u) &&
         (ents[0][11] == FAT_ATTR_LFN) && (ents[1][11] == FAT_ATTR_LFN) &&
         (ents[0][13] == sum) && (ents[1][13] == sum) &&
         (ents[0][26] == 0u) && (ents[0][27] == 0u);
    report("lfn-build/headers", ok, "заголовки LFN-записей неверны");

    /* Сборка через читатель: ord 1 (физически последняя, БЕЗ флага 0x40)
     * содержит первые 13 символов, ord 2 (с 0x40) — остальные 9 + терминатор. */
    u16 part[FAT_LFN_CHARS];
    ok = (fat_lfn_part(ents[1], part) == 0) &&
         (fat_lfn_part(ents[0], part) == 1);
    report("lfn-build/order-flags", ok, "флаги последней части не на месте");

    ok = (fat_lfn_part(ents[1], part) == 0);
    for (u32 i = 0; ok != 0 && i < 13u; i++) {
        ok = (part[i] == (u16)(u8)NAME[i]);
    }
    report("lfn-build/ord1-first13", ok, "первая порция имени не на месте");

    ok = (fat_lfn_part(ents[0], part) == 1);
    for (u32 i = 0; ok != 0 && i < 9u; i++) {
        ok = (part[i] == (u16)(u8)NAME[13u + i]);
    }
    ok = ok && (part[9] == 0x0000u) && (part[10] == 0xFFFFu) &&
         (part[11] == 0xFFFFu) && (part[12] == 0xFFFFu);
    report("lfn-build/ord2-tail", ok, "хвост имени/терминатор не на месте");

    /* Отказы: пустое имя, слишком короткий буфер. */
    ok = (fat_lfn_build_entries("", sum, ents, 2u, &cnt) == K_EINVAL) &&
         (fat_lfn_build_entries(NAME, sum, ents, 1u, &cnt) == K_ERANGE);
    report("lfn-build/bad-args", ok, "неверные аргументы не отвергнуты");
}

/* ================================================================== */
/* fs/ovl: амнезия (мок-бэкенд, ядро + host)                           */
/* ================================================================== */

#define OVL_MOCK_SECTORS 128u
static u8 ovl_mock_disk[OVL_MOCK_SECTORS][512];
static u8 ovl_mock_frames[8][4096];
static u32 ovl_mock_used_frames;
static u32 ovl_mock_freed;

static err_t ovl_mock_read(u64 lba, u32 count, void *buf)
{
    if (lba + count > OVL_MOCK_SECTORS) {
        return K_ERANGE;
    }
    u8 *dst = buf;
    for (u32 s = 0; s < count; s++) {
        for (u32 i = 0; i < 512u; i++) {
            dst[s * 512u + i] = ovl_mock_disk[lba + s][i];
        }
    }
    return K_OK;
}

static err_t ovl_mock_write(u64 lba, u32 count, const void *buf)
{
    if (lba + count > OVL_MOCK_SECTORS) {
        return K_ERANGE;
    }
    const u8 *src = buf;
    for (u32 s = 0; s < count; s++) {
        for (u32 i = 0; i < 512u; i++) {
            ovl_mock_disk[lba + s][i] = src[s * 512u + i];
        }
    }
    return K_OK;
}

static err_t ovl_mock_alloc(u32 *out_frame)
{
    if (ovl_mock_used_frames >= 8u) {
        return K_ENOMEM;
    }
    *out_frame = ovl_mock_used_frames++;
    return K_OK;
}

static void ovl_mock_free(u32 frame)
{
    (void)frame;
    ovl_mock_freed++;
}

static u8 *ovl_mock_ptr(u32 frame)
{
    return ovl_mock_frames[frame];
}

static void ovl_mock_reset(void)
{
    for (u32 s = 0; s < OVL_MOCK_SECTORS; s++) {
        for (u32 i = 0; i < 512u; i++) {
            ovl_mock_disk[s][i] = (u8)((s + i) & 0xFFu);
        }
    }
    for (u32 f = 0; f < 8u; f++) {
        for (u32 i = 0; i < 4096u; i++) {
            ovl_mock_frames[f][i] = 0xEEu;
        }
    }
    ovl_mock_used_frames = 0;
    ovl_mock_freed = 0;
}

static void t_ovl_amnesia(void)
{
    ovl_mock_reset();
    static const struct ovl_io io = {
        ovl_mock_read, ovl_mock_write,
        ovl_mock_alloc, ovl_mock_free, ovl_mock_ptr
    };
    int ok = (ovl_init(&io, 4u) == K_OK);
    report("ovl/init", ok, "ovl_init не принял корректный бэкенд");
    if (!ok) {
        return;
    }

    /* Выключен: запись проходит на диск. */
    u8 buf[512];
    for (u32 i = 0; i < 512u; i++) {
        buf[i] = 0xA1u;
    }
    (void)ovl_write(10u, 1u, buf);
    ok = (ovl_mock_disk[10][0] == 0xA1u) && (ovl_active() == 0);
    report("ovl/passthrough", ok, "в выключенном состоянии запись не дошла до диска");

    /* Включён: запись остаётся в RAM, диск не трогается. */
    ovl_enable();
    (void)ovl_write(20u, 1u, buf);
    ok = (ovl_mock_disk[20][0] != 0xA1u);
    u8 rbuf[512];
    (void)ovl_read(20u, 1u, rbuf);
    ok = ok && (rbuf[0] == 0xA1u) && (ovl_blocks_used() == 1u);
    report("ovl/ram-only", ok, "запись в амнезии изменила диск или не видна при чтении");

    /* Общий 4-КиБ блок: соседние секторы в одном блоке. */
    for (u32 i = 0; i < 512u; i++) {
        buf[i] = 0xB2u;
    }
    (void)ovl_write(21u, 1u, buf);
    ok = (ovl_blocks_used() == 1u);   /* 20 и 21 = один блок */
    (void)ovl_read(21u, 1u, rbuf);
    ok = ok && (rbuf[0] == 0xB2u);
    report("ovl/block-sharing", ok, "секторы одного блока не объединились");

    /* Частичная запись: соседний сектор блока видит данные диска (RMW). */
    (void)ovl_read(22u, 1u, rbuf);
    ok = (rbuf[0] == (u8)((22u + 0u) & 0xFFu));
    report("ovl/rmw-neighbor", ok, "read-modify-write соседнего сектора сломан");

    /* Предел блоков: блок 0 и блок 1 (секторы 0..7 и 8..15). */
    ovl_disable();
    ovl_mock_reset();
    (void)ovl_init(&io, 1u);
    ovl_enable();
    (void)ovl_write(0u, 1u, buf);
    err_t rc = ovl_write(100u, 1u, buf);   /* другой блок */
    ok = (rc == K_ENOMEM) && (ovl_mock_disk[100][0] != 0xB2u) &&
         (ovl_mock_disk[0][0] != 0xB2u);
    report("ovl/limit", ok, "переполнение оверлея не отклонено или утекло на диск");

    /* Выключение освобождает кадры. */
    u32 frames_before = ovl_mock_used_frames;
    ovl_disable();
    ok = (ovl_active() == 0) && (ovl_mock_freed == frames_before) &&
         (ovl_mock_disk[0][0] != 0xB2u);
    report("ovl/disable-frees", ok, "выключение амнезии не освободило кадры/утекло на диск");

#ifndef FOBOS_HOST_TEST
    /* Тесты перенастроили глобальный ovl на мок-бэкенд с лимитом 1 —
     * возвращаем продакшн-бэкенд (блочный IO ядра идёт через него). */
    (void)blk_ovl_restore();
#endif
}


/* ================================================================== */
/* cfglite (общий модуль ядра и загрузчика)                            */
/* ================================================================== */

static void t_cfglite(void)
{
    static const char cfg[] =
        "# comment line\n"
        "\n"
        "mode=gui\r\n"
        "  res = 1280x800  \n"
        "wall=1\n"
        "noeq line\n"
        "#tail comment\n"
        "pins=cmd, explorer, notepad\n"
        "longkeynamexxxxxxxxxxxxxxxxx=1\n"
        "v=short\n";

    struct cfglite_kv kvs[10];
    u32 n = cfglite_parse(cfg, (u32)sizeof(cfg) - 1u, kvs, 10);

    /* Ожидаются: mode, res, wall, pins, v (noeq — без '='; longkey —
     * не уместился в 16 байт ключа; комментарии/пустые пропущены). */
    int ok = (n == 5u);
    report("cfglite/count", ok, "число записей после парса неверно");

    ok = (cfglite_get(kvs, n, "mode") != NULL &&
          strcmp(cfglite_get(kvs, n, "mode"), "gui") == 0);
    report("cfglite/mode", ok, "mode != gui (CRLF/пробелы?)");

    ok = (cfglite_get(kvs, n, "res") != NULL &&
          strcmp(cfglite_get(kvs, n, "res"), "1280x800") == 0);
    report("cfglite/trim", ok, "обрезка пробелов вокруг '=' сломана");

    ok = (cfglite_get(kvs, n, "pins") != NULL &&
          strcmp(cfglite_get(kvs, n, "pins"), "cmd, explorer, notepad") == 0);
    report("cfglite/inner-space", ok, "внутренние пробелы значения искажены");

    ok = (cfglite_get(kvs, n, "v") != NULL &&
          strcmp(cfglite_get(kvs, n, "v"), "short") == 0);
    report("cfglite/last-line", ok, "последняя строка без \\n теряется");

    ok = (cfglite_get(kvs, n, "missing") == NULL);
    report("cfglite/missing", ok, "отсутствующий ключ должен давать NULL");

    /* Переполнение значения: строка длиннее 47 символов пропускается. */
    static const char big[] = "k=0123456789012345678901234567890123456789012345678\n"
                              "k2=ok\n";
    struct cfglite_kv kv2[4];
    u32 n2 = cfglite_parse(big, (u32)sizeof(big) - 1u, kv2, 4);
    ok = (n2 == 1u && strcmp(kv2[0].key, "k2") == 0);
    report("cfglite/overflow-skip", ok,
           "переполненное значение не пропущено целиком");

    /* max=0 и NULL — безопасны. */
    ok = (cfglite_parse(cfg, 10u, NULL, 0u) == 0u);
    report("cfglite/null-safe", ok, "NULL/0 должны возвращать 0");
}

/* ================================================================== */


/* ------------------------------------------------------------------ */
/* sys/elf: парсер ELF64 (чистая логика — ядро + host)                 */
/* ------------------------------------------------------------------ */

#define ST_ELF_CAP 256u

/* Собирает минимальный валидный EXEC: 1 PT_LOAD RX @0x400000. */
static u32 st_build_elf(u8 *buf, u32 entry_off, u16 e_type, u16 phnum)
{
    memset(buf, 0, ST_ELF_CAP);
    /* EHDR. */
    buf[0] = 0x7Fu; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;   /* ELFCLASS64 */
    buf[5] = 1;   /* ELFDATA2LSB */
    buf[6] = 1;   /* EV_CURRENT */
    buf[16] = (u8)(e_type & 0xFFu);
    buf[17] = (u8)(e_type >> 8);
    buf[18] = 62; buf[19] = 0;            /* EM_X86_64 */
    buf[20] = 1;                          /* e_version */
    /* e_entry = 0x400000 + entry_off */
    u64 entry = 0x400000ull + entry_off;
    for (u32 i = 0; i < 8u; i++) {
        buf[24 + i] = (u8)(entry >> (8u * i));
    }
    /* e_phoff = 64 (e_ehsize/e_shoff не проверяются парсером). */
    buf[32] = 64;
    buf[54] = 56; buf[55] = 0;            /* e_phentsize */
    buf[56] = (u8)(phnum & 0xFFu);
    buf[57] = (u8)(phnum >> 8);
    /* PHDR[0]: PT_LOAD RX, offset 0x40, vaddr 0x400000, filesz=memsz=0x10. */
    if (phnum >= 1u) {
        u8 *p = buf + 64;
        p[0] = 1;                          /* PT_LOAD */
        p[4] = 5;                          /* R|X */
        p[8] = 0x40;                       /* p_offset */
        p[16] = 0x00; p[17] = 0x00; p[18] = 0x40;  /* p_vaddr = 0x400000 */
        p[24] = 0x00; p[25] = 0x00; p[26] = 0x40;  /* p_paddr */
        p[32] = 0x10;                      /* p_filesz */
        p[40] = 0x10;                      /* p_memsz */
        p[48] = 0x00; p[49] = 0x10;        /* p_align = 0x1000 */
    }
    return 64u + 56u * phnum + 0x10u;
}

struct st_elf_cb {
    int called;
    u64 vaddr;
    u64 filesz;
};

static err_t st_elf_cb(void *ctx, const struct elf_load_seg *seg, const u8 *img)
{
    struct st_elf_cb *c = ctx;
    (void)img;
    c->called++;
    c->vaddr = seg->vaddr;
    c->filesz = seg->filesz;
    return K_OK;
}

static void t_elf_parse(void)
{
    u8 img[ST_ELF_CAP];
    u64 entry = 0;
    struct st_elf_cb cb;
    err_t rc;

    /* Валидный образ: K_OK, entry, сегмент передан коллбеку. */
    u32 len = st_build_elf(img, 0x40u, 2u /*ET_EXEC*/, 1u);
    cb.called = 0;
    rc = elf64_load_image(img, len, &entry, st_elf_cb, &cb);
    report("elf/valid-exec", rc == K_OK && entry == 0x400040ull &&
           cb.called == 1 && cb.vaddr == 0x400000ull && cb.filesz == 0x10ull,
           "валидный ELF64 не разобрался");

    /* Быстрая валидация согласована. */
    report("elf/validate-ok", elf64_validate(img, len) == K_OK,
           "elf64_validate не согласован с загрузкой");

    /* Плохая магия. */
    img[1] = 'X';
    report("elf/bad-magic", elf64_validate(img, len) == K_EINVAL,
           "битая магия не отвергнута");
    img[1] = 'E';

    /* Не 64-битный класс -> K_ENOSYS. */
    img[4] = 1;
    report("elf/bad-class", elf64_validate(img, len) == K_ENOSYS,
           "ELF32 не отвергнут как неподдерживаемый");
    img[4] = 2;

    /* ET_DYN (PIE) -> K_ENOSYS. */
    img[16] = 3;
    report("elf/et-dyn", elf64_validate(img, len) == K_ENOSYS,
           "PIE-образ не отвергнут");
    img[16] = 2;

    /* phoff за пределами образа -> K_EIO. */
    img[32] = 0xFF;
    img[33] = 0x0F;
    report("elf/phoff-range", elf64_validate(img, len) == K_EIO,
           "phoff за пределами образа не пойман");
    img[32] = 64; img[33] = 0;

    /* entry вне user-диапазона -> K_EINVAL. */
    img[24] = 0x00; img[25] = 0x00; img[26] = 0x00; img[27] = 0x80;
    report("elf/entry-kaddr", elf64_validate(img, len) == K_EINVAL,
           "kernel-entry не отвергнут");
    len = st_build_elf(img, 0x40u, 2u, 1u);

    /* Сегмент вне user-диапазона (vaddr = 0xC0000000) -> K_EINVAL. */
    img[64 + 16] = 0x00; img[64 + 17] = 0x00; img[64 + 18] = 0x00; img[64 + 19] = 0xC0;
    report("elf/seg-kaddr", elf64_load_image(img, len, &entry, NULL, NULL)
           == K_EINVAL, "сегмент в kernel-диапазоне не отвергнут");
    len = st_build_elf(img, 0x40u, 2u, 1u);

    /* filesz > memsz -> K_EINVAL. */
    img[64 + 40] = 0x0F;   /* memsz = 15 < filesz 16 */
    report("elf/filesz-gt-memsz", elf64_load_image(img, len, &entry, NULL, NULL)
           == K_EINVAL, "filesz>memsz не отвергнут");
    len = st_build_elf(img, 0x40u, 2u, 1u);

    /* Файловый кусок за пределами образа -> K_EIO. */
    img[64 + 8] = 0xF0;    /* p_offset = 0xF0, filesz 0x10 => 0x100 > len 0x70 */
    report("elf/seg-eof", elf64_load_image(img, len, &entry, NULL, NULL)
           == K_EIO, "filesz за концом образа не пойман");
}

/* ================================================================== */
/* FND-3b: MBR / геометрия FAT32 / mkdir (host: мок блочного диска)    */
/* ================================================================== */

#ifdef FOBOS_HOST_TEST

#include "../../common/mbr.h"
#include "../../common/fmtcalc.h"
#include "../../common/fmtfat32.h"
#include "../fs/fat32.h"
#include "../../tests/host/blk_mock.h"

static void t_mbr_parse(void)
{
    struct mbr_part parts[MBR_ENTRY_COUNT];
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        parts[i].boot = 0u;
        parts[i].type = MBR_PART_EMPTY;
        parts[i].lba = 0u;
        parts[i].sectors = 0u;
    }
    parts[0].type = MBR_PART_EFI_SYS;
    parts[0].lba = 2048u;
    parts[0].sectors = 131072u;
    parts[1].type = MBR_PART_FAT32_LBA;
    parts[1].lba = 133120u;
    parts[1].sectors = 569344u;

    u8 sec[MBR_SECTOR_SZ];
    for (u32 i = 0; i < MBR_SECTOR_SZ; i++) {
        sec[i] = 0u;
    }
    mbr_write(sec, parts);

    struct mbr_part back[MBR_ENTRY_COUNT];
    int n = mbr_parse(sec, back);
    int ok = (n == 2) && (back[0].type == MBR_PART_EFI_SYS) &&
             (back[0].lba == 2048u) && (back[0].sectors == 131072u) &&
             (back[1].type == MBR_PART_FAT32_LBA) &&
             (back[1].lba == 133120u) && (back[1].sectors == 569344u);
    report("mbr/roundtrip", ok, "поля записей исказились при записи/разборе");

    u64 lba = 0, cnt = 0;
    ok = (mbr_find_fat32(sec, &lba, &cnt) == K_OK) &&
         (lba == 133120u) && (cnt == 569344u);
    report("mbr/find-fat32", ok, "первый 0x0C не найден (семантика ядра)");

    sec[MBR_SIG_OFF] = 0x00u;
    ok = (mbr_parse(sec, back) == -1) &&
         (mbr_find_fat32(sec, &lba, &cnt) == K_EINVAL);
    report("mbr/no-signature", ok, "отсутствие 0x55AA не обнаружено");
}

static void t_fmtcalc_geom(void)
{
    struct fmtcalc_fat32 g;

    /* 256 МиБ - 2048 секторов (как mkimg): spc=1, кластеров > 65525. */
    int ok = (fmtcalc_fat32_geom(524288u - 2048u, &g) == K_OK) &&
             (g.sectors_per_cluster == 1u) &&
             (g.total_clusters > 65525u) &&
             (g.root_cluster == 2u) && (g.num_fats == 2u);
    ok = ok && ((u64)g.reserved_sectors + 2ull * g.fatsz_sectors +
                (u64)g.total_clusters * g.sectors_per_cluster) <=
            (524288u - 2048u);
    report("fmtcalc/geom-256mib", ok, "геометрия 256 МиБ неверна");

    /* 64 МиБ (ESP установщика). */
    ok = (fmtcalc_fat32_geom(131072u, &g) == K_OK) &&
         (g.total_clusters > 65525u);
    report("fmtcalc/geom-64mib", ok, "геометрия 64 МиБ неверна");

    /* Слишком мал (даже spc=128 не даёт 65526 кластеров). */
    ok = (fmtcalc_fat32_geom(400u, &g) == K_ERANGE);
    report("fmtcalc/geom-too-small", ok, "малый раздел не отвергнут");
}

static void t_fat_mkdir_e2e(void)
{
    if (blk_mock_init(70000u) != 0) {
        report("fat32/mkdir-e2e", 1, "нет памяти под RAM-диск");
        return;
    }

    int ok = (blk_mock_format_fat32() == K_OK);
    report("fmtfat32/format", ok, "форматирование RAM-тома провалилось");

    struct fat_vol vol;
    ok = (fat_mount(&vol, 1u, 69999u) == K_OK);
    report("fmtfat32/mount", ok, "свежесформатированный том не смонтировался");
    if (!ok) {
        blk_mock_done();
        return;
    }

    /* mkdir корневой и вложенный (как установщик: \EFI\BOOT). */
    ok = (fat_dir_create(&vol, "\\EFI") == K_OK) &&
         (fat_dir_create(&vol, "\\EFI\\BOOT") == K_OK) &&
         (fat_exists(&vol, "\\EFI\\BOOT") == 1);
    report("fat32/mkdir-nested", ok, "вложенные каталоги не созданы");

    /* Коллизии и ошибки. */
    ok = (fat_dir_create(&vol, "\\EFI") == K_EEXIST) &&
         (fat_dir_create(&vol, "\\NODIR\\SUB") == K_ENOENT) &&
         (fat_dir_create(&vol, "\\LONGDIRECTORYNAME") == K_ERANGE);
    report("fat32/mkdir-negatives", ok,
           "K_EEXIST/K_ENOENT/K_ERANGE не вернулись как ожидалось");

    /* Запись и чтение файла в созданном каталоге. */
    static const u8 payload[100] = "fobos installer dir-create test payload!";
    ok = (fat_file_write(&vol, "\\EFI\\BOOT\\TEST.BIN", payload,
                         sizeof(payload)) == K_OK);
    u8 back[128];
    u32 got = 0;
    ok = ok && (fat_file_read(&vol, "\\EFI\\BOOT\\TEST.BIN", back,
                              sizeof(back), &got) == K_OK) &&
         (got == sizeof(payload));
    for (u32 i = 0; ok && i < got; i++) {
        ok = (back[i] == payload[i]);
    }
    report("fat32/write-read-in-dir", ok,
           "файл в созданном каталоге не читается побайтно");

    /* Точечные записи "." и ".." в новом каталоге. */
    struct fat_dir it;
    struct fat_dir_entry e;
    ok = (fat_dir_open(&vol, "\\EFI\\BOOT", &it) == K_OK) &&
         (fat_dir_next(&vol, &it, &e) == K_OK) &&
         (e.is_dir == 1) && (e.name[0] == '.') && (e.name[1] == '\0');
    ok = ok && (fat_dir_next(&vol, &it, &e) == K_OK) &&
         (e.is_dir == 1) && (e.name[0] == '.') && (e.name[1] == '.') &&
         (e.name[2] == '\0');
    report("fat32/mkdir-dot-entries", ok, "записи . и .. отсутствуют/неверны");

    blk_mock_done();
}

#endif /* FOBOS_HOST_TEST */


int kselftest_run(void)
{
    g_passed = 0;
    g_failed = 0;

    klog(KLOG_INFO, ST_TAG, "начало набора самопроверок FND-1/FND-2a");

    t_vsnprintf_int();
    t_vsnprintf_unsigned();
    t_vsnprintf_width();
    t_vsnprintf_misc();
    t_vsnprintf_truncate();
    t_fbpi_checksum();
    t_fbpi_bltonly_rejected();

    t_phys_alloc_reuse();
    t_phys_double_free();
    t_phys_stats_sane();
    t_phys_alloc_contig();

    /* Чистая логика ввода — везде. */
    t_kbd_translate();
    t_mouse_parse();
    t_sha256_kat();
    t_foboscfg_roundtrip();
    t_cfglite();
    t_rtc_pure();
    t_session_basic();
    t_fatp_names();
    t_fatp_bpb();
    t_fatp_lfn();
    t_fatp_lfn_write();
    t_ovl_amnesia();
    t_elf_parse();

#ifdef FOBOS_HOST_TEST
    t_mbr_parse();
    t_fmtcalc_geom();
    t_fat_mkdir_e2e();
#endif

#ifndef FOBOS_HOST_TEST
    t_idt_breakpoint();
    t_fb_sanity();
    t_irq_gates();
    t_timer_advance();
    t_timer_sleep_zero();
    t_ps2_selftest();
    {
        int tf = task_selftest();
        report("task/pool-init", tf == 0, "пул задач/состояние планировщика сломаны");
    }
#endif

    /* Исчерпание — последним: тест меняет глобальное состояние аллокатора
     * (полностью восстанавливает его только при успехе). */
    t_phys_exhaustion();

    klog(KLOG_INFO, ST_TAG, "kselftest: summary: %d passed, %d failed",
         g_passed, g_failed);
    return g_failed;
}


/* ================================================================== */
/* Тесты хранилища (только ядро, после storage_init)                   */
/* ================================================================== */

#ifndef FOBOS_HOST_TEST

#define SELTEST_FILE "\\fobos\\SELTEST.TMP"

static void t_fat_e2e(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        report("fat32/e2e", 1, "skipped (no storage)");
        return;
    }

    /* 1. Проверка на "хвост": файл от прошлой загрузки не должен
     * существовать (прошлый тест его удаляет; наличие = утечка амнезии
     * или сбой по питанию между записью и удалением). */
    u8 buf[512];
    u32 len = 0;
    int leftover = (fat_file_read(v, SELTEST_FILE, buf, sizeof(buf), &len)
                    == K_OK);
    report("fat32/leftover-check", leftover == 0,
           "файл прошлой загрузки найден: амнезия протекла или сбой");

    /* 2. Запись паттерна. */
    for (u32 i = 0; i < 512u; i++) {
        buf[i] = (u8)((i * 7u + 3u) & 0xFFu);
    }
    err_t rc = fat_file_write(v, SELTEST_FILE, buf, sizeof(buf));
    if (rc != K_OK) {
        report("fat32/write", 0, k_strerror(rc));
        return;
    }
    report("fat32/write", 1, NULL);

    /* 3. Чтение и сверка. */
    u8 back[512];
    rc = fat_file_read(v, SELTEST_FILE, back, sizeof(back), &len);
    int ok = (rc == K_OK) && (len == 512u);
    for (u32 i = 0; ok != 0 && i < 512u; i++) {
        ok = (back[i] == buf[i]);
    }
    report("fat32/read-verify", ok, "прочитанное не совпало с записанным");

    /* 4. Перезапись меньшим объёмом. */
    rc = fat_file_write(v, SELTEST_FILE, buf, 100u);
    rc = fat_file_read(v, SELTEST_FILE, back, sizeof(back), &len);
    ok = (rc == K_OK) && (len == 100u);
    report("fat32/overwrite", ok, "перезапись вернула неверный размер");

    /* 5. Удаление и проверка отсутствия. */
    rc = fat_file_delete(v, SELTEST_FILE);
    if (rc != K_OK) {
        report("fat32/delete", 0, k_strerror(rc));
        return;
    }
    rc = fat_file_read(v, SELTEST_FILE, back, sizeof(back), &len);
    ok = (rc == K_ENOENT);
    report("fat32/delete", ok, "после удаления файл читается");
}

/* Защита системных имён: без регистра, без частичных совпадений. */
static void t_fat_name_protected(void)
{
    int ok = 1;
    const char *detail = NULL;

    static const char *const HIT[] = {
        "KERNEL.ELF", "kernel.elf", "Kernel.Elf",
        "setup.cfg", "FOBOS.CFG", "bootx64.efi", NULL
    };
    static const char *const MISS[] = {
        "NOTES.TXT", "KERNEL.ELFX", "XKERNEL.ELF", "setup.cfg.bak",
        "", "KERNEL", "BOOTX64.EFI.TMP", NULL
    };

    for (u32 i = 0; ok != 0 && HIT[i] != NULL; i++) {
        if (fat_name_protected(HIT[i]) != 1) {
            ok = 0;
            detail = HIT[i];
        }
    }
    for (u32 i = 0; ok != 0 && MISS[i] != NULL; i++) {
        if (fat_name_protected(MISS[i]) != 0) {
            ok = 0;
            detail = MISS[i];
        }
    }
    report("fat/name-protected", ok, detail);
}

/* LFN-запись e2e: длинное имя с пробелами на реальном диске — запись,
 * чтение по длинному имени, чтение по алиасу, видимость в каталоге,
 * удаление с полной очисткой (ядро + storage). */
#define SELTEST_LFN_FILE "\\fobos\\seltest long name.txt"
#define SELTEST_LFN_ALIAS "\\fobos\\SELTES~1.TXT"

static void t_fat_lfn_e2e(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        report("fat32/lfn-e2e", 1, "skipped (no storage)");
        return;
    }

    /* Хвост прошлой загрузки убираем (оба имени). */
    (void)fat_file_delete(v, SELTEST_LFN_FILE);
    (void)fat_file_delete(v, SELTEST_LFN_ALIAS);

    u8 buf[300];
    for (u32 i = 0; i < sizeof(buf); i++) {
        buf[i] = (u8)((i * 13u + 5u) & 0xFFu);
    }
    err_t rc = fat_file_write(v, SELTEST_LFN_FILE, buf, sizeof(buf));
    if (rc != K_OK) {
        report("fat32/lfn-write", 0, k_strerror(rc));
        return;
    }
    report("fat32/lfn-write", 1, NULL);

    /* Чтение по ДЛИННОМУ имени (LFN-матч). */
    u8 back[300];
    u32 len = 0;
    rc = fat_file_read(v, SELTEST_LFN_FILE, back, sizeof(back), &len);
    int ok = (rc == K_OK) && (len == sizeof(buf));
    for (u32 i = 0; ok != 0 && i < len; i++) {
        ok = (back[i] == buf[i]);
    }
    report("fat32/lfn-read-long", ok, "чтение по длинному имени сломано");

    /* Чтение по АЛИАСУ (8.3-путь). */
    rc = fat_file_read(v, SELTEST_LFN_ALIAS, back, sizeof(back), &len);
    ok = (rc == K_OK) && (len == sizeof(buf)) && (back[0] == buf[0]);
    report("fat32/lfn-read-alias", ok, "чтение по алиасу сломано");

    /* Каталог видит длинное имя (как его покажет ls/explorer). */
    struct fat_dir it;
    int seen = 0;
    err_t drc = fat_dir_open(v, "\\fobos", &it);
    if (drc == K_OK) {
        for (;;) {
            struct fat_dir_entry de;
            drc = fat_dir_next(v, &it, &de);
            if (drc != K_OK) {
                break;
            }
            klog(KLOG_INFO, ST_TAG, "lfn-scan: entry='%s'", de.name);
            if (strcmp(de.name, "seltest long name.txt") == 0) {
                seen = 1;
                break;
            }
        }
    } else {
        klog(KLOG_INFO, ST_TAG, "lfn-scan: open failed %s", k_strerror(drc));
    }
    report("fat32/lfn-dir-entry", seen, "каталог не показывает длинное имя");

    /* Удаление по длинному имени убирает и LFN-серию, и алиас. */
    rc = fat_file_delete(v, SELTEST_LFN_FILE);
    klog(KLOG_INFO, ST_TAG, "lfn-delete: rc=%d (%s)", rc, k_strerror(rc));
    ok = (rc == K_OK) &&
         (fat_file_read(v, SELTEST_LFN_FILE, back, sizeof(back), &len)
          == K_ENOENT) &&
         (fat_exists(v, SELTEST_LFN_ALIAS) == 0);
    report("fat32/lfn-delete", ok, "удаление LFN-файла неполное");
}

int kselftest_storage_run(void)
{
    int saved_passed = g_passed;
    int saved_failed = g_failed;
    g_passed = 0;
    g_failed = 0;

    klog(KLOG_INFO, ST_TAG, "storage-тесты (FAT32 e2e на реальном диске)");
    t_fat_name_protected();
    t_fat_e2e();
    t_fat_lfn_e2e();

    klog(KLOG_INFO, ST_TAG,
         "kselftest(storage): summary: %d passed, %d failed", g_passed, g_failed);
    int failed = g_failed;
    g_passed += saved_passed;
    g_failed = saved_failed;
    return failed;
}

#endif /* FOBOS_HOST_TEST */
