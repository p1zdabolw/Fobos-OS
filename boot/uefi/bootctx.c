/*
 * Fobos OS — общий контекст загрузчика (реализация).
 */
#include "bootctx.h"

static EFI_HANDLE g_ctx_image;
static struct EFI_SYSTEM_TABLE *g_ctx_st;

void bootctx_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    g_ctx_image = image;
    g_ctx_st = st;
}

EFI_HANDLE bootctx_image(void)
{
    return g_ctx_image;
}

struct EFI_SYSTEM_TABLE *bootctx_st(void)
{
    return g_ctx_st;
}

struct EFI_BOOT_SERVICES *bootctx_bs(void)
{
    return (g_ctx_st != NULL) ? g_ctx_st->boot_services : NULL;
}

EFI_STATUS bootctx_alloc_below_4g(u32 mem_type, u64 size, u64 *out_phys)
{
    if (out_phys == NULL || size == 0u) {
        return EFI_INVALID_PARAMETER;
    }
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFULL;
    EFI_STATUS st = bootctx_bs()->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS,
                                                 mem_type,
                                                 EFI_SIZE_TO_PAGES(size),
                                                 &addr);
    if (!EFI_ERROR(st)) {
        *out_phys = addr;
    }
    return st;
}

EFI_STATUS bootctx_free(u64 phys, u64 size)
{
    if (phys == 0u || size == 0u) {
        return EFI_INVALID_PARAMETER;
    }
    EFI_PHYSICAL_ADDRESS addr = (EFI_PHYSICAL_ADDRESS)phys;
    return bootctx_bs()->free_pages(addr, EFI_SIZE_TO_PAGES(size));
}

EFI_STATUS bootctx_alloc_pool(u64 size, void **out_buf)
{
    if (out_buf == NULL || size == 0u) {
        return EFI_INVALID_PARAMETER;
    }
    return bootctx_bs()->allocate_pool(EFI_BOOT_SERVICES_DATA, size, out_buf);
}

void bootctx_free_pool(void *buf)
{
    if (buf != NULL) {
        bootctx_bs()->free_pool(buf);
    }
}
