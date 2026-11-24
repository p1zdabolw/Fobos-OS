/*
 * Fobos OS — файловые операции с загрузочным ESP (реализация).
 */
#include "espfile.h"

#include "bootctx.h"
#include "lserial.h"

#define ESPFILE_PATH16_MAX 64u

static void widen16(CHAR16 *dst, u32 cap, const char *src)
{
    u32 i = 0;
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = (CHAR16)(u8)src[i];
    }
    dst[i] = 0;
}

/* Разыменовывает SimpleFileSystem загрузочного образа и открывает корень. */
static int esp_open_root(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **out_sfs,
                         struct EFI_FILE_PROTOCOL **out_root)
{
    struct EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS st = bootctx_bs()->handle_protocol(bootctx_image(),
        &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    if (EFI_ERROR(st) || li == NULL) {
        return -1;
    }
    st = bootctx_bs()->handle_protocol(li->device_handle,
        &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)out_sfs);
    if (EFI_ERROR(st) || *out_sfs == NULL) {
        return -1;
    }
    st = (*out_sfs)->open_volume(*out_sfs, out_root);
    if (EFI_ERROR(st)) {
        return -1;
    }
    return 0;
}

int espfile_read(const char *path_a, u64 max_size, u8 **out_buf, u32 *out_size)
{
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    struct EFI_FILE_PROTOCOL *root = NULL;
    struct EFI_FILE_PROTOCOL *file = NULL;
    CHAR16 path16[ESPFILE_PATH16_MAX];
    u64 size = 0;
    u64 buf = 0;

    *out_buf = NULL;
    *out_size = 0;

    if (esp_open_root(&sfs, &root) != 0) {
        return -1;
    }
    widen16(path16, ESPFILE_PATH16_MAX, path_a);

    if (EFI_ERROR(root->open(root, &file, path16, EFI_FILE_MODE_READ, 0u))) {
        (void)root->close(root);
        return -1;   /* нет файла — штатная ситуация для конфигов */
    }
    /* SetPosition(0xFFFFFFFFFFFFFFFF) = EOF; GetPosition после этого
     * возвращает размер файла (приём из спецификации UEFI). */
    if (EFI_ERROR(file->set_position(file, 0xFFFFFFFFFFFFFFFFULL)) ||
        EFI_ERROR(file->get_position(file, &size))) {
        (void)file->close(file);
        (void)root->close(root);
        return -1;
    }
    if (size == 0u || size > max_size) {
        (void)file->close(file);
        (void)root->close(root);
        return -1;
    }
    if (EFI_ERROR(file->set_position(file, 0u))) {
        (void)file->close(file);
        (void)root->close(root);
        return -1;
    }
    if (EFI_ERROR(bootctx_alloc_below_4g(EFI_LOADER_DATA, size, &buf))) {
        (void)file->close(file);
        (void)root->close(root);
        return -1;
    }
    u64 remaining = size;
    u64 cursor = buf;
    while (remaining > 0u) {
        u64 got = remaining;
        if (EFI_ERROR(file->read(file, &got, (void *)(u64)cursor)) ||
            got == 0u) {
            (void)file->close(file);
            (void)root->close(root);
            (void)bootctx_free(buf, size);
            return -1;
        }
        cursor += got;
        remaining -= got;
    }
    (void)file->close(file);
    (void)root->close(root);
    *out_buf = (u8 *)(u64)buf;
    *out_size = (u32)size;
    return 0;
}

void espfile_free(u8 *buf, u32 size)
{
    if (buf != NULL && size != 0u) {
        (void)bootctx_free((u64)(u64 *)(u64)buf, size);
    }
}

int espfile_put(const char *path_a, const u8 *data, u32 len)
{
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    struct EFI_FILE_PROTOCOL *root = NULL;
    struct EFI_FILE_PROTOCOL *file = NULL;
    CHAR16 path16[ESPFILE_PATH16_MAX];

    if (esp_open_root(&sfs, &root) != 0) {
        return -1;
    }
    widen16(path16, ESPFILE_PATH16_MAX, path_a);

    /* Удаляем существующий (иначе хвост старого содержимого останется). */
    if (!EFI_ERROR(root->open(root, &file, path16, EFI_FILE_MODE_READ, 0u))) {
        (void)file->del(file);
        file = NULL;
    }
    if (EFI_ERROR(root->open(root, &file, path16,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                                 EFI_FILE_MODE_CREATE,
                             0u))) {
        (void)root->close(root);
        return -1;
    }
    u64 remaining = len;
    u64 cursor = (u64)data;
    while (remaining > 0u) {
        u64 put = remaining;
        if (EFI_ERROR(file->write(file, &put, (const void *)(u64)cursor)) ||
            put == 0u) {
            (void)file->close(file);
            (void)root->close(root);
            return -1;
        }
        cursor += put;
        remaining -= put;
    }
    (void)file->flush(file);
    (void)file->close(file);
    (void)root->close(root);
    return 0;
}

void espfile_delete(const char *path_a)
{
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    struct EFI_FILE_PROTOCOL *root = NULL;
    struct EFI_FILE_PROTOCOL *file = NULL;
    CHAR16 path16[ESPFILE_PATH16_MAX];

    if (esp_open_root(&sfs, &root) != 0) {
        return;
    }
    widen16(path16, ESPFILE_PATH16_MAX, path_a);
    if (!EFI_ERROR(root->open(root, &file, path16, EFI_FILE_MODE_READ, 0u))) {
        (void)file->del(file);
    }
    (void)root->close(root);
}
