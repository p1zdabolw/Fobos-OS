# Fobos OS — сборка FND-2a (UEFI-загрузчик + ядро + интерактивность + ESP-образ).
#
# Цели:
#   make            — собрать BOOTX64.efi и kernel.elf (с валидацией раскладки)
#   make esp        — собрать build/esp.img (FAT32 + MBR)
#   make selftest   — headless-прогон в QEMU/OVMF + интерактивный e2e-тест ввода
#                     (скрипт сам печатает в гостя через monitor и проверяет отклик)
#   make host-tests — unit-тесты на host-инструментарии
#   make run        — прогон в окне QEMU для интерактивной работы (kshell)
#   make verify     — все проверки разом
#   make clean
#
# Требования: gcc (>=11) с -mgeneral-regs-only, ld с эмуляцией i386pep
# (стандартный binutils x86_64), nasm >= 2.15, python3.
# QEMU/OVMF: qemu_selftest.py ищет их автоматически; при нестандартном
# расположении задайте FOBOS_QEMU и FOBOS_OVMF_DIR.

BUILD  ?= build
CC     ?= gcc
LD     ?= ld
NASM   ?= nasm
PYTHON ?= python3
QEMU   ?= qemu-system-x86_64

# ------------------------------------------------------------------
# Ядро: freestanding C11, model kernel, без SIMD и red zone.
# -fno-tree-loop-distribute-patterns: gcc на -O2 умеет заменять байтовые циклы
# в наших memcpy/memset вызовом тех же memcpy/memset => бесконечная рекурсия.
# -ffreestanding ЭТО НЕ ОТКЛЮЧАЕТ (memcpy/memset входят в freestanding-набор).
# ------------------------------------------------------------------
KCFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -fno-tree-loop-distribute-patterns \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -Wall -Wextra -Werror -O2 -g \
          -Iinclude -Icommon -Ikernel -DFOBOS_KERNEL
KLDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/kernel.ld

# ------------------------------------------------------------------
# User-space (FND-3a): freestanding C11, ring 3, integer-only.
# Red zone РАЗРЕШЕНА (ring 3: ядро не пишет под rsp user-кода).
# SIMD запрещён: состояние FPU не сохраняется планировщиком (FND-3b).
# ------------------------------------------------------------------
UCFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -fno-tree-loop-distribute-patterns -fno-builtin \
          -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -Wall -Wextra -Werror -O2 -g \
          -Iinclude -Iuser
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T user/user.ld

USERPROGS := hello mtest kecho
USERELF := $(USERPROGS:%=$(BUILD)/user/%.elf)

# ------------------------------------------------------------------
# Загрузчик: ABI UEFI x64 (Microsoft), -fno-ident для чистого PE.
# ------------------------------------------------------------------
LCFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-ident -mabi=ms \
          -mno-red-zone -Wall -Wextra -Werror -O2 -g \
          -Iinclude -Icommon -Iboot/uefi
LLDFLAGS := -m i386pep --image-base 0x100000 -e efi_main

KOBJS := $(BUILD)/kernel/entry.o \
	$(BUILD)/kernel/kmain.o \
	$(BUILD)/kernel/shell.o \
	$(BUILD)/kernel/arch/gdt.o \
	$(BUILD)/kernel/arch/idt.o \
	$(BUILD)/kernel/arch/idt_stub.o \
	$(BUILD)/kernel/arch/tss.o \
	$(BUILD)/kernel/arch/pic.o \
	$(BUILD)/kernel/arch/irq.o \
	$(BUILD)/kernel/mm/phys.o \
	$(BUILD)/kernel/mm/vmm.o \
	$(BUILD)/kernel/drivers/serial.o \
	$(BUILD)/kernel/drivers/fb.o \
	$(BUILD)/kernel/drivers/timer.o \
	$(BUILD)/kernel/drivers/ps2.o \
	$(BUILD)/kernel/drivers/kbd.o \
	$(BUILD)/kernel/drivers/mouse.o \
	$(BUILD)/kernel/drivers/rtc.o \
	$(BUILD)/kernel/drivers/pci.o \
	$(BUILD)/kernel/drivers/ahci.o \
	$(BUILD)/kernel/fs/blk.o \
	$(BUILD)/kernel/fs/ovl.o \
	$(BUILD)/kernel/fs/fatp.o \
	$(BUILD)/kernel/fs/fat32.o \
	$(BUILD)/kernel/gfx/console.o \
	$(BUILD)/kernel/gfx/ui.o \
	$(BUILD)/kernel/gfx/uidraw.o \
	$(BUILD)/kernel/gfx/desktop.o \
	$(BUILD)/kernel/gfx/dskapps.o \
	$(BUILD)/kernel/gfx/splash.o \
	$(BUILD)/kernel/gfx/login.o \
	$(BUILD)/kernel/sys/task.o \
	$(BUILD)/kernel/sys/switch.o \
	$(BUILD)/kernel/sys/syscall.o \
	$(BUILD)/kernel/sys/elf.o \
	$(BUILD)/kernel/sys/users.o \
	$(BUILD)/kernel/sys/session.o \
	$(BUILD)/kernel/sys/power.o \
	$(BUILD)/kernel/lib/log.o \
	$(BUILD)/kernel/lib/vsnprintf.o \
	$(BUILD)/kernel/lib/string.o \
	$(BUILD)/kernel/lib/errors.o \
	$(BUILD)/kernel/selftest/selftest.o \
	$(BUILD)/kernel/fbpi.o \
	$(BUILD)/kernel/mbr.o \
	$(BUILD)/kernel/sha256.o \
	$(BUILD)/kernel/foboscfg.o \
	$(BUILD)/kernel/cfglite.o

LOBJS := $(BUILD)/uefi/loader.o \
	$(BUILD)/uefi/efi_guids.o \
	$(BUILD)/uefi/lserial.o \
	$(BUILD)/uefi/lgfx.o \
	$(BUILD)/uefi/lin.o \
	$(BUILD)/uefi/oobe.o \
	$(BUILD)/uefi/uisup.o \
	$(BUILD)/uefi/bootctx.o \
	$(BUILD)/uefi/espfile.o \
	$(BUILD)/uefi/disk.o \
	$(BUILD)/uefi/fmt.o \
	$(BUILD)/uefi/ublk.o \
	$(BUILD)/uefi/setup.o \
	$(BUILD)/uefi/fat32.o \
	$(BUILD)/uefi/fatp.o \
	$(BUILD)/uefi/errors.o \
	$(BUILD)/uefi/vsnprintf.o \
	$(BUILD)/uefi/mbr.o \
	$(BUILD)/uefi/fmtcalc.o \
	$(BUILD)/uefi/fmtfat32.o \
	$(BUILD)/uefi/string.o \
	$(BUILD)/uefi/longmode.o \
	$(BUILD)/uefi/fbpi.o \
	$(BUILD)/uefi/sha256.o \
	$(BUILD)/uefi/foboscfg.o \
	$(BUILD)/uefi/cfglite.o

all: $(BUILD)/BOOTX64.efi $(BUILD)/kernel.elf $(USERELF)

# --- user-space ---
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/user/lib/%.o: user/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/user/lib/start.o: user/lib/start.S
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf -o $@ $<

USEROBJS := $(BUILD)/user/lib/fos.o $(BUILD)/user/lib/start.o

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(USEROBJS) user/user.ld
	$(LD) $(ULDFLAGS) -o $@ $(filter-out user/user.ld,$^)

# --- ядро ---
$(BUILD)/kernel.elf: $(KOBJS) kernel/kernel.ld
	$(LD) $(KLDFLAGS) -o $@ $(KOBJS)
	$(PYTHON) scripts/check_kernel_elf.py $@

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf -o $@ $<

$(BUILD)/kernel/fbpi.o: common/fbpi.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kernel/sha256.o: common/sha256.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kernel/mbr.o: common/mbr.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kernel/foboscfg.o: common/foboscfg.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kernel/cfglite.o: common/cfglite.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/fbpi.o: common/fbpi.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/sha256.o: common/sha256.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/foboscfg.o: common/foboscfg.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/cfglite.o: common/cfglite.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/string.o: kernel/lib/string.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

# --- загрузчик ---
$(BUILD)/BOOTX64.efi: $(LOBJS) | $(BUILD)
	$(LD) $(LLDFLAGS) -o $@ $(LOBJS)
	$(PYTHON) scripts/pe_patch.py $@

$(BUILD)/uefi/%.o: boot/uefi/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/%.o: boot/uefi/%.S
	@mkdir -p $(dir $@)
	$(NASM) -f win64 -g -o $@ $<

# --- общие модули в загрузчике (единые с ядром, без копий) ---
# fat32/fatp: ЕДИНЫЙ FAT32-драйвер (запись файлов установщика);
# платформенные зависимости подменяет boot/uefi/ublk.c.
$(BUILD)/uefi/fat32.o: kernel/fs/fat32.c $(BUILD)/uefi/ublk.o
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/fatp.o: kernel/fs/fatp.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/errors.o: kernel/lib/errors.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/vsnprintf.o: kernel/lib/vsnprintf.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/mbr.o: common/mbr.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/fmtcalc.o: common/fmtcalc.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/uefi/fmtfat32.o: common/fmtfat32.c
	@mkdir -p $(dir $@)
	$(CC) $(LCFLAGS) -MMD -MP -c -o $@ $<

# --- ESP-образ ---
$(BUILD)/esp.img: $(BUILD)/BOOTX64.efi $(BUILD)/kernel.elf $(USERELF) scripts/mkimg.py
	$(PYTHON) scripts/mkimg.py --out $@ --boot $(BUILD)/BOOTX64.efi --kernel $(BUILD)/kernel.elf \
           --user $(BUILD)/user/hello.elf=HELLO.ELF \
           --user $(BUILD)/user/mtest.elf=MTEST.ELF \
           --user $(BUILD)/user/kecho.elf=KECHO.ELF

esp: $(BUILD)/esp.img

# --- QEMU selftest ---
# CI-детерминизм: перед прогоном ESP пересобирается с нуля (сценарии
# пишут в образ setup.cfg/fobos.cfg; тестовый прогон обязан начинаться
# с чистого диска). Рекурсивный $(MAKE): rm гарантированно выполняется
# ДО решения о пересборке esp.img. Ручной `make run` образ НЕ затирает.
.PHONY: selftest
selftest:
	rm -f $(BUILD)/esp.img
	$(MAKE) --no-print-directory esp
	$(PYTHON) scripts/qemu_selftest.py --esp $(BUILD)/esp.img --log $(BUILD)/serial.log

# --- визуальный/интерактивный прогон (окно QEMU); при другой прошивке поправьте пути ---
OVMF_DIR ?= /usr/share/OVMF

run: $(BUILD)/esp.img
	cp $(OVMF_DIR)/OVMF_VARS_4M.fd $(BUILD)/VARS.run.fd
	$(QEMU) -machine q35 -m 512 -smp 1 \
           -drive if=pflash,format=raw,readonly=on,file=$(OVMF_DIR)/OVMF_CODE_4M.fd \
           -drive if=pflash,format=raw,file=$(BUILD)/VARS.run.fd \
           -drive file=$(BUILD)/esp.img,format=raw,if=ide \
           -serial stdio -display gtk,zoom-to-fit=off

# --- host unit-тесты ---
HOSTSRC := tests/host/runner.c tests/host/mock_log.c tests/host/blk_mock.c \
          kernel/selftest/selftest.c kernel/lib/vsnprintf.c kernel/lib/string.c \
          kernel/lib/errors.c kernel/mm/phys.c kernel/drivers/kbd.c \
          kernel/drivers/mouse.c kernel/drivers/rtc.c kernel/sys/users.c \
          kernel/sys/session.c kernel/fs/fatp.c kernel/fs/fat32.c kernel/fs/ovl.c \
           kernel/sys/elf.c \
          common/fbpi.c common/sha256.c common/foboscfg.c common/cfglite.c \
          common/mbr.c common/fmtcalc.c common/fmtfat32.c
HOSTHDRS := $(wildcard include/fobos/*.h) $(wildcard kernel/lib/*.h) \
           kernel/mm/phys.h kernel/sys/*.h kernel/selftest/selftest.h kernel/drivers/kbd.h \
           kernel/drivers/mouse.h kernel/drivers/rtc.h kernel/sys/*.h \
           common/*.h

$(BUILD)/host_tests: $(HOSTSRC) $(HOSTHDRS) | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -g -fno-tree-loop-distribute-patterns \
           -Iinclude -Icommon -I. -DFOBOS_HOST_TEST -o $@ $(HOSTSRC)

host-tests: $(BUILD)/host_tests
	$(BUILD)/host_tests

verify: host-tests all esp selftest

clean:
	rm -rf $(BUILD)

$(BUILD):
	mkdir -p $(BUILD)

.PHONY: all esp selftest run host-tests verify clean

-include $(KOBJS:.o=.d) $(LOBJS:.o=.d)
