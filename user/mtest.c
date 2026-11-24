/*
 * Fobos OS — user-программа mtest (FND-3a): проверка SYS_brk и памяти.
 * Растёт heap на 64 КиБ, пишет/читает по краям, печатает маркер MEMOK.
 */
#include "lib/fos.h"

#define GROW (64u * 1024u)

int main(void)
{
    void *b0 = fos_brk((void *)0);
    void *b1 = fos_brk((char *)b0 + GROW);

    if (b1 != (char *)b0 + GROW) {
        fos_printf("MEMFAIL brk: got=%p want=%p\n", b1, (char *)b0 + GROW);
        return 1;
    }

    volatile unsigned *lo = (volatile unsigned *)b0;
    volatile unsigned *hi = (volatile unsigned *)((char *)b0 + GROW - 4u);
    *lo = 42u;
    *hi = 7u;
    if (*lo != 42u || *hi != 7u) {
        fos_printf("MEMFAIL rw\n");
        return 1;
    }

    /* Возврат барьера вниз: страницы снимаются (проверка unmap). */
    void *b2 = fos_brk(b0);
    if (b2 != b0) {
        fos_printf("MEMFAIL shrink: got=%p want=%p\n", b2, b0);
        return 1;
    }

    fos_printf("mtest: heap grown %u KiB at %p, rw+shrink ok\n",
               (unsigned)(GROW / 1024u), b0);
    fos_printf("MEMOK mtest\n");
    return 0;
}
