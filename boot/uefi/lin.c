/*
 * Fobos OS — реализация ConIn-ввода загрузчика (см. lin.h).
 *
 * Эхо в ConOut НЕ используется: Setup рисует поля в framebuffer, а текстовый
 * ConOut наложился бы поверх графики. Редактирование строки — на стороне
 * OOBE (oobe_edit), которое перерисовывает поле на каждый ввод.
 */
#include "lin.h"

#include "efi.h"

static struct EFI_SYSTEM_TABLE *g_lst;
static struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *g_conin_ex;

int lin_init(struct EFI_SYSTEM_TABLE *st)
{
    if (st == NULL || st->con_in == NULL) {
        return -1;
    }
    g_lst = st;

    /* EX-протокол (состояния модификаторов) — опционален. */
    struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = NULL;
    EFI_STATUS st2 = st->boot_services->handle_protocol(
        st->console_in_handle, &EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID,
        (void **)&ex);
    if (!EFI_ERROR(st2) && ex != NULL) {
        g_conin_ex = ex;
    }
    return 0;
}

int lin_poll_key(u16 *out_scan, u16 *out_char)
{
    if (g_lst == NULL || g_lst->con_in == NULL) {
        return -1;
    }
    struct EFI_INPUT_KEY key;
    EFI_STATUS st = g_lst->con_in->read_key_stroke(g_lst->con_in, &key);
    if (EFI_ERROR(st)) {
        return 1; /* EFI_NOT_READY: нет событий */
    }
    if (out_scan != NULL) {
        *out_scan = key.scan_code;
    }
    if (out_char != NULL) {
        *out_char = key.unicode_char;
    }
    return 0;
}

/* 1 = Shift удерживается/был нажат (по EX-состоянию), 0 = нет, -1 = EX недоступен. */
int lin_poll_shift(void)
{
    if (g_conin_ex == NULL) {
        return -1;
    }
    struct EFI_KEY_DATA kd;
    EFI_STATUS st = g_conin_ex->read_key_stroke_ex(g_conin_ex, &kd);
    if (EFI_ERROR(st)) {
        return 0; /* нет событий; состояние shift без события не читается */
    }
    u32 ss = kd.key_state.key_shift_state;
    if ((ss & EFI_SHIFT_VALID) != 0u &&
        (ss & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0u) {
        return 1;
    }
    return 0;
}

int lin_poll_direct(u8 *out_code)
{
    if (out_code == NULL) {
        return -1;
    }
    for (u32 i = 0; i < 8u; i++) {
        u8 st;
        __asm__ volatile ("inb %1, %0" : "=a"(st) : "Nd"((u16)0x64u));
        if ((st & 0x01u) == 0u) {
            return 0; /* выходной буфер пуст */
        }
        u8 d;
        __asm__ volatile ("inb %1, %0" : "=a"(d) : "Nd"((u16)0x60u));
        if ((st & 0x20u) != 0u) {
            continue; /* байт мыши — не наш */
        }
        /* Один байт за вызов: интерпретация (Shift/ESC/I) — на стороне
         * окна (oobe_esc_window), чтобы ни один забранный байт не терялся.
         * Оставшиеся байты очереди достанутся следующим вызовам (окно
         * опрашивает каждые 5 мс) или драйверу прошивки. */
        *out_code = d;
        return 1;
    }
    return 0;
}

int lin_wait_key(u32 timeout_ms, u16 *out_scan, u16 *out_char)
{
    if (g_lst == NULL) {
        return -1;
    }
    u64 waited = 0;
    while (waited < timeout_ms) {
        if (lin_poll_key(out_scan, out_char) == 0) {
            return 0;
        }
        g_lst->boot_services->stall(1000u); /* 1 мс */
        waited += 1u;
    }
    return 1;
}
