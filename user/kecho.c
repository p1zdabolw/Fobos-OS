/*
 * Fobos OS — user-программа kecho (FND-3a): блокирующее чтение клавиатуры
 * через SYS_read с эхом. Демонстрация блокировки задач (wait-очередь).
 * Выход — ESC.
 */
#include "lib/fos.h"

int main(void)
{
    fos_printf("kecho: echo keys, ESC to exit\n");
    for (;;) {
        struct fos_keyevent ev;
        if (fos_getkey(&ev) != 0) {
            break;
        }
        if (ev.code == (unsigned char)FOS_KEY_ESC) {
            break;
        }
        if (ev.ch >= 0x20 && ev.ch < 127) {
            fos_printf("%c", ev.ch);
        } else if (ev.code == (unsigned char)FOS_KEY_ENTER) {
            fos_puts("\n");
        }
    }
    fos_puts("\nkecho: bye\n");
    return 0;
}
