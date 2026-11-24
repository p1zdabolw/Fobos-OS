/*
 * Fobos OS — реализация FBPI v0 (общая для загрузчика, ядра и host-тестов).
 * Слой: без зависимостей от оборудования (выполняется и под EFI, и в user-space).
 */
#include <fobos/boot_info.h>
#include <fobos/errors.h>

#include <stddef.h> /* offsetof, size_t */

/* Защита ABI: сдвиг структуры — ошибка сборки, а не тихая порча памяти. */
_Static_assert(sizeof(struct fbpi_boot_info)  == 192, "FBPI v0.1 ABI: размер boot_info изменился");
_Static_assert(sizeof(struct fbpi_framebuffer) == 40, "FBPI v0.1 ABI: размер framebuffer изменился");
_Static_assert(sizeof(struct fbpi_efi_memdesc) == 40, "FBPI v0.1 ABI: размер efi_memdesc изменился");
_Static_assert(offsetof(struct fbpi_boot_info, checksum) == 8,  "FBPI v0.1 ABI: смещение checksum");
_Static_assert(offsetof(struct fbpi_boot_info, efi_mmap_phys) == 40, "FBPI v0.1 ABI: смещение efi_mmap_phys");
_Static_assert(offsetof(struct fbpi_boot_info, cfg_phys) == 72, "FBPI v0.1 ABI: смещение cfg_phys");
_Static_assert(offsetof(struct fbpi_boot_info, fb) == 88, "FBPI v0.1 ABI: смещение fb");

u32 fbpi_compute_checksum(const struct fbpi_boot_info *bi)
{
    const u32 *w = (const u32 *)(const void *)bi;
    const size_t words = sizeof(*bi) / sizeof(u32);
    u32 sum = 0;

    for (size_t i = 0; i < words; i++) {
        sum += w[i];
    }
    /* Поле checksum (word #2) при расчёте считается нулём. */
    sum -= w[offsetof(struct fbpi_boot_info, checksum) / sizeof(u32)];
    return 0u - sum; /* дополнение: сумма всех слов готового заголовка == 0 (mod 2^32) */
}

u32 fbpi_pixel_rgb(const struct fbpi_framebuffer *fb, u32 rgb)
{
    if (fb == NULL) {
        return 0;
    }
    u32 r = (rgb >> 16) & 0xFFu;
    u32 g = (rgb >> 8) & 0xFFu;
    u32 b = rgb & 0xFFu;

    switch (fb->pixel_format) {
    case FBPI_PF_RGBX:
        return r | (g << 8) | (b << 16);   /* байты: R G B x */
    case FBPI_PF_BGRX:
        return b | (g << 8) | (r << 16);   /* байты: B G R x */
    case FBPI_PF_BITMASK: {
        /* Маска может иметь произвольную ширину <= 8 бит на канал; масштабируем. */
        struct channel { u32 mask; u32 value; } ch[3] = {
            { fb->mask_red, r },
            { fb->mask_green, g },
            { fb->mask_blue, b }
        };
        u32 out = 0;
        for (int i = 0; i < 3; i++) {
            u32 m = ch[i].mask;
            if (m == 0u) {
                continue;
            }
            u32 shift = __builtin_ctz(m);
            u32 width = 32u - __builtin_clz(m) - shift;
            u32 max = (width >= 8u) ? 0xFFu : ((1u << width) - 1u);
            u32 scaled = (ch[i].value * max + 127u) / 255u; /* округление к ближ. */
            out |= scaled << shift;
        }
        return out;
    }
    default:
        return 0; /* BLTONLY: прямого доступа нет */
    }
}

err_t fbpi_validate(const struct fbpi_boot_info *bi)
{
    if (bi == NULL) {
        return K_EINVAL;
    }

    const u32 *w = (const u32 *)(const void *)bi;
    const size_t words = sizeof(*bi) / sizeof(u32);
    u32 sum = 0;

    for (size_t i = 0; i < words; i++) {
        sum += w[i];
    }
    if (sum != 0u) {
        return K_EINVAL;
    }

    if (bi->magic != FBPI_MAGIC || bi->version != FBPI_VERSION ||
        bi->header_size != (u16)sizeof(struct fbpi_boot_info)) {
        return K_EINVAL;
    }
    if ((bi->boot_flags & ~FBPI_BOOTF_MASK) != 0u) {
        return K_EINVAL; /* неизвестные флаги — расхождение ABI */
    }
    if (bi->cfg_phys == 0u) {
        if (bi->cfg_size != 0u) {
            return K_EINVAL;
        }
    } else {
        /* Блоб настроек: минимум — заголовок+тег foboscfg; максимум — 64 КиБ. */
        if (bi->cfg_size < 96u || bi->cfg_size > FBPI_BOOTF_CFG_MAX) {
            return K_EINVAL;
        }
    }

    if (bi->kernel_vbase <= bi->kernel_pbase) {
        return K_EINVAL;
    }
    if (bi->kernel_pbase == 0u || bi->kernel_size_bytes == 0u) {
        return K_EINVAL;
    }

    /* Карта памяти обязана присутствовать и иметь шаг >= базового дескриптора. */
    if (bi->efi_mmap_phys == 0u || bi->efi_mmap_size == 0u ||
        bi->efi_desc_size < sizeof(struct fbpi_efi_memdesc) ||
        (bi->efi_desc_size % 8u) != 0u) {
        return K_EINVAL;
    }

    /*Framebuffer: осмысленная геометрия и поддерживаемый формат прямого доступа. */
    if (bi->fb.width == 0u || bi->fb.height == 0u || bi->fb.bpp != 32u) {
        return K_EINVAL;
    }
    if (bi->fb.pitch_bytes < (u64)bi->fb.width * 4u) {
        return K_EINVAL;
    }
    if (bi->fb.pixel_format != FBPI_PF_RGBX &&
        bi->fb.pixel_format != FBPI_PF_BGRX &&
        bi->fb.pixel_format != FBPI_PF_BITMASK) {
        return K_EINVAL; /* FBPI_PF_BLTONLY не даёт прямого доступа — отвергаем */
    }
    if (bi->fb.pixel_format == FBPI_PF_BITMASK) {
        if (bi->fb.mask_red == 0u || bi->fb.mask_green == 0u || bi->fb.mask_blue == 0u) {
            return K_EINVAL;
        }
    }

    if (bi->firmware_vendor[FBPI_VENDOR_MAX - 1u] != '\0') {
        return K_EINVAL;
    }

    return K_OK;
}
