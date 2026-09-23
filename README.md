# FOS — Fobos Operating System

A from-scratch x86_64 graphical operating system written in C11 and NASM.

FOS boots via GRUB2 with Multiboot2, enters long mode, sets up paging,
drives PS/2 input, paints a software compositor on the linear framebuffer,
and ships a small userspace of native GUI applications. It also has a
working TCP/IP stack and can fetch and render plain-HTTP pages from the
real internet.

## Screenshots

<img width="634" height="475" alt="Снимок экрана 2026-09-23 164922" src="https://github.com/user-attachments/assets/be8fa5ef-718d-4ce4-9508-c9f0330ec824" />
<img width="637" height="475" alt="Снимок экрана 2026-09-23 164635" src="https://github.com/user-attachments/assets/644071f4-977e-492a-8ee1-a3b9bfd20dbb" />


## Features

### Kernel

- Custom GDT, TSS, and IDT for x86_64 long mode
- Bitmap physical memory manager with Multiboot2 mmap parsing
- 4-level paging (PML4) with map / unmap / query helpers
- First-fit heap allocator with coalescing (`kmalloc`, `kzalloc`,
  `kfree`, `krealloc`)
- PIT timer at 100 Hz
- PS/2 keyboard and mouse drivers with proper packet-phase handling
- PIC remapping to vectors 32–47
- Basic PCI enumeration
- ELF64 loader scaffolding
- Round-robin scheduler scaffolding
- SYSCALL / SYSRET MSR programming and an `int 0x80` dispatch path

### Graphics and UI

- Linear framebuffer abstraction with 32bpp, 24bpp, and 16bpp paths
- Double-buffered software rendering
- Built-in 8×16 bitmap font
- Custom compositor with desktop wallpaper, taskbar, start menu,
  right-click context menu, and clock
- Window manager: overlapping windows, focus, drag, close
- Widget toolkit: buttons, labels, text boxes
- Desktop icon system with per-extension icon artwork
  (`.txt`, `.exe`, `.cmd` / `.bat`, `.url` / `.html`, and a fallback)
- PS/2 cursor with proper hotspot and pixel-art rendering

### Applications

- **Terminal** — around 45 commands modeled on a subset of bash,
  with command history and a batch-script interpreter
- **Notepad** — scratch editor with a working save-to-filesystem bar
- **Files** — in-memory filesystem browser
- **Internet Explorer** — HTTP/1.1 client with chunked decoding,
  redirect following, cookie jar, and HTML text and link extraction
- **Script runner** — interprets `.bat`, `.cmd`, and `.exe` files as
  batch scripts with variables, labels, and `goto`

### Filesystem

- In-memory, initramfs-like flat filesystem
- Create, read, write, remove, list
- Ships with sample text files, batch scripts, and `.url` shortcuts

### Networking

- RTL8139 NIC driver (works under QEMU's user-mode NAT)
- Ethernet, ARP, IPv4 with checksums
- ICMP echo (ping)
- UDP with checksums
- DNS resolver over UDP
- TCP client with three-way handshake, sequence tracking, and
  proper FIN handling
- HTTP/1.1 client with:
  - `Host`, `User-Agent`, `Accept`, `Accept-Encoding`, `Cookie`
  - chunked transfer-encoding decoding
  - `Location` following, up to 6 redirect hops
  - `Set-Cookie` storage with host-matching cookie jar
  - response content-type parsing

## Requirements

- `gcc` with `-m64` support
- `ld`
- `nasm`
- `grub-mkrescue` and `xorriso` (Debian/Ubuntu: `grub-pc-bin`,
  `grub-common`, `xorriso`, `mtools`)
- `qemu-system-x86_64`

Install on Debian or Ubuntu:
sudo apt install build-essential nasm xorriso grub-pc-bin grub-common qemu-system-x86 mtools
## Building

This produces `fos.elf` (the kernel) and `fos.iso` (a bootable
Elbe-Torito image with GRUB2 as the loader).

## Running
make run
Equivalent raw invocation:
qemu-system-x86_64 -cdrom fos.iso -m 256 -serial stdio -vga std -nic user,model=rtl8139

`make run-nox` boots headless and sends everything to serial stdout,
useful for CI or for reading kernel logs without a display.

## Verifying the ISO
make verify-grub

This extracts the `grub.cfg` that ended up inside the ISO and prints
it, which is the ground truth for how the bootloader will behave.

## Boot log

A successful boot prints roughly the following to serial:
FOS kernel starting
MB2 info at 0x00000000001000b0 total=1920
MB2 tag type=8 size=38
FB: 800x600 24bpp pitch=2400 addr=0x00000000fd000000
PMM: 255 MB total, 254 MB free
RTL8139: io=c000 mac=52:54:0:12:34:56
NET: ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3
PCI 00:00.0 8086:1237 class=06.00
PCI 00:03.0 10ec:8139 class=02.00
FOS ready. Free: 252296 KB, desktop icons: 11

## Using the system

### Terminal commands

Files: `ls`, `cat`, `touch`, `write`, `rm`, `cp`, `mv`, `head`, `tail`,
`wc`, `grep`, `file`, `stat`, `tree`, `df`

Shell: `echo`, `clear`, `pwd`, `cd`, `which`, `history`

System: `ver`, `uname`, `mem`, `free`, `uptime`, `date`, `hostname`,
`whoami`, `sleep`

Network: `ifconfig`, `ping`, `nslookup`, `wget`, `curl`

Scripting: `run FILE`, `bash FILE`, `exec FILE`

### Network diagnostics
ifconfig
ping 10.0.2.2
nslookup example.com
wget http://example.com/

`ping 10.0.2.2` reaches QEMU's user-mode gateway. `ping 8.8.8.8` will
time out because QEMU's slirp does not forward ICMP beyond the guest.

### Browser

Type a URL or a query into the address bar:

- `http://example.com/`
- `http://info.cern.ch/`
- `http://neverssl.com/`
- `fos operating system` (searches the built-in index)
- `docs.local` (internal documentation)
- `about:home`

Plain HTTP sites render their text and links. HTTPS-only sites
return a redirect notice.

### Batch scripts

Scripts are plain text with `.bat`, `.cmd`, or `.exe` extensions.
Supported directives:
@echo off suppress command echo
@echo on restore command echo
echo TEXT print TEXT
set NAME=VALUE define a variable
%NAME% expand a variable inline
:label define a label
goto label jump to a label
pause print wait message, delay 1.5 seconds
rem TEXT comment
:: TEXT comment
exit end the script

Any other line is dispatched to the terminal, so scripts can call
`ls`, `cat`, `cp`, `mv`, `grep`, `echo`, and every other command.

## Architecture
boot/
multiboot2.asm Multiboot2 header, long-mode entry, boot paging
grub.cfg GRUB configuration

kernel/
kmain.c Kernel entry after long mode
gdt.c/idt.c Descriptor tables
isr.asm Interrupt stubs
pmm.c/vmm.c Physical and virtual memory
heap.c Kernel heap allocator
timer.c PIT driver
keyboard.c PS/2 keyboard
mouse.c PS/2 mouse
pci.c PCI enumeration
fb.c Framebuffer abstraction and double buffering
fs.c In-memory filesystem
rtl8139.c Network card driver
net.c ARP, IPv4, ICMP, UDP, DNS, TCP
http.c HTTP/1.1 client and cookie jar
html.c HTML text and link extractor
sched.c Scheduler scaffolding
syscall.c Syscall dispatch
elf.c ELF64 loader scaffolding

lib/
string.c mem.c printf.c

gui/
font.c Bitmap font
cursor.c Mouse cursor artwork
compositor.c Desktop, taskbar, menus
window.c Window manager
widget.c Buttons, labels, text boxes
desktop.c Desktop icons

apps/
terminal.c Shell
notepad.c Text editor
files.c File browser
script.c Batch-script interpreter
browser.c Internet Explorer

## Design notes

- **No libc, no libgcc, no external library.** Freestanding C11 only.
- **No floating point** anywhere in the kernel or applications.
- **No SSE, AVX, or MMX.** Baseline x86_64 integer instructions only.
- **No higher-half kernel.** The first 4 GiB are identity-mapped so
  physical addresses are directly accessible.
- **Compiles clean** under `-Wall -Wextra -Werror -std=c11
  -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
  -mcmodel=kernel -nostdlib -nostdinc`.
- **Small footprint.** Kernel ELF is under 200 KB.

## Known limitations

- **HTTPS is not implemented.** TLS requires SHA-256, AES or
  ChaCha20, X25519 or RSA, and X.509 parsing, all of which are
  absent. Sites that redirect HTTP to HTTPS display a notice and
  offer HTTP-only fallbacks. This is the single largest gap.
- **No image decoding.** PNG and JPEG are not decoded; `<img>` tags
  render as `[Image: alt-text]` placeholders.
- **In-memory filesystem only.** Everything is lost on reboot.
  There is no persistent storage driver (no ATA, AHCI, or NVMe).
- **Single-threaded.** The scheduler is a stub and does not perform
  context switches. All applications run cooperatively in the kernel
  address space.
- **No userspace isolation.** There is no ring-3 execution, no
  per-process page tables, and no working ELF userspace loader.
- **RTL8139 only.** The network driver does not support e1000,
  virtio-net, or any other NIC.
- **Single connection.** The TCP client maintains one connection
  at a time.
- **No TLS, HTTP/2, or WebSocket.** Only HTTP/1.0 and HTTP/1.1 over
  plain TCP.
- **No sound, USB, or SMP.**

## Roadmap

- TLS 1.3 with X25519 key exchange and ChaCha20-Poly1305 AEAD
  (this unlocks the modern web)
- PNG decoding via a DEFLATE decompressor
- ATA or AHCI driver for persistent storage
- Real preemptive multitasking with per-process address spaces
- Ring-3 userspace and a proper ELF loader
- e1000 and virtio-net drivers

## Origins

FOS was built as a single continuous project from a blank screen.
Every line of assembly, C, and linker script in the kernel, the
compositor, the window manager, the applications, and the network
stack was written from scratch against public specifications:
the Intel SDM, the Multiboot2 spec, the RTL8139 datasheet, the
PCI Local Bus specification, RFC 791, 792, 793, 826, 1035, and
the HTTP/1.1 RFCs.

