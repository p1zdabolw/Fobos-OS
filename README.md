# FOS — Fobos Operating System

A from-scratch x86_64 graphical operating system written in C11 and NASM.

FOS boots via GRUB2 with Multiboot2, enters long mode, sets up paging,
drives PS/2 input, paints a software compositor on the linear framebuffer,
ships a small userspace of native GUI applications, and reaches the real
internet over plain HTTP. The interface is available in **English and
Russian**, switchable at runtime.

Current version: **0.3**.

## Screenshots

<img width="1278" height="717" alt="image" src="https://github.com/user-attachments/assets/5327ea73-32f8-416e-9a10-b74e8002c2ed" />


## What's new in 0.3

- Full **English / Russian interface** with a runtime toggle
- **Cyrillic keyboard layout** (ЙЦУКЕН) toggled with Shift+Alt
- **Screen resolution switcher** in Settings, driven by Bochs VBE
- **Wallpaper app** with eight procedural modes and eight procedural photos
- **Photos app** with slideshow and "Set as wallpaper"
- **Media Player** with PC-speaker playback and a spectrum visualizer
- **Settings app** covering resolution, language, clock format, mouse speed
- Extended network stack: **HTTP/1.1 with chunked transfer**, redirects, cookies
- **HTML text and link extraction** for rendering real web pages
- Batch **scripting** with variables, labels, and `goto`

## Features

### Kernel

- Custom GDT, TSS, and IDT for x86_64 long mode
- Bitmap physical memory manager with Multiboot2 mmap parsing
- 4-level paging (PML4) with map / unmap / query helpers
- First-fit heap allocator with coalescing (`kmalloc`, `kzalloc`,
  `kfree`, `krealloc`)
- PIT timer at 100 Hz, PS/2 keyboard and mouse drivers
- PIC remapping to vectors 32–47
- Basic PCI enumeration
- Bochs VBE graphics-mode driver at runtime
- PC-speaker driver via PIT channel 2
- ELF64 loader scaffolding, round-robin scheduler scaffolding
- SYSCALL / SYSRET MSR programming and an `int 0x80` dispatch path

### Graphics and UI

- Linear framebuffer with 32bpp, 24bpp, and 16bpp paths
- Double-buffered software rendering
- Runtime resolution switching between six modes
- Built-in 8×16 bitmap font covering **ASCII and Cyrillic**
- Compositor with wallpaper, desktop icons, taskbar, start menu,
  right-click context menu, and clock
- Window manager: overlapping windows, focus, drag, close
- Widget toolkit: buttons, labels, text fields
- Desktop icons with per-extension artwork
  (`.txt`, `.exe`, `.cmd` / `.bat`, `.url` / `.html`)
- Eight procedural wallpaper modes: solid, gradient V, gradient H,
  checker, plasma, starfield, diagonal, photo

### Localization

- Two-locale string table in `lib/i18n.c`
- English (US) and Russian (Русский) interface
- Runtime switch in Settings, applies instantly to menus, buttons,
  window titles, and app labels
- Cyrillic keyboard layout toggled with Shift+Alt
- Taskbar language indicator (EN / RU)
- Independent toggles for **keyboard language** and **interface language**

### Applications

- **Terminal** — ~45 commands modeled on a subset of bash, with
  command history and a batch-script interpreter
- **Browser** — HTTP/1.1 client with chunked decoding, redirects,
  cookie jar, and HTML text and link extraction
- **Notepad** — scratch editor with save-to-filesystem
- **Files** — in-memory filesystem browser
- **Photos** — eight procedural images with slideshow and
  set-as-wallpaper
- **Media Player** — five built-in chiptunes with a spectrum visualizer
- **Wallpaper** — eight modes, live switching
- **Settings** — display resolution, keyboard language, interface
  language, wallpaper shortcut, clock format, mouse speed, and a
  system-information panel
- **Script runner** — interprets `.bat`, `.cmd`, and `.exe` files as
  batch scripts

### Filesystem

- In-memory, initramfs-like flat filesystem
- Create, read, write, remove, list
- Ships with sample text files, batch scripts, and `.url` shortcuts

### Networking

- RTL8139 NIC driver (works under QEMU's user-mode NAT)
- Ethernet, ARP, IPv4 with checksums
- ICMP echo (ping), UDP with checksums
- DNS resolver over UDP
- TCP client with three-way handshake, sequence tracking, FIN handling
- HTTP/1.1 client:
  - `Host`, `User-Agent`, `Accept`, `Accept-Encoding`, `Cookie`
  - chunked transfer-encoding decoding
  - `Location` following up to 6 redirects
  - `Set-Cookie` storage with host-matching cookie jar
  - response content-type parsing
- HTML extractor with block elements, headings, lists, images with
  alt text, `<pre>` whitespace preservation, entity decoding, and
  absolute-link resolution

## Requirements

- `gcc` with `-m64` support
- `ld`
- `nasm`
- `grub-mkrescue` and `xorriso`
- `qemu-system-x86_64`

Install on Debian or Ubuntu:
sudo apt install build-essential nasm xorriso grub-pc-bin grub-common
qemu-system-x86 mtools

## Building
make

This produces `fos.elf` (the kernel, under 250 KB) and `fos.iso`
(a bootable El Torito image with GRUB2 as the loader).

## Running
make run

Equivalent raw invocation:
qemu-system-x86_64 -cdrom fos.iso -m 256 -serial stdio -vga std
-nic user,model=rtl8139
-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

`make run-nox` boots headless with serial-only output.

The `-audiodev pa` argument uses PulseAudio on the host. On Windows
hosts, replace `pa` with `dsound`. On systems without host audio, the
Media Player still runs and animates; only the sound is missing.

## Verifying the ISO
make verify-grub

Extracts and prints the `grub.cfg` that ended up inside the ISO — the
ground truth for how the bootloader will behave.

## Boot log

A successful boot prints roughly the following to serial:

FOS kernel starting
MB2 info at 0x00000000001000b0 total=1920
MB2 tag type=8 size=38
FB: 800x600 24bpp pitch=2400 addr=0x00000000fd000000 (vbe=yes)
PMM: 255 MB total, 254 MB free
RTL8139: io=c000 mac=52:54:0:12:34:56
NET: ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3
PCI 00:00.0 8086:1237 class=06.00
PCI 00:03.0 10ec:8139 class=02.00
FOS ready. Free: 252xxx KB, desktop icons: 11, layout: EN

## Using the system

### Language switching

Press **Shift + Alt** together to toggle between EN and RU. The taskbar
shows the current layout next to the clock. Cyrillic works in the
Terminal, Notepad, browser address bar, and the Settings test field.

To change the **interface language**, open **Start → Settings** and use
the *Interface Language* section. Everything changes immediately:
menu entries, button labels, window titles, section headers.

### Terminal commands

Files: `ls`, `cat`, `touch`, `write`, `rm`, `cp`, `mv`, `head`, `tail`,
`wc`, `grep`, `file`, `stat`, `tree`, `df`

Shell: `echo`, `clear`, `pwd`, `cd`, `which`, `history`

System: `ver`, `uname`, `mem`, `free`, `uptime`, `date`, `hostname`,
`whoami`, `sleep`

Network: `ifconfig`, `ping`, `nslookup`, `wget`, `curl`

Language: `lang [en|ru]`

Scripting: `run FILE`, `bash FILE`, `exec FILE`

### Network diagnostics
ifconfig
ping 10.0.2.2
nslookup example.com
wget http://example.com/

`ping 10.0.2.2` reaches QEMU's user-mode gateway. `ping 8.8.8.8` will
time out because QEMU's slirp does not forward ICMP beyond the guest.

### Browser

Type a URL or a query in the address bar:

- `http://example.com/`
- `http://info.cern.ch/`
- `http://neverssl.com/`
- `fos operating system` (searches the built-in index)
- `docs.local` (internal documentation)
- `about:home`

Plain HTTP sites render their text and links. HTTPS-only sites return a
redirect notice and offer HTTP-only fallbacks.

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

### Wallpaper

Open **Start → Wallpaper** or **Start → Settings → Open Wallpaper**.
Eight procedural modes: Solid, Gradient V, Gradient H, Checker,
Plasma, Stars, Diagonal, Photo. Click any swatch to apply.

The Photo mode reads from the same eight images the Photos app
displays. Clicking the Photo swatch cycles through them.

### Photos

Open **Start → Photos**. Use `< Prev` and `Next >` to navigate. Click
**Set Wallpaper** to make the current image the desktop background.
Click **Slideshow** to auto-advance every four seconds.

### Media Player

Open **Start → Media Player**. Click **Play** to start the current
track. **Next >** and `< Prev` cycle through five chiptunes. The
spectrum visualizer animates while playing. The PC speaker is a
square-wave device, so this plays simple tones rather than music.

### Settings

Open **Start → Settings**. Sections:

- **Display Resolution** — six modes, applied instantly
- **Keyboard Language** — English / Русский, with a live test field
- **Interface Language** — switches the UI between EN and RU
- **Wallpaper** — shortcut to the wallpaper app
- **Clock Format** — 12-hour or 24-hour taskbar clock
- **Mouse Speed** — Slow / Normal / Fast
- **System Information** — memory, uptime, network, kernel version

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
keyboard.c PS/2 keyboard with EN / RU layouts
mouse.c PS/2 mouse
pci.c PCI enumeration
fb.c Framebuffer with Bochs VBE runtime resize
fs.c In-memory filesystem
rtl8139.c Network card driver
net.c ARP, IPv4, ICMP, UDP, DNS, TCP
http.c HTTP/1.1 client and cookie jar
html.c HTML text and link extractor
speaker.c PC speaker via PIT channel 2
sched.c Scheduler scaffolding
syscall.c Syscall dispatch
elf.c ELF64 loader scaffolding

lib/
string.c mem.c printf.c
i18n.c English / Russian string tables

gui/
font.c Bitmap font with ASCII and Cyrillic
cursor.c Mouse cursor artwork
compositor.c Desktop, taskbar, menus
window.c Window manager
widget.c Buttons, labels, text fields
desktop.c Desktop icons
wallpaper.c Wallpaper modes and settings app
settings.c Control panel

apps/
terminal.c Shell
notepad.c Text editor
files.c File browser
script.c Batch-script interpreter
browser.c Internet Explorer
photos.c Photo viewer
media.c Media player

## Design notes

- **No libc, no libgcc, no external library.** Freestanding C11 only.
- **No floating point** anywhere in the kernel or applications.
- **No SSE, AVX, or MMX.** Baseline x86_64 integer instructions only.
- **No higher-half kernel.** The first 4 GiB are identity-mapped.
- **Compiles clean** under `-Wall -Wextra -Werror -std=c11
  -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
  -mcmodel=kernel -nostdlib -nostdinc`.
- **Small footprint.** Kernel ELF is under 250 KB.

## Localization notes

The interface is bilingual. All app labels, menu items, button text,
and window titles come from `lib/i18n.c`, which holds two parallel
tables indexed by a `STR_*` enumeration.

Everything a user navigates is translated. What is not translated, on
purpose:

- **Terminal command names** (`ls`, `cat`, `ping`) — universal
  Unix-style names, kept as-is for muscle memory
- **Terminal error messages** — following the standard where bash on a
  localized Linux system prints errors in English
- **Browser mock page content** — demo pages in `docs.local` are
  example content, not chrome
- **File names** — user data, kept ASCII so the shell can address them

Two language settings are independent: **keyboard language** determines
what keys produce (Shift+Alt toggles it), and **interface language**
determines what the OS displays.

## Known limitations

- **HTTPS is not implemented.** TLS requires SHA-256, AES or
  ChaCha20, X25519 or RSA, and X.509 parsing. Sites that redirect
  HTTP to HTTPS display a notice and offer HTTP-only fallbacks. This
  is the single largest gap between FOS and the modern web.
- **No image decoding.** PNG and JPEG are not decoded; the Photos app
  renders eight procedural images instead. `<img>` tags in HTML render
  as `[Image: alt-text]` placeholders.
- **In-memory filesystem only.** Everything is lost on reboot. There
  is no persistent storage driver (no ATA, AHCI, or NVMe).
- **Settings do not persist.** Language, resolution, clock format, and
  mouse speed reset to defaults on every boot, because there is no
  writable filesystem.
- **Single-threaded.** The scheduler is a stub and does not perform
  context switches. All applications run cooperatively in the kernel
  address space.
- **No userspace isolation.** There is no ring-3 execution, no
  per-process page tables, and no working ELF userspace loader.
- **RTL8139 only.** The network driver does not support e1000,
  virtio-net, or any other NIC.
- **Single TCP connection.** The client maintains one connection at a
  time.
- **No TLS, HTTP/2, or WebSocket.** Only HTTP/1.0 and HTTP/1.1 over
  plain TCP.
- **PC speaker only.** No AC'97 or HDA audio; the Media Player plays
  square-wave tones, not music with timbre.
- **No USB, SMP, sound card, or ACPI shutdown.** Power off from the
  QEMU window (Ctrl+Alt+G, then close) or by killing the process.

## Roadmap

- **TLS 1.3** with X25519 key exchange and ChaCha20-Poly1305 AEAD —
  this is what unlocks the modern web
- PNG decoding via a DEFLATE decompressor
- ATA or AHCI driver for persistent storage, so settings survive
  reboots
- Real preemptive multitasking with per-process address spaces
- Ring-3 userspace and a proper ELF loader
- e1000 and virtio-net drivers
- More keyboard layouts (German, French, Spanish)

## Origins

FOS was built as a single continuous project from a blank screen.
Every line of assembly, C, and linker script in the kernel, the
compositor, the window manager, the applications, the network stack,
and the localization layer was written from scratch against public
specifications: the Intel SDM, the Multiboot2 spec, the RTL8139
datasheet, the Bochs VBE extensions, the PCI Local Bus specification,
the Windows-1251 code page for Cyrillic, and RFCs 791, 792, 793, 826,
1035, and the HTTP/1.1 family.
