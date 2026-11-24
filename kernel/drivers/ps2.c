/*
 * Fobos OS — реализация контроллера 8042.
 *
 * Последовательность инициализации (описана в OSDev "8042 PS/2 Controller"):
 *   flush -> disable ports -> read config -> controller self-test ->
 *   interface tests -> enable ports -> включить IRQ и клок в конфиге.
 *
 * Трансляция сканкодов (бит 6 конфига) включена: драйвер клавиатуры
 * рассчитывает на сканкод-набор 1. Тайминги — ограниченные циклы с
 * io_wait, БЕЗ зависимости от PIT (init выполняется до sti).
 */
#include "ps2.h"
#include <stddef.h>
#include "../lib/io.h"
#include "../lib/log.h"

#define PS2_STATUS   0x64u
#define PS2_CMD      0x64u
#define PS2_DATA     0x60u

#define ST_OBF  0x01u   /* output buffer full */
#define ST_IBF  0x02u   /* input buffer full */

#define CMD_READ_CFG    0x20u
#define CMD_WRITE_CFG   0x60u
#define CMD_DISABLE_P1  0xADu
#define CMD_ENABLE_P1   0xAEu
#define CMD_DISABLE_P2  0xA7u
#define CMD_ENABLE_P2   0xA8u
#define CMD_SELFTEST    0xAAu
#define CMD_TEST_P1     0xABu
#define CMD_TEST_P2     0xA9u
#define CMD_WRITE_P2    0xD4u
#define CMD_PULSE       0xF0u   /* базовый nibble для pulse-out (0xF0|bits) */

#define CFG_P1_IRQ     0x01u
#define CFG_P2_IRQ     0x02u
#define CFG_P2_CLOCK   0x20u   /* 1 = второй порт отключён (clock) */
#define CFG_TRANSLATE  0x40u

/* Ограничение ожиданий: на реальном железе 8042 отвечает за микросекунды;
 * сотни тысяч итераций с io_wait — запас больше любого легитимного отклика. */
#define WAIT_LIMIT 200000u

static const char *const PS2_TAG = "drv/ps2";

static int wait_ibf_clear(void)
{
    for (u32 i = 0; i < WAIT_LIMIT; i++) {
        if ((inb(PS2_STATUS) & ST_IBF) == 0u) {
            return 1;
        }
        io_wait();
    }
    return 0;
}

static int wait_obf_set(void)
{
    for (u32 i = 0; i < WAIT_LIMIT; i++) {
        if ((inb(PS2_STATUS) & ST_OBF) != 0u) {
            return 1;
        }
        io_wait();
    }
    return 0;
}

static void flush_output(void)
{
    for (u32 i = 0; i < 64u; i++) {
        if ((inb(PS2_STATUS) & ST_OBF) == 0u) {
            return;
        }
        (void)inb(PS2_DATA);
        io_wait();
    }
}

static int send_cmd(u8 cmd)
{
    if (!wait_ibf_clear()) {
        return 0;
    }
    outb(PS2_CMD, cmd);
    return 1;
}

/* Запись байта в порт 1 (0x60) — общий примитив. */
static int send_port1_raw(u8 b)
{
    if (!wait_ibf_clear()) {
        return 0;
    }
    outb(PS2_DATA, b);
    return 1;
}

static int read_data_bounded(u8 *out)
{
    if (wait_obf_set()) {
        *out = inb(PS2_DATA);
        return 1;
    }
    return 0;
}

err_t ps2_read_data(u8 *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    if ((inb(PS2_STATUS) & ST_OBF) == 0u) {
        return K_EAGAIN;
    }
    *out = inb(PS2_DATA);
    return K_OK;
}

static int send_port1(u8 b)
{
    return send_port1_raw(b);
}

static int send_port2(u8 b)
{
    if (!send_cmd(CMD_WRITE_P2)) {
        return 0;
    }
    if (!wait_ibf_clear()) {
        return 0;
    }
    outb(PS2_DATA, b);
    return 1;
}

err_t ps2_write_port1(u8 b)
{
    return send_port1(b) ? K_OK : K_EIO;
}

err_t ps2_write_port2(u8 b)
{
    return send_port2(b) ? K_OK : K_EIO;
}

err_t ps2_controller_selftest(u8 *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    /* Дренаж: чужие байты устройств (мышь стримит) не должны маскировать
     * ответ контроллера. Затем фильтрующий цикл до кода самотеста. */
    flush_output();
    if (!send_cmd(CMD_SELFTEST)) {
        return K_EIO;
    }
    for (u32 i = 0; i < WAIT_LIMIT; i++) {
        u8 r = 0;
        if (read_data_bounded(&r)) {
            if (r == 0x55u || r == 0xFCu || r == 0xFDu) {
                *out = r; /* 0x55 = OK, 0xFC/0xFD = коды отказа 8042 */
                return K_OK;
            }
            /* посторонний байт — глотаем и продолжаем */
        }
    }
    return K_EIO;
}

/* Команда устройству + ожидание ACK. Вызывается до sti либо под cli. */
static int port_cmd_ack(int port2, u8 b)
{
    int sent = port2 ? send_port2(b) : send_port1_raw(b);
    if (!sent) {
        return 0;
    }
    for (u32 i = 0; i < WAIT_LIMIT; i++) {
        u8 r = 0;
        if (read_data_bounded(&r)) {
            if (r == 0xFAu) {
                return 1; /* ACK */
            }
            if (r == 0xFEu) {
                return 0; /* Resend: не считаем успехом */
            }
            /* посторонний байт — глотаем и продолжаем ждать */
        }
    }
    return 0;
}

err_t ps2_port1_cmd(u8 b)
{
    return port_cmd_ack(0, b) ? K_OK : K_EIO;
}

err_t ps2_port2_cmd(u8 b)
{
    return port_cmd_ack(1, b) ? K_OK : K_EIO;
}

/* Читает конфиг; требует отключённые порты (иначе IRQ-биты смысла не имеют). */
static int read_config(u8 *cfg)
{
    if (!send_cmd(CMD_READ_CFG)) {
        return 0;
    }
    return read_data_bounded(cfg);
}

static int write_config(u8 cfg)
{
    if (!send_cmd(CMD_WRITE_CFG)) {
        return 0;
    }
    return send_port1_raw(cfg);
}

err_t ps2_init(struct ps2_info *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    out->dual_channel = 0;
    out->kbd_ok = 0;
    out->mouse_ok = 0;
    out->config = 0;

    /* 1. Отключить порты и слить выходной буфер. */
    (void)send_cmd(CMD_DISABLE_P1);
    (void)send_cmd(CMD_DISABLE_P2);
    flush_output();

    /* 2. Прочитать конфиг; определить dual-channel пробой: включаем клок
     *    второго порта и читаем назад — если бит 5 остался ясным, порт есть. */
    u8 cfg = 0;
    if (!read_config(&cfg)) {
        klog(KLOG_ERROR, PS2_TAG, "8042: конфигурационный байт недоступен");
        return K_EIO;
    }
    u8 probe = (u8)(cfg & (u8)~CFG_P2_CLOCK);
    if (!write_config(probe)) {
        return K_EIO;
    }
    u8 back = 0;
    if (!read_config(&back)) {
        return K_EIO;
    }
    if ((back & CFG_P2_CLOCK) == 0u) {
        out->dual_channel = 1;
    }
    cfg = back;

    /* 3. Самопроверка контроллера. */
    u8 st = 0;
    if (ps2_controller_selftest(&st) != K_OK || st != 0x55u) {
        klog(KLOG_ERROR, PS2_TAG, "8042: self-test вернул 0x%02x (ожидается 0x55)", st);
        return K_EIO;
    }

    /* 4. Interface-тесты портов. */
    if (!send_cmd(CMD_TEST_P1)) {
        return K_EIO;
    }
    u8 t1 = 0;
    (void)read_data_bounded(&t1);
    out->kbd_ok = (t1 == 0x00u);

    if (out->dual_channel != 0) {
        if (!send_cmd(CMD_TEST_P2)) {
            return K_EIO;
        }
        u8 t2 = 0;
        (void)read_data_bounded(&t2);
        out->mouse_ok = (t2 == 0x00u);
    }

    /* 5. Итоговый конфиг: трансляция вкл., IRQ/clок — по наличию устройств.
     *    Сначала IRQ/clок выключены (устройства ещё не активированы). */
    cfg &= (u8)~(CFG_P1_IRQ | CFG_P2_IRQ);
    cfg |= CFG_TRANSLATE;
    if (out->dual_channel != 0) {
        cfg |= CFG_P2_CLOCK; /* порт 2 пока отключён по клоку */
    }
    if (!write_config(cfg)) {
        return K_EIO;
    }

    /* 6. Включить порты. */
    (void)send_cmd(CMD_ENABLE_P1);
    if (out->dual_channel != 0 && out->mouse_ok != 0) {
        (void)send_cmd(CMD_ENABLE_P2);
    }

    /* 7. Разрешить IRQ и клоки устройств в конфиге. */
    if (out->kbd_ok != 0) {
        cfg |= CFG_P1_IRQ;
    }
    if (out->dual_channel != 0 && out->mouse_ok != 0) {
        cfg &= (u8)~CFG_P2_CLOCK;
        cfg |= CFG_P2_IRQ;
    }
    if (!write_config(cfg)) {
        return K_EIO;
    }

    /* 8. Включить сканирование клавиатуры (0xF4): после self-test контроллера
     *    устройства молчат, пока им не разрешат передачу. Мышь включает
     *    0xF4 в своём драйвере (mouse_init). */
    if (out->kbd_ok != 0) {
        if (ps2_port1_cmd(0xF4u) != K_OK) {
            klog(KLOG_WARN, PS2_TAG,
                 "клавиатура: 0xF4 без ACK (сканирование может быть выключено)");
        }
    }

    out->config = cfg;
    klog(KLOG_INFO, PS2_TAG,
         "8042: dual=%d kbd=%s mouse=%s cfg=0x%02x (translate=on)",
         out->dual_channel,
         out->kbd_ok ? "ok" : "FAIL",
         (out->dual_channel && out->mouse_ok) ? "ok" : "n/a",
         cfg);
    return K_OK;
}
