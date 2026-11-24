/*
 * Fobos OS — спин-блокировки ядра (header-only).
 *
 * Правила использования (обязательные, см. также docs/roadmap.md, FND-2):
 *  - блокировка НЕ рекурсивна: повторный spin_lock на том же CPU = deadlock;
 *  - критическая секция обязана быть короткой и не вызывать код, который
 *    может паниковать или выделять память;
 *  - на FND-1 ядро однопроцессорное (SMP включается в FND-2), но API уже
 *    потокобезопасен — прерывания/препротекания на той же задаче обрабатываются
 *    через irqsave-варианты;
 *  - порядок захвата нескольких блокировок: log -> phys (единственный
 *    допустимый порядок на FND-1; расширяется в docs/roadmap.md FND-2).
 *
 * Реализация: CAS-цикл с pause; unlock — release-store.
 *
 * FOBOS_HOST_TEST: irqsave-варианты деградируют до обычных spin_lock —
 * host-тесты однопоточны, а cli/popfq привилегированы (#GP в ring 3).
 */
#ifndef FOBOS_LIB_SPINLOCK_H
#define FOBOS_LIB_SPINLOCK_H

#include <fobos/types.h>

struct spinlock {
    volatile u32 val;   /* 0 = свободен, 1 = занят */
    const char *name;   /* для отладочных дампов (lockdep в FND-2) */
};

#define SPINLOCK_INIT(nm) { 0u, (nm) }

static inline void spin_init(struct spinlock *l, const char *name)
{
    l->val = 0u;
    l->name = name;
}

static inline void spin_lock(struct spinlock *l)
{
    for (;;) {
        u32 expected = 0u;
        if (__atomic_compare_exchange_n(&l->val, &expected, 1u, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        __builtin_ia32_pause();
    }
}

static inline int spin_trylock(struct spinlock *l)
{
    u32 expected = 0u;
    return __atomic_compare_exchange_n(&l->val, &expected, 1u, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
               ? 1
               : 0;
}

static inline void spin_unlock(struct spinlock *l)
{
    __atomic_store_n(&l->val, 0u, __ATOMIC_RELEASE);
}

#ifdef FOBOS_HOST_TEST

static inline u64 spin_lock_irqsave(struct spinlock *l)
{
    spin_lock(l);
    return 0u;
}

static inline void spin_unlock_irqrestore(struct spinlock *l, u64 flags)
{
    (void)flags;
    spin_unlock(l);
}

#else /* !FOBOS_HOST_TEST — ядро: ring 0 */

static inline u64 spin_lock_irqsave(struct spinlock *l)
{
    u64 flags;
    __asm__ volatile ("pushfq\n\t"
                      "cli\n\t"
                      "popq %0"
                      : "=r"(flags)
                      :
                      : "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(struct spinlock *l, u64 flags)
{
    spin_unlock(l);
    __asm__ volatile ("pushq %0\n\t"
                      "popfq"
                      :
                      : "r"(flags)
                      : "memory", "cc");
}

#endif /* FOBOS_HOST_TEST */

#endif /* FOBOS_LIB_SPINLOCK_H */
