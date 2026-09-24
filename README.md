# FOS — Fobos Operating System

A from-scratch x86_64 graphical operating system written in C11 and NASM.

FOS boots via GRUB2 with Multiboot2, enters long mode, sets up paging,
drives PS/2 input, paints a software compositor on the linear framebuffer,
ships a small userspace of native GUI applications, has real preemptive
multitasking, persistent storage, and reaches the real internet over plain
HTTP. The interface runs in **English and Russian**, switchable at runtime.

Current version: **0.4**.

## What's new in 0.4

- **Real preemptive multitasking** — 32 priority levels, real context
  switches driven by the 1 kHz timer, round-robin within priority
- **Soft real-time support** — periodic tasks with millisecond deadlines
  and per-task jitter accounting (`rtstat` in the terminal)
- **ATA PIO driver** and **DFS filesystem** on a persistent disk
- **Settings and files survive reboot** — FOS is no longer live-only
- **RTC driver** — wall-clock time from the CMOS chip
- **Timezone selection** — 28 presets, offset applied to the RTC reading
- **Taskbar close buttons** and **right-click to close** — no need to
  reach the window on small screens
- **Off-screen window dragging** — windows can be pushed mostly off any
  edge and always dragged back
- **Window size restoration** across resolution changes
- Terminal gains `sync`, `save`, `tz`, and `rtstat` commands

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

## Live mode, and what changed

FOS **is no longer live-only**. It has an ATA PIO driver, a DFS filesystem
on disk, and persists settings, files, and timezone across reboots.

What FOS still is:

- **Booted from the CD.** The kernel lives on the ISO and is loaded into
  RAM by GRUB every boot. There is no installer yet that copies the
  kernel to disk and configures a bootloader for it.
- **Safe to try on any machine.** The ATA driver only touches the disk
  it is explicitly given on the QEMU command line (`-drive file=...`).
  It never inspects or modifies other disks.

What FOS is not:

- **Not live-only anymore.** Files you write and settings you change
  survive reboot, because they are persisted to `fos.img` on each
  write through the in-memory cache.
- **Not installed in the traditional sense.** If you remove the CD,
  the machine cannot boot FOS. That is what an installer would fix.

Think of it as **live boot with a persistent home partition**. The OS
comes from the CD; the data comes from the disk.

## Features

### Kernel

- Custom GDT, TSS, and IDT for x86_64 long mode
- Bitmap physical memory manager with Multiboot2 mmap parsing
- 4-level paging (PML4) with map / unmap / query helpers
- First-fit heap allocator with coalescing (`kmalloc`, `kzalloc`,
  `kfree`, `krealloc`)
- **Preemptive scheduler** with 32 priority levels, per-task quantum,
  sleep/wake, and periodic-task API
- **1 kHz PIT** driving the scheduler
- **CMOS RTC** with timezone offset
- PS/2 keyboard (English + Russian) and mouse
- PIC remapping to vectors 32–47
- PCI enumeration
- Bochs VBE graphics-mode driver
- PC-speaker driver via PIT channel 2
- **ATA PIO driver** with LBA28 addressing
- **DFS filesystem** with a 512-byte superblock, 16-sector directory,
  and contiguous file extents
- SYSCALL / SYSRET MSR programming and an `int 0x80` dispatch path

### Graphics and UI

- Linear framebuffer with 32bpp, 24bpp, and 16bpp paths
- Double-buffered software rendering
- Runtime resolution switching between six modes
- Built-in 8×16 bitmap font covering **ASCII and Cyrillic**
- Compositor with wallpaper, desktop icons, taskbar, start menu,
  right-click context menu, and clock
- Window manager: overlapping windows, focus, drag, close, **taskbar
  close button, right-click-to-close, off-screen clamp**
- Widget toolkit: buttons, labels, text fields
- Desktop icons with per-extension artwork
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
- **Timezone selection** with 28 presets from UTC-12 to UTC+14

### Applications

- **Terminal** — ~50 commands modeled on a subset of bash, with
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
  language, timezone, wallpaper shortcut, clock format, mouse speed,
  and a system-information panel
- **Script runner** — interprets `.bat`, `.cmd`, and `.exe` files as
  batch scripts

### Filesystem

- In-memory cache in front of a persistent on-disk filesystem
- Every write is flushed to disk in the same call
- Create, read, write, remove, list
- Files survive reboot
- Flat namespace, no directories, no permissions, no timestamps

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

### Real-time

- Periodic tasks with millisecond deadlines
- Strict priority preemption
- Per-task jitter accounting: max, average, and missed-periods
- `rtstat` in the terminal shows the live table
- Soft real-time only: no WCET analysis, no priority inheritance,
  no measured interrupt latency bound

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

This produces:

- `fos.elf` — the kernel (under 350 KB)
- `fos.iso` — a bootable El Torito image with GRUB2 as the loader
- `fos.img` — a 10 MB disk image, created on first build

## Running
make run

Equivalent raw invocation:
qemu-system-x86_64 -cdrom fos.iso -m 256 -serial stdio -vga std
-drive file=fos.img,format=raw,if=ide,index=0,media=disk
-nic user,model=rtl8139
-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

`make run-nox` boots headless with serial-only output.

The `-audiodev pa` argument uses PulseAudio on the host. On Windows
hosts, replace `pa` with `dsound`. On systems without host audio, the
Media Player still runs and animates; only the sound is missing.

## Resetting the disk

To start from a fresh, unformatted disk:
make reset-disk

The next `make run` will reformat `fos.img` and FOS will boot with
default settings.

## Verifying the ISO
make verify-grub

Extracts and prints the `grub.cfg` that ended up inside the ISO — the
ground truth for how the bootloader will behave.

## Boot log

A successful boot prints roughly the following to serial:
FOS kernel starting
FB: 800x600 24bpp pitch=2400 addr=0x00000000fd000000 (vbe=yes)
PMM: 255 MB total, 244 MB free
RTL8139: io=c000 mac=52:54:0:12:34:56
NET: ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3
PCI 00:00.0 8086:1237 class=06.00
PCI 00:03.0 10ec:8139 class=02.00
RTC: raw 13:31:32 09/24/26 bcd=1
ATA: QEMU HARDDISK 20480 sectors (10 MB)
DFS: no filesystem, formatting
DFS: formatted 10 MB disk
FOS ready. Free: 240400 KB, storage: ATA/DFS
logger: 2026-09-24 13:31:33 jitter=0/0 missed=0
logger: 2026-09-24 13:31:34 jitter=1/0 missed=0

Second boot:
ATA: QEMU HARDDISK 20480 sectors (10 MB)
DFS: mounted, 8 files, next free sector 21
Settings: loaded

## Using the system

### Language switching

Press **Shift + Alt** together to toggle between EN and RU. The taskbar
shows the current layout next to the clock. Cyrillic works in the
Terminal, Notepad, browser address bar, and the Settings test field.

To change the **interface language**, open **Start → Settings** and use
the *Interface Language* section. Everything changes immediately:
menu entries, button labels, window titles, section headers.

### Window management

- **Click and drag the title bar** to move a window. Windows can be
  pushed mostly off any edge; 48 pixels of the title bar always remain
  reachable so you can drag them back.
- **Click the red square in the title bar** to close.
- **Right-click any taskbar button** to close that window.
- **Click the small red square on the right side of a taskbar button**
  to close it as well.
- **Click the taskbar button** to raise and focus.

### Terminal commands

Files: `ls`, `cat`, `touch`, `write`, `rm`, `cp`, `mv`, `head`, `tail`,
`wc`, `grep`, `file`, `stat`, `tree`, `df`

Shell: `echo`, `clear`, `pwd`, `cd`, `which`, `history`

System: `ver`, `uname`, `mem`, `free`, `uptime`, `date`, `hostname`,
`whoami`, `sleep`

Network: `ifconfig`, `ping`, `nslookup`, `wget`, `curl`

Language: `lang [en|ru]`

Timezone: `tz` (show current), `tz list` (list presets), `tz N`
(0..27)

Storage: `sync` (flush to disk), `save` (persist settings)

Real-time: `rtstat` (jitter table)

Scripting: `run FILE`, `bash FILE`, `exec FILE`

### Timezone

The RTC on most machines holds either local time (typical for QEMU
and laptops) or UTC (typical for servers). FOS reads it verbatim and
applies a user-configured offset:

- If your RTC holds **local time**, leave the offset at `UTC+00:00
  London` and the clock is correct.
- If your RTC holds **UTC**, set the offset to your local timezone
  and the clock is correct.

The Settings app's Timezone section has 28 presets covering UTC-12
to UTC+14. The terminal `tz` command accepts the same presets by
index.

Settings persist across reboot.

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

Any other line is dispatched to the terminal.

### Wallpaper

Open **Start → Wallpaper** or **Start → Settings → Open Wallpaper**.
Eight procedural modes: Solid, Gradient V, Gradient H, Checker,
Plasma, Stars, Diagonal, Photo. Click any swatch to apply. Persists
across reboot.

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
- **Keyboard Language** — English / Русский
- **Interface Language** — English / Русский
- **Timezone** — 28 presets, `<` and `>` to cycle
- **Wallpaper** — shortcut to the wallpaper app
- **Clock Format** — 12-hour or 24-hour
- **Mouse Speed** — Slow / Normal / Fast
- **System Information** — memory, uptime, network, kernel version

Every change persists to disk and is reloaded on the next boot.

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
timer.c PIT driver at 1 kHz
rtc.c CMOS real-time clock with timezone
keyboard.c PS/2 keyboard with EN / RU layouts
mouse.c PS/2 mouse
pci.c PCI enumeration
fb.c Framebuffer with Bochs VBE runtime resize
fs.c In-memory filesystem cache
ata.c ATA PIO driver
diskfs.c On-disk DFS filesystem
sched.c Preemptive scheduler with priority and deadlines
sched_asm.asm Context-switch primitives
syscall.c Syscall dispatch
elf.c ELF64 loader scaffolding
rtl8139.c Network card driver
net.c ARP, IPv4, ICMP, UDP, DNS, TCP
http.c HTTP/1.1 client and cookie jar
html.c HTML text and link extractor
speaker.c PC speaker via PIT channel 2

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
settings.c Control panel with persistence

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
- **Small footprint.** Kernel ELF is under 350 KB.

## Real-time notes

The scheduler is **soft real-time capable**. Periodic tasks at
priority 0 preempt everything else and hit their deadlines with the
jitter shown by `rtstat`. On QEMU, max jitter is typically 0–2 ms and
missed periods are 0. On real hardware, expect occasional 1–3 ms
spikes from cache misses and device IRQs.

This is not hard real-time. The three things that would be needed:

- Measured and bounded interrupt latency (currently unmeasured)
- Priority inheritance for shared resources (currently absent)
- Formal worst-case execution time analysis for every ISR and the
  scheduler path

Each is a project of its own.

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
- **ATA PIO only.** Reads and writes work at PIO speed. No DMA, no
  NCQ, no secondary channel. Only the primary master IDE drive is used.
- **DFS does not free orphaned sectors.** Rewriting a file at a
  different size leaves the old sectors reserved. Fine for the disk
  sizes FOS targets; a real free-list would be needed for a larger
  filesystem.
- **No image decoding.** PNG and JPEG are not decoded; the Photos app
  renders eight procedural images instead. `<img>` tags in HTML render
  as `[Image: alt-text]` placeholders.
- **No installer.** FOS boots from CD only. The disk holds data, not
  the OS. Installing FOS to disk would require a bootloader installer
  and an ext2/4 or FOS-native writer.
- **Single-threaded syscall path.** Interrupts are disabled during
  some kernel operations; there is no per-CPU locking because the
  system is UP-only.
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
- **Soft real-time only.** See the Real-time notes above.

## Roadmap

- **TLS 1.3** with X25519 key exchange and ChaCha20-Poly1305 AEAD —
  this is what unlocks the modern web
- **Installer** that writes the kernel to disk and configures GRUB
- PNG decoding via a DEFLATE decompressor
- AHCI driver for modern SATA disks
- Ring-3 userspace and a proper ELF loader
- e1000 and virtio-net drivers
- More keyboard layouts (German, French, Spanish)
- Hard real-time work: interrupt latency measurement, priority
  inheritance, WCET analysis

## Origins

FOS was built as a single continuous project from a blank screen.
Every line of assembly, C, and linker script in the kernel, the
compositor, the window manager, the applications, the network stack,
the scheduler, the storage subsystem, and the localization layer was
written from scratch against public specifications: the Intel SDM,
the Multiboot2 spec, the RTL8139 datasheet, the ATA-4 specification,
the Bochs VBE extensions, the PCI Local Bus specification, the
Windows-1251 code page for Cyrillic, and RFCs 791, 792, 793, 826,
1035, and the HTTP/1.1 family.

## License

MIT.
