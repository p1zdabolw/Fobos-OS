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
  kernel/rtc.c \
  kernel/keyboard.c \
  kernel/mouse.c \
  kernel/pci.c \
  kernel/fb.c \
  kernel/fs.c \
  kernel/ata.c \
  kernel/diskfs.c \
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
  kernel/isr.asm \
  kernel/sched_asm.asm

C_OBJS   := $(C_SRCS:.c=.o)
ASM_OBJS := $(ASM_SRCS:.asm=.o)
OBJS     := $(ASM_OBJS) $(C_OBJS)

KERNEL := fos.elf
ISO    := fos.iso
DISK   := fos.img

.PHONY: all clean run run-nox iso verify-grub reset-disk

all: $(ISO) $(DISK)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=512 count=20480 2>/dev/null

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

reset-disk:
	rm -f $(DISK)

run: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) -m 256 -serial stdio -vga std \
	        -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
	        -nic user,model=rtl8139 \
	        -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

run-nox: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) -m 128 -display none -serial stdio \
	        -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
	        -nic user,model=rtl8139

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf iso_root