/*
 * Fobos OS — user-программа hello (FND-3a): первый процесс ring 3.
 * Печатает баннер и маркер USEROK для e2e-сценария CI.
 */
#include "lib/fos.h"

int main(void)
{
    fos_printf("hello: Fobos OS user-space v0\n");
    fos_printf("hello: pid=%d abi=%u uptime=%u ms\n",
               fos_getpid(), fos_abi_version(), fos_uptime());
    fos_printf("USEROK hello\n");
    return 0;
}
