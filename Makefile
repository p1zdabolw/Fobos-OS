ARCH      := x86_64
CC        := gcc
LD        := ld
AS        := nasm
OBJCOPY   := objcopy
QEMU      := qemu-system-x86_64
XORRISO   := xorriso
GRUB_MKRESCUE := grub-mkrescue

CFLAGS := -Wall -Wextra -Werror -std=c11 -ffreestanding \
          -fno-stack-protector -fno-pic -mno-red-zone \
          -mcmodel=kernel -nostdlib -nostdinc -fno-builtin \
          -mno-sse -mno-sse2 -mno-mmx -mno-80387 -O2 \
          -I. -Ikernel -Ilib -Igui -Iapps

LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000

C_SRCS := \
  kernel/kmain.c \
  kernel/gdt.c \
  kernel/idt.c \
  kernel/serial.c \
  kernel/pmm.c \
  kernel/vmm.c \
  kernel/heap.c \
  kernel/timer.c \
  kernel/keyboard.c \
  kernel/mouse.c \
  kernel/pci.c \
  kernel/fb.c \
  kernel/fs.c \
  kernel/sched.c \
  kernel/syscall.c \
  kernel/elf.c \
  kernel/rtl8139.c \
  kernel/net.c \
  kernel/http.c \
  kernel/html.c \
  kernel/speaker.c \
  lib/string.c \
  lib/mem.c \
  lib/printf.c \
  lib/i18n.c \
  gui/font.c \
  gui/cursor.c \
  gui/compositor.c \
  gui/window.c \
  gui/widget.c \
  gui/desktop.c \
  gui/wallpaper.c \
  gui/settings.c \
  apps/terminal.c \
  apps/files.c \
  apps/notepad.c \
  apps/script.c \
  apps/browser.c \
  apps/photos.c \
  apps/media.c

ASM_SRCS := \
  boot/multiboot2.asm \
  kernel/isr.asm

C_OBJS   := $(C_SRCS:.c=.o)
ASM_OBJS := $(ASM_SRCS:.asm=.o)
OBJS     := $(ASM_OBJS) $(C_OBJS)

KERNEL := fos.elf
ISO    := fos.iso

.PHONY: all clean run run-nox iso verify-grub

all: $(ISO)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

iso: $(ISO)

$(ISO): $(KERNEL) boot/grub.cfg
	rm -rf iso_root
	mkdir -p iso_root/boot/grub
	cp $(KERNEL) iso_root/boot/fos.elf
	cp boot/grub.cfg iso_root/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) iso_root

verify-grub: $(ISO)
	xorriso -osirrox on -indev $(ISO) -extract /boot/grub/grub.cfg /tmp/fos_grub.cfg
	@echo "----- grub.cfg inside ISO -----"
	@cat /tmp/fos_grub.cfg
	@echo "-------------------------------"

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 256 -serial stdio -vga std -nic user,model=rtl8139 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

run-nox: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 128 -display none -serial stdio -nic user,model=rtl8139

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf iso_root