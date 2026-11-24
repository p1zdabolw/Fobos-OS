/*
 * Fobos OS — kmain: последовательность инициализации FND-2b.
 *
 * Технические этапы: serial -> log -> boot-info -> GDT -> IDT(48) -> phys
 * -> PIC/IRQ -> PIT -> 8042/PS2 -> kbd/mouse -> fb -> sti -> kselftest
 * -> RTC -> uidraw.
 *
 * Поток загрузки (system startup):
 *   1. splash (3 c): подсказка "Hold SHIFT for the kernel console";
 *      удержание Shift -> режим kernel console (kshell, обслуживание).
 *   2. Конфиг системы: блоб foboscfg из boot_info (создан OOBE в загрузчике);
 *      если блога нет или пользователей 0 — экран "setup required"
 *      (Enter -> перезагрузка в Setup).
 *   3. Экран входа: имя+пароль (KDF-проверка), F2/F3 — reboot/shutdown.
 *   4. Рабочий стол: UI + kshell от имени пользователя; logout/смена
 *      пользователя возвращают на экран входа.
 *
 * Глобальные хоткеи: Ctrl+Alt+Del — перезагрузка (в любом режиме).
 */
#include <fobos/boot_info.h>
#include <fobos/errors.h>

#include <stddef.h>

#include "version.h"
#include "arch/arch.h"
#include "arch/pic.h"
#include "arch/irq.h"
#include "drivers/serial.h"
#include "drivers/fb.h"
#include "drivers/timer.h"
#include "drivers/ps2.h"
#include "drivers/kbd.h"
#include "drivers/mouse.h"
#include "drivers/rtc.h"
#include "gfx/console.h"
#include "gfx/ui.h"
#include "gfx/uidraw.h"
#include "gfx/splash.h"
#include "gfx/desktop.h"
#include "fs/blk.h"
#include "gfx/login.h"
#include "lib/log.h"
#include "lib/cpuid.h"
#include "lib/io.h"
#include "mm/phys.h"
#include "mm/vmm.h"
#include "selftest/selftest.h"
#include "sys/users.h"
#include "sys/session.h"
#include "sys/power.h"
#include "sys/task.h"
#include "sys/syscall.h"
#include "shell.h"

static const char *const BOOT_TAG = "boot";

static const char *fbpf_name(u8 pf)
{
    switch (pf) {
    case FBPI_PF_RGBX:    return "RGBX";
    case FBPI_PF_BGRX:    return "BGRX";
    case FBPI_PF_BITMASK: return "BITMASK";
    default:              return "BLTONLY";
    }
}

/* Мост klog -> fbcon (паника и диагностические строки видны на экране).
 * Порог синка ниже в kmain: в терминал попадают только WARN+ — INFO-поток
 * подсистем не засоряет консоль пользователя (спам виден в serial). */
static void log_console_sink(const char *line)
{
    con_write(line);
}

/* Ctrl+Alt+Del — перезагрузка в любом режиме (возврата нет). */
static int is_cad(const struct kbd_event *ev)
{
    return ev->code == KEY_DEL &&
           (ev->flags & KBD_FL_CTRL) != 0u &&
           (ev->flags & KBD_FL_ALT) != 0u;
}

/* Главный цикл интерактивного режима. Возвращает 0, если пользователь
 * вышел из сессии (logout) — экранному потоку; в консольном режиме
 * logout игнорируется и цикл бесконечен.
 *
 * Кадр FND-2d/2f: события собираются в пачку, затем:
 *   GUI — dsk_redraw() строит ВЕСЬ кадр (включая консоль терминала на
 *         z-позиции окна cmd) в задний буфер и делает fb_present();
 *   TUI — зона старого спрайта курсора перекрашивается ИЗ ЯЧЕЕК консоли
 *         (con_flush_rect_px), затем con_flush() отрисовывает изменения
 *         и uidraw_cursor_draw() печатает спрайт на новом месте: следы
 *         курсора в TUI невозможны по построению (FND-2f);
 *   в конце fb_present() переносит готовый кадр в VRAM (no-op, если
 *   с прошлого present ничего не менялось). */
static void main_loop(int console_mode)
{
    for (;;) {
        int did = 0;

        struct kbd_event kev;
        while (kbd_poll(&kev) == K_OK) {
            if (is_cad(&kev)) {
                machine_reboot();
            }
            /* GUI-роутинг (FND-2e): Start/меню/текстовые приложения
             * перехватывают ввод; остальное — kshell (терминал). */
            if (dsk_kbd_event(&kev) == 0) {
                shell_input(&kev);
            }
            did = 1;
        }

        struct mouse_event mev;
        while (mouse_poll(&mev) == K_OK) {
            dsk_mouse_event(&mev);
            did = 1;
        }

        if (dsk_statusbar_due() != 0) {
            dsk_statusbar_update();
            did = 1;
        }

        if (dsk_redraw_pending() != 0 ||
            (dsk_gui_active() != 0 && con_dirty() != 0 &&
             dsk_cmd_open() != 0)) {
            /* GUI: полный кадр композитора (консоль — в z-порядке окон). */
            dsk_redraw();
        } else if (dsk_gui_active() == 0) {
            /* TUI/консольный режим: консоль — верхний слой, курсор —
             * накладной слой кадра (FND-2f). */
            u32 sx, sy, sw, sh;
            if (uidraw_cursor_stamp_rect(&sx, &sy, &sw, &sh) != 0) {
                con_flush_rect_px(sx, sy, sw, sh);
            }
            con_flush();
            uidraw_cursor_draw();
        }
        fb_present();

        if (did == 0) {
            /* Idle-фаза задачи ядра: ждём прерывание, затем отдаём квант
             * user-задачам (FND-3a). Возврат в цикл — с IF=1. */
            __asm__ volatile ("sti; hlt; cli");
            sched_poll();
        }
        __asm__ volatile ("sti");

        /* logout работает только в сессионном (экранном) режиме. */
        if (console_mode == 0 && shell_logout_requested() != 0) {
            shell_clear_logout();
            return;
        }
    }
}

/* Старт рабочего стола в заданном режиме (TUI/GUI). 0 = успех. */
static int desktop_start(int mode)
{
    if (dsk_init(mode) != K_OK) {
        return -1;
    }
    klog_set_sink(log_console_sink);
    klog_set_sink_level(KLOG_WARN);   /* терминал — не журнал: только WARN+ */
    shell_init();
    return 0;
}

void kmain(const struct fbpi_boot_info *bi)
{
    serial_init(SERIAL_COM1);
    klog_init();
    klog_set_level(KLOG_DEBUG);

    klog(KLOG_INFO, BOOT_TAG, "FOBOS: kernel entry — Fobos OS %s", FOBOS_VERSION_STR);

    if (bi == NULL || fbpi_validate(bi) != K_OK) {
        klog_panic(BOOT_TAG, "FBPI: boot info не прошла валидацию (загрузчик/ядро разошлись по ABI)");
    }

    klog(KLOG_INFO, BOOT_TAG, "FBPI: прошивка \"%s\"", bi->firmware_vendor);
    klog(KLOG_INFO, BOOT_TAG,
         "ядро: vbase=0x%llx pbase=0x%llx размер=%llu КиБ",
         (unsigned long long)bi->kernel_vbase, (unsigned long long)bi->kernel_pbase,
         (unsigned long long)(bi->kernel_size_bytes >> 10));
    klog(KLOG_INFO, BOOT_TAG,
         "fb: %ux%u bpp=%u fmt=%s pitch=%llu байт, phys=0x%llx",
         (unsigned int)bi->fb.width, (unsigned int)bi->fb.height,
         (unsigned int)bi->fb.bpp, fbpf_name(bi->fb.pixel_format),
         (unsigned long long)bi->fb.pitch_bytes, (unsigned long long)bi->fb.phys_addr);

    err_t rc;

    rc = gdt_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "gdt_init: %s", k_strerror(rc));
    }

    rc = idt_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "idt_init: %s", k_strerror(rc));
    }

    rc = phys_init(bi);
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "phys_init: %s", k_strerror(rc));
    }

    /* FND-3a: TSS (RSP0/IST1) + NXE/SMAP/SMEP — до первого user-маппинга. */
    rc = tss_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "tss_init: %s", k_strerror(rc));
    }

    /* VMM: фиксирует CR3 загрузчика как прототип адресных пространств. */
    rc = vmm_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "vmm_init: %s", k_strerror(rc));
    }

    rc = fb_init(bi);
    int fb_ok = (rc == K_OK);
    if (fb_ok == 0) {
        klog(KLOG_WARN, BOOT_TAG, "fb_init: %s (графика будет недоступна)",
             k_strerror(rc));
    }

    rc = pic_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "pic_init: %s", k_strerror(rc));
    }

    rc = irq_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "irq_init: %s", k_strerror(rc));
    }

    rc = timer_init(TIMER_DEFAULT_HZ);
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "timer_init: %s", k_strerror(rc));
    }

    struct ps2_info ps2;
    for (u32 i = 0; i < sizeof(ps2); i++) {
        ((u8 *)&ps2)[i] = 0;
    }
    rc = ps2_init(&ps2);
    if (rc != K_OK) {
        /* Реальное железо: на части машин (ноутбуки, тонкие клиенты)
         * контроллера 8042 нет вообще. Паника здесь означала бы
         * невозможность загрузки; система продолжает работать — ввод
         * недоступен до появления соответствующих драйверов (xHCI — FND-2),
         * serial-консоль сохраняет работоспособность. */
        klog(KLOG_WARN, BOOT_TAG,
             "ps2_init: %s — PS/2-контроллер отсутствует/неисправен, "
             "клавиатура и мышь недоступны",
             k_strerror(rc));
    } else {
        rc = kbd_init();
        if (rc != K_OK) {
            klog(KLOG_WARN, BOOT_TAG, "kbd_init: %s (клавиатура недоступна)",
                 k_strerror(rc));
        }

        if (ps2.dual_channel != 0 && ps2.mouse_ok != 0) {
            rc = mouse_init();
            if (rc != K_OK) {
                klog(KLOG_WARN, BOOT_TAG, "mouse_init: %s (мышь недоступна)",
                     k_strerror(rc));
            }
        } else {
            klog(KLOG_WARN, BOOT_TAG,
                 "второй канал PS/2 отсутствует — мышь не инициализируется");
        }
    }

    /* Прерывания включаются ПОСЛЕ программирования всех контроллеров. */
    __asm__ volatile ("sti");
    klog(KLOG_INFO, BOOT_TAG, "прерывания включены (IF=1), PIC -> вектора 0x20..0x2F");

    /* Хранилище: PCI -> AHCI -> MBR -> FAT32. Не критично для загрузки:
     * без него работают все подсистемы, кроме файлов/персистентности. */
    err_t src = storage_init();
    int storage_ok = (src == K_OK);
    if (storage_ok != 0) {
        int failed_storage = kselftest_storage_run();
        if (failed_storage != 0) {
            klog(KLOG_ERROR, BOOT_TAG,
                 "kselftest(storage): %d тестов упало — загрузка прервана",
                 failed_storage);
            if (cpu_hypervisor_present() != 0) {
                qemu_debug_exit(QEMU_EXIT_FAIL);
            }
            klog_panic(BOOT_TAG, "остановка после падения storage-тестов");
        }
    } else {
        klog(KLOG_WARN, BOOT_TAG,
             "storage недоступно (%s): файлы и персистентность выключены",
             k_strerror(src));
    }

    int failed = kselftest_run();
    if (failed != 0) {
        klog(KLOG_ERROR, BOOT_TAG, "kselftest: %d тестов упало — загрузка прервана", failed);
        if (cpu_hypervisor_present() != 0) {
            qemu_debug_exit(QEMU_EXIT_FAIL);
        }
        klog_panic(BOOT_TAG, "остановка после падения самопроверок");
    }

    /* RTC: не критичен для загрузки. */
    (void)rtc_init();

    /* FND-3a: задачи + syscall ABI. Порядок: sched_init (idle = kmain,
 * хуки таймера/клавиатуры) -> syscall_init (MSR, EFER.SCE). До этого
 * момента прерывания уже включены, но user-задач нет — вытеснений нет. */
    rc = sched_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "sched_init: %s", k_strerror(rc));
    }
    rc = syscall_init();
    if (rc != K_OK) {
        klog_panic(BOOT_TAG, "syscall_init: %s", k_strerror(rc));
    }

    /* Уровень графики для splash/логина. Задний буфер выделяется ПОСЛЕ
     * kselftest (тест исчерпания обязан видеть полный пул кадров). */
    if (fb_ok != 0) {
        err_t bbrc = fb_backbuffer_init();
        if (bbrc != K_OK) {
            klog(KLOG_WARN, BOOT_TAG,
                 "backbuffer: %s — рисуем напрямую в VRAM (возможно мерцание)",
                 k_strerror(bbrc));
        }
        (void)uidraw_init();
        /* Кэш обоев (FND-2e): не критичен, при нехватке памяти фон
         * рисуется процедурно каждый кадр. */
        err_t wlrc = fb_walllayer_init();
        if (wlrc != K_OK) {
            klog(KLOG_WARN, BOOT_TAG, "walllayer: %s — обои без кэша",
                 k_strerror(wlrc));
        }
    }

    /* --- Поток загрузки FND-2b --- */

    /* Режим kernel console: по явному запросу загрузчика (Shift в окне
     * boot manager -> FBPI_BOOTF_CONSOLE) ИЛИ по удержанию Shift в окне
     * splash (3 c). Первый путь детерминирован (прошивочный ввод), второй
     * покрывает удержание Shift с момента включения (тип-матика реальной
     * клавиатуры даёт повторные make-коды в окне). */
    int console_mode;
    if ((bi->boot_flags & FBPI_BOOTF_CONSOLE) != 0u) {
        klog(KLOG_INFO, BOOT_TAG,
             "[boot] console: requested by boot manager (shift)");
        console_mode = 1;
    } else {
        console_mode = splash_wait(FOBOS_SPLASH_MS);
    }

    /* Конфиг системы: hostname + пользователи из блога загрузчика. */
    session_reset();
    users_reset();
    if (bi->cfg_phys != 0u && bi->cfg_size != 0u) {
        struct fobos_config cfg;
        err_t crc = foboscfg_decode((const void *)(u64)bi->cfg_phys,
                                    bi->cfg_size, &cfg);
        if (crc == K_OK) {
            session_set_hostname(cfg.hostname);
            crc = users_apply(&cfg);
            if (crc == K_OK) {
                klog(KLOG_INFO, BOOT_TAG,
                     "[boot] config: users=%u hostname=%s",
                     (unsigned int)users_count(), session_hostname());
            } else {
                klog(KLOG_ERROR, BOOT_TAG,
                     "[boot] config: пользователи из блога не применились (%s)",
                     k_strerror(crc));
            }
        } else {
            klog(KLOG_ERROR, BOOT_TAG,
                 "[boot] config: блоб настроек повреждён (%s) — продолжаем без него",
                 k_strerror(crc));
        }
    } else {
        klog(KLOG_WARN, BOOT_TAG,
             "[boot] config: блога настроек нет (OOBE не выполнялась?)");
    }

    /* Стартовый режим рабочего стола: флаг загрузчика (fobos.cfg,
     * прочитанный загрузчиком) + контрольное чтение \\fobos\\fobos.cfg
     * самим ядром (источник истины, если хранилище доступно). */
    int start_mode = DSK_TUI;
    int locked = ((bi->boot_flags & FBPI_BOOTF_GUI) != 0u) ? 1 : 0;
    if ((bi->boot_flags & FBPI_BOOTF_GUI) != 0u) {
        start_mode = DSK_GUI;
    }
    if (storage_ok != 0) {
        int file_mode = DSK_TUI;
        err_t frc = dsk_read_lock(&file_mode);
        if (frc == K_OK) {
            start_mode = file_mode;
            locked = 1;
            klog(KLOG_INFO, BOOT_TAG,
                 "[boot] fobos.cfg: mode=%s (locked)",
                 (file_mode == DSK_GUI) ? "gui" : "tui");
        } else if (frc == K_ENOENT) {
            klog(KLOG_INFO, BOOT_TAG,
                 "[boot] fobos.cfg: not found (default %s)",
                 (start_mode == DSK_GUI) ? "gui" : "tui");
        } else {
            klog(KLOG_WARN, BOOT_TAG, "[boot] fobos.cfg: %s",
                 k_strerror(frc));
        }
    }
    klog(KLOG_INFO, BOOT_TAG, "[boot] desktop: %s%s",
         (start_mode == DSK_GUI) ? "gui" : "tui",
         (locked != 0) ? " (locked)" : "");

    if (console_mode != 0) {
        /* Режим kernel console: диагностика/обслуживание, без сессии.
         * Стартуем в TUI; `fos == gui` доступен и здесь. */
        klog(KLOG_INFO, BOOT_TAG, "[boot] mode: kernel console (shift)");
        if (desktop_start(DSK_TUI) != 0) {
            klog(KLOG_WARN, BOOT_TAG, "UI недоступен: остаёмся в headless-режиме (serial)");
        }
        klog(KLOG_INFO, BOOT_TAG,
             "FOBOS: boot complete — %s, kernel console активна", FOBOS_STAGE_STR);
        main_loop(1);
        /* недостижимо: консольный режим не выходит по logout */
    }

    /* Экранный поток: вход / setup-required -> рабочий стол. */
    for (;;) {
        int user_idx = (fb_ok != 0) ? login_screen_run() : -1;
        if (user_idx == -1) {
            machine_reboot();
        }
        if (user_idx == -2) {
            machine_poweroff();
        }
        if (user_idx < 0) {
            /* Графики нет: работать в консольном режиме без сессии. */
            klog(KLOG_WARN, BOOT_TAG, "UI недоступен: остаёмся в headless-режиме (serial)");
            klog(KLOG_INFO, BOOT_TAG,
                 "FOBOS: boot complete — %s, headless", FOBOS_STAGE_STR);
            main_loop(1);
        }

        session_start((u32)user_idx);
        klog(KLOG_INFO, BOOT_TAG, "[session] opened user=%s", session_user());

        if (desktop_start(start_mode) != 0) {
            klog(KLOG_WARN, BOOT_TAG, "UI недоступен: сессия закрыта (serial-режим)");
            session_end();
            continue;
        }

        klog(KLOG_INFO, BOOT_TAG,
             "FOBOS: boot complete — %s, session=%s", FOBOS_STAGE_STR, session_user());
        main_loop(0);

        /* logout: возвращаемся на экран входа. */
        klog(KLOG_INFO, BOOT_TAG, "[session] closed (logout)");
        session_end();
        shell_deinit();
    }
}
