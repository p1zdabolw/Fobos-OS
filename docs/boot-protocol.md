# FBPI — Fobos Boot Protocol Interface

Версия: 0.1 (заголовок `include/fobos/boot_info.h` — единственный источник истины).
Статус: активен. Любое изменение структуры = bump FBPI_VERSION + запись в историю ниже.

## 1. Ответственность сторон

### Загрузчик (`boot/uefi/loader.c`) обязан:
1. Прочитать `\fobos\kernel.elf` с ESP (SimpleFileSystem), валидировать ELF64
   ET_EXEC/x86_64, проверить `p_vaddr - p_paddr == KBASE` для каждого PT_LOAD.
2. Разместить ядро по фиксированной физической базе **KPHYS = 0x200000** (2 МиБ)
   методом `AllocatePages(AllocateAddress)`. Ядро собрано с фиксированной
   higher-half раскладкой, перемещение образа на FND-1 невозможно.
3. Получить framebuffer через GOP (режим прошивки; BltOnly — фатальная ошибка).
4. Запросить карту памяти `GetMemoryMap` (буфер — EfiBootServicesData < 4 ГиБ).
5. Построить начальные таблицы страниц (см. §3).
6. Заполнить `struct fbpi_boot_info` (192 байта), вычислить checksum.
7. Вызвать `SetWatchdogTimer(0)`, затем `ExitBootServices` с ретраем по
   `EFI_INVALID_PARAMETER` (карта могла измениться).
8. Передать управление: `CR3 = PML4`, `RSP = стек перехода`,
   `RDI = физ. адрес boot_info` (SysV), `JMP e_entry` (виртуальный адрес).

Кроме того, на FND-2b загрузчик выполняет фазу System Setup (см. §7):
окно boot manager (ESC = сброс Setup, SHIFT = консоль ядра), OOBE-мастер
первого запуска, чтение/запись UEFI-переменной `FobosSetup`.

### Ядро обязано:
1. Валидировать boot_info (`fbpi_validate`): магия, версия, размер, checksum,
   здравомыслие полей. Ошибка — panic с диагностированием.
2. Считать зарезервированной всю память, не помеченную картой EFI как
   `EfiConventionalMemory` (это автоматически защищает код/данные загрузчика,
   буфер карты и boot info).
3. Не разыменовывать `efi_system_table_phys` на FND-1 (таблицы прошивки
   доступны только до ExitBootServices; виртуальный адресный прогон — FND-2).

## 2. Размещение ядра (FBPI v0)

| Константа    | Значение               | Где дублируется                    |
|--------------|------------------------|------------------------------------|
| KBASE        | `0xFFFFFFFF80000000`   | kernel.ld, entry.S, loader.c       |
| KPHYS        | `0x0000000000200000`   | kernel.ld, loader.c, check_kernel_elf.py |
| e_entry      | KBASE + KPHYS          | проверяется check_kernel_elf.py    |
| верх окна    | `0x40000000` (4 ГиБ)   | loader.c, check_kernel_elf.py      |

Почему 2 МиБ, а не 1 МиБ: OVMF загружает EFI-приложение по нижней свободной
области — фактически в 0x100000 (подтверждено дампом карты памяти). Образ
загрузчика (~52 КиБ) обязан остаться зарезервированным, поэтому ядро сдвинуто
на 2 МиБ с запасом.

Согласованность констант между kernel.ld / loader.c / entry.S /
check_kernel_elf.py проверяется автоматически на каждой сборке ядра.

## 3. Начальная раскладка страниц (временная, FND-1)

Страницы 2 МиБ, флаги PRESENT|WRITE, **NX не выставляется** — это осознанное
упрощение загрузочного пути; полноценный W^X строит VMM в FND-2 и заменяет
эти таблицы собственными.

```
PML4[0]   -> PDPT0[0..3] : identity 0..4 ГиБ (4 x PD x 512 x 2 МиБ)
PML4[0]   -> PDPT0[i>=4] : ленивые PD (до-маппирование регионов >= 4 ГиБ,
                            если прошивка загрузила образ загрузчика высоко)
PML4[511] -> PDPT511[510] -> PDHH : V 0xFFFFFFFF80000000..+1 ГиБ -> P 0..+1 ГиБ
```

Ядро получает управление с этим CR3 и НЕ обязано сохранять таблицы: FND-2
строит собственное адресное пространство. До этого момента действует правило:
аллокатор кадров выдаёт только память < 4 ГиБ (через identity-окно она видна).

## 4. Передача управления (ABI прыжка)

| Регистр | Значение                                   |
|---------|--------------------------------------------|
| CR3     | физ. адрес PML4                            |
| RSP     | вершина стека перехода (32 КиБ, < 4 ГиБ)   |
| RDI     | физ. адрес `struct fbpi_boot_info`         |
| RIP     | `e_entry` из ELF (виртуальный, higher-half)|

Первое действие ядра (`kernel/entry.S`): установить собственный стек (64 КиБ,
.bss ядра) и преобразовать RDI из физического адреса в виртуальный
(`+ KBASE`).

## 5. Поля struct fbpi_boot_info (v0.1)

| Смещение | Поле                  | Тип                      | Смысл |
|----------|-----------------------|--------------------------|-------|
| 0x00     | magic                 | u32 = 0x31504246 "FBP1"  | сигнатура |
| 0x04     | version               | u16 = 1                  | версия протокола |
| 0x06     | header_size           | u16 = 192                | sizeof |
| 0x08     | checksum              | u32                      | дополнение суммы u32-слов до нуля |
| 0x0C     | kernel_size_bytes     | u32                      | span всех PT_LOAD |
| 0x10     | reserved0             | u32 + паддинг            | — |
| 0x18     | kernel_vbase          | u64                      | = KBASE |
| 0x20     | kernel_pbase          | u64                      | = KPHYS |
| 0x28     | efi_mmap_phys         | u64                      | буфер карты памяти |
| 0x30     | efi_mmap_size         | u32                      | байт данных |
| 0x34     | efi_desc_size         | u32                      | шаг дескриптора (>= 40, кратно 8) |
| 0x38     | efi_desc_version      | u32                      | версия от прошивки |
| 0x3C     | reserved1             | u32                      | — |
| 0x40     | efi_system_table_phys | u64                      | для SetVirtualAddressMap (FND-2) |
| 0x48     | cfg_phys              | u64                      | блоб настроек foboscfg (0 = нет) |
| 0x50     | cfg_size              | u32                      | размер блога (96..64 КиБ) |
| 0x54     | boot_flags            | u32                      | FBPI_BOOTF_* |
| 0x58     | fb                    | fbpi_framebuffer (40 Б)  | GOP framebuffer |
| 0x80     | firmware_vendor[64]   | char                     | ASCII, скопирован загрузчиком |

`boot_flags`: бит 0 `FBPI_BOOTF_CONSOLE` — запрос режима kernel console
(поставлен загрузчиком: Shift в окне boot manager). Неизвестные биты —
ошибка валидации (расхождение ABI).

## 6. История версий

- **v0** — начальная версия: EFI mmap + GOP framebuffer + фиксированное
  размещение ядра @ 2 МиБ + указатели на таблицы EFI. Известные ограничения
  (снимаются в FND-2): память выше 4 ГиБ не выдаётся кадрами; NX/W^X не
  настроены; адрес ядра фиксирован (KASLR — отдельный этап).
- **v0.1** (FND-2b) — добавлены `cfg_phys`/`cfg_size` (блоб настроек
  foboscfg, создан OOBE-мастером в загрузчике) и `boot_flags`
  (FBPI_BOOTF_CONSOLE — запрос консоли ядра). Размер 176 -> 192 байта.

## 7. Фаза System Setup и UEFI-переменная FobosSetup

В окне boot manager (1 с после старта загрузчика, подсказка в верхней
строке):
- **ESC** — сброс настроек: переменная `FobosSetup` удаляется, запускается
  OOBE-мастер заново;
- **SHIFT** — запрос режима kernel console: загрузчик ставит
  `FBPI_BOOTF_CONSOLE`, ядро грузится сразу в kshell (обслуживание, без
  сессии; OOBE пропускается). Детекция — прямой опрос 8042 (0x60/0x64)
  и SimpleTextInputEx; см. `boot/uefi/lin.c`.

Если настроек нет (переменной нет / повреждена) — графический OOBE-мастер
(шаги: welcome -> hostname -> [username -> password -> confirm -> more]* ->
save). Итог кодируется `common/foboscfg.c` и:
1. сохраняется в UEFI-переменную **FobosSetup** (vendor GUID
   `4F626F46-6F73-4F53-8CD1-4C552158F10B`, атрибуты NV|BS|RT);
2. копируется в память EfiLoaderData < 4 ГиБ и передаётся ядру через
   `cfg_phys`/`cfg_size`.

Формат блоба: заголовок 64 Б (magic "FBS2", version, flags, user_count,
hostname[32]), записи пользователей по 88 Б (name[32], is_admin, salt[16],
hash[32]), тег SHA-256 по телу. Пароли НЕ хранятся: только соль и KDF-хеш
(`foboscfg_kdf`: 4096 итераций SHA-256 с индексом раунда; замена на
PBKDF2-HMAC/Argon2id — FND-3+, см. roadmap).

До появления дисковой подсистемы (FND-2c: PCI/AHCI/FAT32) переменная
`FobosSetup` — единственный носитель конфигурации: она переживает
перезагрузку и полное выключение, но рантайм-изменения (passwd, useradd)
живут только до перезагрузки.

## 8. Поток загрузки FND-2b

```
UEFI -> BOOTX64.EFI:
  boot manager (1 c: ESC=Setup-reset, SHIFT=kernel console)
  [нет FobosSetup -> OOBE-мастер -> SetVariable]
  kernel.elf -> boot_info(FBPI v0.1) -> ExitBootServices -> jump

Ядро:
  kselftest (40) -> RTC -> splash (3 c: удержание Shift = консоль)
  [FBPI_BOOTF_CONSOLE или Shift] -> kernel console (fobos(kconsole)>)
  [иначе] -> нет пользователей -> "setup required" (Enter = reboot в Setup)
          -> есть пользователи -> экран входа (KDF-проверка пароля)
                                -> рабочий стол (kshell от имени пользователя)
  Ctrl+Alt+Del — перезагрузка в любом режиме.
```
