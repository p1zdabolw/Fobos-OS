/*
 * Fobos OS — OOBE-мастер первого запуска в загрузчике (FND-2b).
 *
 * Почему Setup живёт в загрузчике: только он имеет доступ к UEFI Runtime
 * Services (SetVariable) и может СОХРАНИТЬ результаты до ExitBootServices.
 * Ядро после ExitBootServices сохранить ничего не может (дисковая
 * подсистема появится в FND-2c). Графика — общий шрифт + lgfx-примитивы
 * поверх GOP; ввод — ConIn (прошивочный PS/2-драйвер), посимвольное
 * редактирование с перерисовкой поля.
 *
 * Шаги: welcome -> hostname -> [username -> password -> confirm -> more]*
 * -> save. По завершении формируется struct fobos_config для записи в
 * UEFI-переменную FobosSetup и передачи ядру через FBPI.
 */
#include "oobe.h"

#include "efi.h"
#include "lgfx.h"
#include "lin.h"
#include "lserial.h"
#include "uisup.h"
#include "font8x8.h"
#include "../../common/sha256.h"

#include <fobos/boot_info.h>

static struct EFI_SYSTEM_TABLE *g_ost;
static u32 g_salt_counter;

#define OOBE_HOSTNAME_MAX FOBOSCFG_HOSTNAME_MAX
#define OOBE_NAME_MAX     FOBOSCFG_NAME_MAX
#define OOBE_PW_MAX       48u

#define BOX_W 480u
#define BOX_H 160u

int oobe_init(struct EFI_SYSTEM_TABLE *st)
{
    if (st == NULL) {
        return -1;
    }
    g_ost = st;
    uisup_init(st);
    return 0;
}

static u32 str_len(const char *s)
{
    u32 n = 0;
    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

/* Соль: SHA-256(TSC || счётчик || имя[32]). Salt не секретен, но уникален. */
static void oobe_salt(const char name[FOBOSCFG_NAME_MAX],
                      u8 salt[FOBOSCFG_SALT_LEN])
{
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    u64 tsc = ((u64)hi << 32) | lo;
    u64 mixed[2] = { tsc, (u64)g_salt_counter };

    struct sha256_ctx c;
    u8 out[SHA256_DIGEST_LEN];
    sha256_init(&c);
    sha256_update(&c, mixed, sizeof(mixed));
    sha256_update(&c, name, FOBOSCFG_NAME_MAX); /* массив фиксированной длины */
    sha256_final(&c, out);
    for (u32 i = 0; i < FOBOSCFG_SALT_LEN; i++) {
        salt[i] = out[i];
    }
    g_salt_counter++;
}

int oobe_esc_window(void)
{
    if (lgfx_width() != 0u) {
        u32 w = lgfx_width();
        lgfx_fill_rect(0, 0, w, 20u, LGFX_COL_PANEL);
        lgfx_text(8u, 6u, "Fobos boot manager - ESC = Setup, I = Install, SHIFT = kernel console",
                  LGFX_COL_TX_DIM, LGFX_COL_PANEL);
    }
    lserial_puts("[loader] boot manager: esc window\n");

    /* Окно 1 c: прямой порт 0x60/0x64, EX-состояние Shift и обычные клавиши
     * (ESC = сброс настроек, I = установщик). Байт, забранный прямым
     * опросом, интерпретируется здесь же — раньше не-Shift байты
     * поглощались, и ESC/I в окне пропадали (гонка за выходной буфер 8042
     * с драйвером прошивки). Коды: набор 1 при translate=on (ESC=0x01,
     * I=0x17, LShift=0x2A, RShift=0x36), набор 2 при translate=off
     * (ESC=0x76, I=0x43, Shift=0x12/0x59); break-коды (|0x80) и служебные
     * байты — без действия. */
    u64 waited = 0;
    while (waited < 1000u) {
        u8 dcode = 0;
        if (lin_poll_direct(&dcode) == 1) {
            if (dcode == 0x2Au || dcode == 0x36u ||
                dcode == 0x12u || dcode == 0x59u) {
                lserial_puts("[loader] boot manager: shift detected\n");
                return 2;
            }
            if (dcode == 0x01u || dcode == 0x76u) {
                return 1;
            }
            if (dcode == 0x17u || dcode == 0x43u) {
                lserial_puts("[loader] boot manager: install requested (I)\n");
                return 3;
            }
        }
        if (lin_poll_shift() == 1) {
            lserial_puts("[loader] boot manager: shift detected\n");
            return 2;
        }
        u16 scan = 0, ch = 0;
        if (lin_poll_key(&scan, &ch) == 0) {
            if (scan == EFI_SCAN_ESC) {
                return 1;
            }
            if (ch == 'i' || ch == 'I') {
                lserial_puts("[loader] boot manager: install requested (I)\n");
                return 3;
            }
        }
        if (g_ost != NULL) {
            g_ost->boot_services->stall(5000u); /* 5 мс — часто против таймер-поллинга прошивки */
        }
        waited += 5u;
    }
    return 0;
}


enum oobe_step {
    OOBE_WELCOME = 1,
    OOBE_HOSTNAME = 2,
    OOBE_USERNAME = 3,
    OOBE_PASSWORD = 4,
    OOBE_CONFIRM = 5,
    OOBE_MORE = 6,
    OOBE_SAVE = 7
};

int oobe_run(struct fobos_config *out_cfg)
{
    if (out_cfg == NULL || g_ost == NULL) {
        return -1;
    }

    if (lgfx_width() == 0u || lgfx_height() == 0u) {
        return -1; /* графика обязательна для Setup */
    }
    uisup_layout(BOX_W, BOX_H);

    char hostname[OOBE_HOSTNAME_MAX];
    hostname[0] = 'f';
    hostname[1] = 'o';
    hostname[2] = 'b';
    hostname[3] = 'o';
    hostname[4] = 's';
    hostname[5] = '-';
    hostname[6] = 'p';
    hostname[7] = 'c';
    hostname[8] = '\0';

    struct fobos_config cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) {
        ((u8 *)&cfg)[i] = 0;
    }
    cfg.flags = FOBOSCFG_FLAG_OOBE_DONE;

    char name[OOBE_NAME_MAX];
    char pw[OOBE_PW_MAX];
    char pw2[OOBE_PW_MAX];
    char yn[4];
    yn[0] = 'n';
    yn[1] = '\0';

    enum oobe_step step = OOBE_WELCOME;

    while (step != OOBE_SAVE) {
        switch (step) {
        case OOBE_WELCOME:
            lserial_puts("[loader] oobe: step=welcome\n");
            uisup_screen(1, 7u, "welcome",
                        "Welcome to Fobos OS!",
                        "Setup will create the first user account.",
                        "Enter: continue");
            uisup_edit(yn, sizeof(yn), 0);
            step = OOBE_HOSTNAME;
            break;

        case OOBE_HOSTNAME:
            lserial_puts("[loader] oobe: step=hostname\n");
            uisup_screen(2, 7u, "hostname",
                        "Name this computer",
                        "(lowercase letters, digits, dash; default fobos-pc)",
                        "Enter: continue   Esc: back");
            {
                int rc = uisup_edit(hostname, sizeof(hostname), 0);
                if (rc == -1) {
                    step = OOBE_WELCOME;
                    break;
                }
                if (foboscfg_valid_hostname(hostname) == 0) {
                    uisup_error("Invalid hostname, try again");
                    break;
                }
            }
            for (u32 i = 0; i < FOBOSCFG_HOSTNAME_MAX; i++) {
                cfg.hostname[i] = hostname[i];
            }
            cfg.hostname[FOBOSCFG_HOSTNAME_MAX - 1u] = '\0';
            step = OOBE_USERNAME;
            break;

        case OOBE_USERNAME:
            lserial_puts("[loader] oobe: step=username\n");
            uisup_screen(3, 7u, "username",
                        "Create a user account",
                        "(lowercase, starts with a letter or _)",
                        "Enter: continue   Esc: back");
            name[0] = '\0';
            {
                int rc = uisup_edit(name, sizeof(name), 0);
                if (rc == -1) {
                    step = OOBE_HOSTNAME;
                    break;
                }
                if (foboscfg_valid_username(name) == 0) {
                    uisup_error("Invalid username, try again");
                    break;
                }
            }
            /* дубликаты запрещены */
            {
                int dup = 0;
                for (u32 i = 0; i < cfg.user_count; i++) {
                    if (str_len(cfg.users[i].name) == str_len(name)) {
                        u32 k = 0;
                        while (cfg.users[i].name[k] != '\0' &&
                               cfg.users[i].name[k] == name[k]) {
                            k++;
                        }
                        if (cfg.users[i].name[k] == '\0') {
                            dup = 1;
                        }
                    }
                }
                if (dup != 0) {
                    uisup_error("User already exists");
                    break;
                }
            }
            step = OOBE_PASSWORD;
            break;

        case OOBE_PASSWORD:
            lserial_puts("[loader] oobe: step=password\n");
            uisup_screen(4, 7u, "password",
                        "Set a password",
                        "(at least 4 characters, echoed as *)",
                        "Enter: continue   Esc: back");
            pw[0] = '\0';
            {
                int rc = uisup_edit(pw, sizeof(pw), 1);
                if (rc == -1) {
                    step = OOBE_USERNAME;
                    break;
                }
                if (str_len(pw) < 4u) {
                    uisup_error("Password is too short (min 4)");
                    break;
                }
            }
            step = OOBE_CONFIRM;
            break;

        case OOBE_CONFIRM:
            lserial_puts("[loader] oobe: step=confirm\n");
            uisup_screen(5, 7u, "confirm",
                        "Confirm the password",
                        "",
                        "Enter: continue   Esc: back");
            pw2[0] = '\0';
            {
                int rc = uisup_edit(pw2, sizeof(pw2), 1);
                if (rc == -1) {
                    step = OOBE_PASSWORD;
                    break;
                }
            }
            {
                u32 k = 0;
                while (pw[k] != '\0' && pw[k] == pw2[k]) {
                    k++;
                }
                if (pw[k] != pw2[k]) {
                    uisup_error("Passwords do not match");
                    break;
                }
            }

            /* Добавляем пользователя в конфиг. */
            {
                u8 salt[FOBOSCFG_SALT_LEN];
                u8 hash[FOBOSCFG_HASH_LEN];
                u32 ui = cfg.user_count;
                u32 k = 0;
                while (name[k] != '\0' && k + 1u < FOBOSCFG_NAME_MAX) {
                    cfg.users[ui].name[k] = name[k];
                    k++;
                }
                cfg.users[ui].name[k] = '\0';
                cfg.users[ui].is_admin = (ui == 0u) ? 1u : 0u; /* первый - администратор */
                oobe_salt(cfg.users[ui].name, salt);
                foboscfg_kdf(salt, pw, FOBOSCFG_KDF_ROUNDS, hash);
                for (u32 j = 0; j < FOBOSCFG_SALT_LEN; j++) {
                    cfg.users[ui].salt[j] = salt[j];
                }
                for (u32 j = 0; j < FOBOSCFG_HASH_LEN; j++) {
                    cfg.users[ui].hash[j] = hash[j];
                }
                cfg.user_count = ui + 1u;
            }
            step = OOBE_MORE;
            break;

        case OOBE_MORE:
            lserial_puts("[loader] oobe: step=more\n");
            uisup_screen(6, 7u, "more users",
                        "Add another user? (y/n)",
                        "The first user is the administrator.",
                        "Enter: confirm choice   Esc: back");
            {
                int rc = uisup_edit(yn, sizeof(yn), 0);
                if (rc == -1) {
                    step = OOBE_CONFIRM;
                    break;
                }
            }
            if (yn[0] == 'y' || yn[0] == 'Y') {
                if (cfg.user_count >= FOBOSCFG_MAX_USERS) {
                    uisup_error("Maximum number of users reached");
                    break;
                }
                step = OOBE_USERNAME;
            } else {
                step = OOBE_SAVE;
            }
            break;

        default:
            step = OOBE_SAVE;
            break;
        }
    }

    lserial_puts("[loader] oobe: step=save\n");
    uisup_screen(7, 7u, "save",
                "Saving setup...",
                "Settings will be written to platform storage (UEFI variable).",
                "");
    *out_cfg = cfg;
    return 0;
}
