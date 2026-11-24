/*
 * Fobos OS — менеджер виртуальной памяти x86_64 (FND-3a).
 *
 * Раскладка PTE (x86_64):
 *   bit 0  P     present
 *   bit 1  W     writable
 *   bit 2  U     user
 *   bit 7  PS    page size (PML4/PDPT/PD: большая страница)
 *   bits 12..51  физический адрес
 *   bit 63 NX    no-execute (EFER.NXE включён в tss_init)
 *
 * Разделяемые записи PML4 (копируются из прототипа загрузчика):
 *   [0..255] — identity < 4 ГиБ (загрузчик, supervisor; PS-страницы 2 МиБ
 *   на уровне PD — УГАДЫВАТЬ по битам нельзя: user-маппинги живут в тех же
 *   слотах PML4[0]); [256..511] — higher-half ядра.
 *
 * РЕЕСТР КАДРОВ (ключевое решение FND-3a): каждый процесс владеет явным
 * списком выделенных кадров (таблицы + данные), который пополняют
 * vmm_map_page/vmm_space_create и опустошает vmm_space_destroy. Обход
 * дерева страниц при destroy НЕ выполняется: отличить разделяемые таблицы
 * загрузчика от собственных по битам PTE невозможно, а ошибочное
 * освобождение identity-таблиц — порча адресации всей системы.
 *
 * Адресация таблиц: по identity-отображению (< 4 ГиБ) — физический адрес
 * таблицы разыменовывается как указатель (см. шапку vmm.h).
 */
#include "vmm.h"
#include "phys.h"
#include "../lib/log.h"
#include "../lib/string.h"

#define PTE_P   (1ull << 0)
#define PTE_W   (1ull << 1)
#define PTE_U   (1ull << 2)
#define PTE_PS  (1ull << 7)
#define PTE_NX  (1ull << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* Индексы страницы в раскладке x86_64 (9+9+9+12). */
#define PT_IDX(v) (((v) >> 12) & 0x1FFull)
#define PD_IDX(v) (((v) >> 21) & 0x1FFull)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FFull)
#define PML4_IDX(v) (((v) >> 39) & 0x1FFull)

/* Проверка user-виртуального адреса: нижняя половина PML4 (canonical user),
 * кроме самой нулевой страницы (NULL-deref ловится железом). */
#define VMM_USER_LIMIT 0x0000800000000000ull

/* Пул пространств и реестры кадров (статические — без динамической памяти). */
#define VMM_MAX_SPACES   20u
#define VMM_SPACE_FRAMES 8192u   /* потолок 32 МиБ user-памяти на процесс (v0) */

static u64 g_kernel_cr3;
static struct vmm_stats g_stats;
static const char *const VMM_TAG = "mm/vmm";

struct space_reg {
    int used;
    u64 pml4;
    u64 frames[VMM_SPACE_FRAMES];   /* все кадры процесса (таблицы+данные) */
    u32 count;
    u32 tables;                     /* из frames[] — таблицы (для статистики) */
};
static struct space_reg g_spaces[VMM_MAX_SPACES];

static u64 read_cr3(void)
{
    u64 v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline u64 *pml4_ptr(u64 pml4)
{
    return (u64 *)(u64)pml4;   /* identity < 4 ГиБ (см. шапку) */
}

/* Bigpage-запись (PS=1): страница данных/identity, а не таблица. */
static inline int entry_is_bigpage(u64 e)
{
    return (e & (PTE_PS | PTE_P)) == (PTE_PS | PTE_P);
}

u64 vmm_kernel_cr3(void)
{
    return g_kernel_cr3;
}

struct vmm_stats vmm_get_stats(void)
{
    return g_stats;
}

static struct space_reg *reg_find(u64 pml4)
{
    for (u32 i = 0; i < VMM_MAX_SPACES; i++) {
        if (g_spaces[i].used != 0 && g_spaces[i].pml4 == pml4) {
            return &g_spaces[i];
        }
    }
    return NULL;
}

/* Регистрация кадра за процессом. При исчерпании реестра — K_ENOMEM. */
static err_t reg_add(struct space_reg *sp, u64 paddr)
{
    if (sp->count >= VMM_SPACE_FRAMES) {
        return K_ENOMEM;
    }
    sp->frames[sp->count++] = paddr;
    return K_OK;
}

/* Принадлежит ли кадр процессу (частная таблица)? */
static int reg_has(const struct space_reg *sp, u64 paddr)
{
    for (u32 i = 0; i < sp->count; i++) {
        if (sp->frames[i] == paddr) {
            return 1;
        }
    }
    return 0;
}

/* Снятие кадра с учёта (swap-remove; порядок не значим). */
static void reg_remove(struct space_reg *sp, u64 paddr)
{
    for (u32 i = 0; i < sp->count; i++) {
        if (sp->frames[i] == paddr) {
            sp->frames[i] = sp->frames[sp->count - 1u];
            sp->count--;
            return;
        }
    }
}

err_t vmm_init(void)
{
    g_kernel_cr3 = read_cr3();
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_spaces, 0, sizeof(g_spaces));
    klog(KLOG_INFO, VMM_TAG, "VMM: kernel CR3 = 0x%llx (прототип для новых пространств)",
         (unsigned long long)g_kernel_cr3);
    return K_OK;
}

err_t vmm_space_create(u64 *out_pml4)
{
    if (out_pml4 == NULL) {
        return K_EINVAL;
    }

    /* Свободный слот реестра. */
    struct space_reg *sp = NULL;
    for (u32 i = 0; i < VMM_MAX_SPACES; i++) {
        if (g_spaces[i].used == 0) {
            sp = &g_spaces[i];
            break;
        }
    }
    if (sp == NULL) {
        return K_ENOMEM;
    }

    u64 pml4 = 0;
    err_t rc = phys_alloc_frame(&pml4);
    if (rc != K_OK) {
        return rc;
    }

    u64 *tab = pml4_ptr(pml4);
    memset(tab, 0, PAGE_SIZE);

    /* Копируем разделяемые записи прототипа: нижнюю половину (identity)
     * и всю верхнюю (ядро). */
    u64 *proto = pml4_ptr(g_kernel_cr3);
    for (u32 i = 0; i < 256u; i++) {
        tab[i] = proto[i];
    }
    for (u32 i = 256u; i < 512u; i++) {
        tab[i] = proto[i];
    }

    sp->used = 1;
    sp->pml4 = pml4;
    sp->count = 0;
    *out_pml4 = pml4;
    g_stats.spaces++;
    return K_OK;
}

err_t vmm_space_destroy(u64 pml4)
{
    if (pml4 == 0u || pml4 == g_kernel_cr3) {
        return K_EINVAL;
    }

    struct space_reg *sp = reg_find(pml4);
    if (sp == NULL) {
        return K_EINVAL;   /* чужое/неизвестное пространство */
    }

    /* Освобождаем ТОЛЬКО зарегистрированные кадры (таблицы + данные).
     * Разделяемые таблицы загрузчика не трогаются по построению. */
    for (u32 i = 0; i < sp->count; i++) {
        err_t rc = phys_free_frame(sp->frames[i]);
        if (rc != K_OK) {
            klog(KLOG_ERROR, VMM_TAG,
                 "destroy: кадр 0x%llx не освобождён (%s) — состояние пула повреждено",
                 (unsigned long long)sp->frames[i], k_strerror(rc));
        }
        if (g_stats.user_frames > 0u) {
            g_stats.user_frames--;
        }
    }
    g_stats.page_tables -= sp->tables;
    sp->used = 0;
    sp->count = 0;
    sp->tables = 0;
    sp->pml4 = 0;

    (void)phys_free_frame(pml4);
    if (g_stats.spaces > 0u) {
        g_stats.spaces--;
    }
    return K_OK;
}

void vmm_space_load(u64 pml4)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

/* Делает таблицу в слоте приватной для процесса:
 *   отсутствует     -> выделить и занулить;
 *   наша (реестр)   -> оставить;
 *   чужая (прототип загрузчика, общая) -> КЛОНИРОВАТЬ: запись в общую
 *   таблицу сделали бы user-маппинги видимыми ядру и всем процессам.
 * Все таблицы user-половины получают U: привилегия U/S в x86 — AND по
 * всем уровням обхода, supervisor-родитель делает недоступной даже
 * user-страницу-лист. */
#define TABLE_FLAGS (PTE_P | PTE_W | PTE_U)

static err_t table_ensure_private(struct space_reg *sp, u64 *slot)
{
    u64 e = *slot;
    if ((e & PTE_P) == 0u) {
        u64 np = 0;
        err_t rc = phys_alloc_frame(&np);
        if (rc != K_OK) {
            return rc;
        }
        memset(pml4_ptr(np), 0, PAGE_SIZE);
        *slot = np | TABLE_FLAGS;
        rc = reg_add(sp, np);
        if (rc != K_OK) {
            (void)phys_free_frame(np);
            *slot = e;
            return rc;
        }
        g_stats.page_tables++;
        sp->tables++;
        return K_OK;
    }
    if (entry_is_bigpage(e)) {
        return K_EINVAL;   /* bigpage на уровне таблиц не расщепляется здесь */
    }
    if (reg_has(sp, e & PTE_ADDR_MASK)) {
        return K_OK;
    }
    /* Чужая таблица (прототип загрузчика): клон. */
    u64 np = 0;
    err_t rc = phys_alloc_frame(&np);
    if (rc != K_OK) {
        return rc;
    }
    memcpy(pml4_ptr(np), pml4_ptr(e & PTE_ADDR_MASK), PAGE_SIZE);
    *slot = np | TABLE_FLAGS;
    rc = reg_add(sp, np);
    if (rc != K_OK) {
        (void)phys_free_frame(np);
        *slot = e;
        return rc;
    }
    g_stats.page_tables++;
    sp->tables++;
    return K_OK;
}

/* Расщепление bigpage (identity 2 МиБ загрузчика) в 4 КиБ-таблицу:
 * 512 записей повторяют отображение той же физической области с теми же
 * атрибутами (P|W, supervisor, исполняемая), минус PS. Таблица — частная. */
static err_t bigpage_split(struct space_reg *sp, u64 *slot)
{
    u64 e = *slot;
    u64 base = e & PTE_ADDR_MASK;
    u64 attrs = e & ~(PTE_ADDR_MASK | PTE_PS);

    u64 np = 0;
    err_t rc = phys_alloc_frame(&np);
    if (rc != K_OK) {
        return rc;
    }
    u64 *pt = pml4_ptr(np);
    for (u32 j = 0; j < 512u; j++) {
        pt[j] = (base + (u64)j * PAGE_SIZE) | attrs;
    }
    *slot = np | TABLE_FLAGS;
    rc = reg_add(sp, np);
    if (rc != K_OK) {
        (void)phys_free_frame(np);
        *slot = e;
        return rc;
    }
    g_stats.page_tables++;
    sp->tables++;
    return K_OK;
}

err_t vmm_map_page(u64 pml4, u64 vaddr, u64 paddr, u64 flags)
{
    if ((vaddr & (PAGE_SIZE - 1u)) != 0u || (paddr & (PAGE_SIZE - 1u)) != 0u) {
        return K_EINVAL;
    }
    if (vaddr >= VMM_USER_LIMIT || vaddr < PAGE_SIZE) {
        return K_EINVAL;   /* user-диапазон: [4 КиБ, 0x8000'0000'0000) */
    }
    if ((flags & VMM_PF_USER) == 0u) {
        return K_EINVAL;
    }

    struct space_reg *sp = reg_find(pml4);
    if (sp == NULL) {
        return K_EINVAL;
    }

    u64 *tab = pml4_ptr(pml4);
    u32 i4 = (u32)PML4_IDX(vaddr);
    u32 i3 = (u32)PDPT_IDX(vaddr);
    u32 i2 = (u32)PD_IDX(vaddr);
    u32 i1 = (u32)PT_IDX(vaddr);

    if (table_ensure_private(sp, &tab[i4]) != K_OK) {
        return K_EINVAL;
    }
    if (table_ensure_private(sp, &pml4_ptr(tab[i4] & PTE_ADDR_MASK)[i3]) != K_OK) {
        return K_EINVAL;
    }

    u64 *pdpt = pml4_ptr(tab[i4] & PTE_ADDR_MASK);
    u64 *pd = pml4_ptr(pdpt[i3] & PTE_ADDR_MASK);

    if ((pd[i2] & PTE_P) == 0u) {
        if (table_ensure_private(sp, &pd[i2]) != K_OK) {
            return K_EINVAL;
        }
    } else if (entry_is_bigpage(pd[i2])) {
        /* user-страница внутри identity 2 МиБ: расщепляем bigpage
         * (частная таблица, identity-атрибуты сохраняются). */
        if (bigpage_split(sp, &pd[i2]) != K_OK) {
            return K_ENOMEM;
        }
    } else if (reg_has(sp, pd[i2] & PTE_ADDR_MASK) == 0) {
        return K_EINVAL;   /* таблица не наша — пользовательский слот чужой */
    }

    u64 *pt = pml4_ptr(pd[i2] & PTE_ADDR_MASK);
    u64 pte = paddr | PTE_P | PTE_U;
    if ((flags & VMM_PF_RW) != 0u) {
        pte |= PTE_W;
    }
    if ((flags & VMM_PF_NX) != 0u) {
        pte |= PTE_NX;
    }
    int was_absent = (pt[i1] & PTE_P) == 0u;
    if (was_absent != 0) {
        err_t rc = reg_add(sp, paddr);
        if (rc != K_OK) {
            return rc;
        }
        g_stats.user_frames++;
    }
    pt[i1] = pte;
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    return K_OK;
}

err_t vmm_unmap_page(u64 pml4, u64 vaddr)
{
    if ((vaddr & (PAGE_SIZE - 1u)) != 0u || vaddr >= VMM_USER_LIMIT) {
        return K_EINVAL;
    }

    struct space_reg *sp = reg_find(pml4);
    if (sp == NULL) {
        return K_EINVAL;
    }

    u64 *tab = pml4_ptr(pml4);
    u32 i4 = (u32)PML4_IDX(vaddr);
    u32 i3 = (u32)PDPT_IDX(vaddr);
    u32 i2 = (u32)PD_IDX(vaddr);
    u32 i1 = (u32)PT_IDX(vaddr);

    if ((tab[i4] & PTE_P) == 0u || entry_is_bigpage(tab[i4])) {
        return K_EINVAL;
    }
    u64 *pdpt = pml4_ptr(tab[i4] & PTE_ADDR_MASK);
    if ((pdpt[i3] & PTE_P) == 0u || entry_is_bigpage(pdpt[i3])) {
        return K_EINVAL;
    }
    u64 *pd = pml4_ptr(pdpt[i3] & PTE_ADDR_MASK);
    if ((pd[i2] & PTE_P) == 0u || entry_is_bigpage(pd[i2])) {
        return K_EINVAL;
    }
    u64 *pt = pml4_ptr(pd[i2] & PTE_ADDR_MASK);
    if ((pt[i1] & PTE_P) == 0u) {
        return K_EINVAL;
    }

    u64 frame = pt[i1] & PTE_ADDR_MASK;
    pt[i1] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    reg_remove(sp, frame);
    if (g_stats.user_frames > 0u) {
        g_stats.user_frames--;
    }
    return phys_free_frame(frame);
}

err_t vmm_user_range_ok(u64 pml4, u64 uaddr, u64 len)
{
    if (len == 0u) {
        return K_OK;
    }
    if (uaddr < PAGE_SIZE || uaddr >= VMM_USER_LIMIT) {
        return K_EINVAL;
    }
    if (uaddr + len < uaddr || uaddr + len > VMM_USER_LIMIT) {
        return K_EINVAL;   /* переполнение/выход за user-диапазон */
    }

    u64 *tab = pml4_ptr(pml4);
    u64 cur = uaddr;
    u64 end = uaddr + len;

    while (cur < end) {
        u32 i4 = (u32)PML4_IDX(cur);
        u32 i3 = (u32)PDPT_IDX(cur);
        u32 i2 = (u32)PD_IDX(cur);
        u32 i1 = (u32)PT_IDX(cur);

        if ((tab[i4] & PTE_P) == 0u || entry_is_bigpage(tab[i4])) {
            return K_EINVAL;   /* не маплено либо bigpage прототипа */
        }
        u64 *pdpt = pml4_ptr(tab[i4] & PTE_ADDR_MASK);
        if ((pdpt[i3] & PTE_P) == 0u || entry_is_bigpage(pdpt[i3])) {
            return K_EINVAL;
        }
        u64 *pd = pml4_ptr(pdpt[i3] & PTE_ADDR_MASK);
        if ((pd[i2] & PTE_P) == 0u || entry_is_bigpage(pd[i2])) {
            /* bigpage на уровне PD = identity 2 МиБ загрузчика:
            * supervisor, не user-страница — доступ запрещён. */
            return K_EINVAL;
        }
        u64 *pt = pml4_ptr(pd[i2] & PTE_ADDR_MASK);
        if ((pt[i1] & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) {
            return K_EINVAL;
        }

        /* Следующая граница 4 КиБ страницы. */
        cur = (cur & ~0xFFFull) + PAGE_SIZE;
    }
    return K_OK;
}
