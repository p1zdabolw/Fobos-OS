/*
 * Fobos OS — host-раннер unit-тестов (быстрая итерация без QEMU).
 *
 * Модель памяти: резервируется окно 0x100000..0x300000 через mmap(MAP_FIXED)
 * — оно эмулирует "физическую" RAM, на которой работает bitmap-аллокатор.
 * Мок-карта EFI построена вокруг этого окна:
 *   [0x100000, 0x101000)  EfiBootServicesData  (boot info — как в реальном загрузчике)
 *   [0x101000, 0x300000)  EfiConventional      (свободная RAM)
 *   [0x400000, 0x401000)  EfiMemoryMappedIO    (проверка резервирования)
 * Структура boot_info кладётся в начало окна (адрес 0x100000).
 *
 * Сборка/запуск: make host-tests
 */
#include <fobos/boot_info.h>
#include <fobos/errors.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/mman.h>

static void segv_handler(int sig)
{
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "\nhost-test: FATAL signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    _exit(139);
}

#include "../../kernel/selftest/selftest.h"
#include "../../kernel/mm/phys.h"

#define MOCK_WINDOW_BASE  0x100000ull
#define MOCK_WINDOW_SIZE  0x200000ull   /* 2 МиБ */
#define MOCK_MAX_PADDR    0x401000ull

static struct fbpi_efi_memdesc g_mock_descs[3];
static struct fbpi_boot_info *g_bi;

static int setup_mock(void)
{
    void *p = mmap((void *)(uintptr_t)MOCK_WINDOW_BASE, MOCK_WINDOW_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    fprintf(stderr, "host-test: mmap(0x%llx) = %p (errno=%d)\n",
            (unsigned long long)MOCK_WINDOW_BASE, p, p == MAP_FAILED ? 0 : 0);
    if (p == MAP_FAILED || (uintptr_t)p != MOCK_WINDOW_BASE) {
        fprintf(stderr, "host-test: cannot map mock window at 0x%llx\n",
                (unsigned long long)MOCK_WINDOW_BASE);
        return -1;
    }

    g_bi = (struct fbpi_boot_info *)(uintptr_t)MOCK_WINDOW_BASE;
    memset((void *)MOCK_WINDOW_BASE, 0, MOCK_WINDOW_SIZE);

    /* Дескрипторы карты памяти EFI. */
    g_mock_descs[0].type = FBPI_EFI_BOOT_SERVICES_DATA;
    g_mock_descs[0].physical_start = MOCK_WINDOW_BASE;
    g_mock_descs[0].number_of_pages = 1;
    g_mock_descs[1].type = FBPI_EFI_CONVENTIONAL;
    g_mock_descs[1].physical_start = MOCK_WINDOW_BASE + 0x1000ull;
    g_mock_descs[1].number_of_pages = (MOCK_WINDOW_SIZE - 0x1000ull) / 4096ull;
    g_mock_descs[2].type = FBPI_EFI_MMIO;
    g_mock_descs[2].physical_start = 0x400000ull;
    g_mock_descs[2].number_of_pages = 1;

    memset(g_bi, 0, sizeof(*g_bi));
    g_bi->magic = FBPI_MAGIC;
    g_bi->version = FBPI_VERSION;
    g_bi->header_size = (u16)sizeof(*g_bi);
    g_bi->kernel_size_bytes = 0x20000u;                  /* 128 КиБ */
    g_bi->kernel_vbase = 0xFFFFFFFF80180000ull;
    g_bi->kernel_pbase = 0x180000ull;                    /* внутри мок-окна */
    g_bi->efi_mmap_phys = (u64)(uintptr_t)g_mock_descs;
    g_bi->efi_mmap_size = (u32)sizeof(g_mock_descs);
    g_bi->efi_desc_size = (u32)sizeof(struct fbpi_efi_memdesc);
    g_bi->efi_desc_version = 1u;
    g_bi->fb.width = 64u;
    g_bi->fb.height = 64u;
    g_bi->fb.bpp = 32u;
    g_bi->fb.pitch_bytes = 256u;
    g_bi->fb.pixel_format = FBPI_PF_RGBX;
    memcpy(g_bi->firmware_vendor, "HostMock", 9u);
    g_bi->checksum = fbpi_compute_checksum(g_bi);

    return 0;
}

int main(void)
{
    signal(SIGSEGV, segv_handler);
    signal(SIGBUS, segv_handler);
    signal(SIGILL, segv_handler);
    if (setup_mock() != 0) {
        return 2;
    }
    if (fbpi_validate(g_bi) != K_OK) {
        fprintf(stderr, "host-test: mock boot info invalid\n");
        return 2;
    }
    err_t rc = phys_init(g_bi);
    if (rc != K_OK) {
        fprintf(stderr, "host-test: phys_init failed: %s\n", k_strerror(rc));
        return 2;
    }

    int failed = kselftest_run();
    printf("host-tests: %s (%d failed)\n", (failed == 0) ? "PASS" : "FAIL", failed);
    return (failed == 0) ? 0 : 1;
}
