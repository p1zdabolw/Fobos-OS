/*
 * Fobos OS — минимальные определения UEFI x64 (по UEFI Specification 2.10).
 *
 * Это СОБСТВЕННЫЙ заголовок (внешние библиотеки вроде gnu-efi не используются):
 * описаны только те таблицы и протоколы, которые нужны загрузчику. Порядок и
 * типы членов взяты из спецификации дословно — таблицы прошивки обязаны
 * совпадать байт-в-байт. Корректность раскладки защищена _Static_assert по
 * смещениям ключевых членов и runtime-проверкой сигнатур таблиц.
 *
 * Загрузчик компилируется с -mabi=ms: ABI вызова UEFI x64 — Microsoft x64
 * (аргументы rcx, rdx, r8, r9 + 32 байта shadow space, red zone запрещена).
 */
#ifndef FOBOS_UEFI_EFI_H
#define FOBOS_UEFI_EFI_H

#include <fobos/types.h>
#include <stddef.h>

/* Forward-объявления и канонические UEFI-имена (typedef до определения —
 * это устраняет “declared inside parameter list” и даёт привычные имена типов). */
struct EFI_DEVICE_PATH_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_INPUT_KEY;
struct EFI_SIMPLE_TEXT_OUTPUT_MODE;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_FILE_PROTOCOL;
struct EFI_LOADED_IMAGE_PROTOCOL;
struct EFI_PIXEL_BITMASK;
struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
struct EFI_GOP_MODE;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_MEMORY_DESCRIPTOR;
struct EFI_TABLE_HEADER;
struct EFI_CONFIGURATION_TABLE;
struct EFI_RUNTIME_SERVICES;
struct EFI_BOOT_SERVICES;
struct EFI_SYSTEM_TABLE;
struct EFI_BLOCK_IO_MEDIA;
struct EFI_BLOCK_IO_PROTOCOL;
struct EFI_TIME;

typedef struct EFI_DEVICE_PATH_PROTOCOL          EFI_DEVICE_PATH_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL    EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct EFI_INPUT_KEY                     EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_MODE       EFI_SIMPLE_TEXT_OUTPUT_MODE;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL   EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL                 EFI_FILE_PROTOCOL;
typedef struct EFI_LOADED_IMAGE_PROTOCOL         EFI_LOADED_IMAGE_PROTOCOL;
typedef struct EFI_PIXEL_BITMASK                 EFI_PIXEL_BITMASK;
typedef struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct EFI_GOP_MODE                      EFI_GOP_MODE;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL      EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct EFI_MEMORY_DESCRIPTOR             EFI_MEMORY_DESCRIPTOR;
typedef struct EFI_TABLE_HEADER                  EFI_TABLE_HEADER;
typedef struct EFI_CONFIGURATION_TABLE           EFI_CONFIGURATION_TABLE;
typedef struct EFI_RUNTIME_SERVICES              EFI_RUNTIME_SERVICES;
typedef struct EFI_BOOT_SERVICES                 EFI_BOOT_SERVICES;
typedef struct EFI_SYSTEM_TABLE                  EFI_SYSTEM_TABLE;
typedef struct EFI_BLOCK_IO_MEDIA                EFI_BLOCK_IO_MEDIA;
typedef struct EFI_BLOCK_IO_PROTOCOL             EFI_BLOCK_IO_PROTOCOL;
typedef struct EFI_TIME                          EFI_TIME;

/* ------------------------------------------------------------------ */
/* Базовые типы и константы                                            */
/* ------------------------------------------------------------------ */

typedef u64  EFI_STATUS;
typedef u64  UINTN;
typedef s64  INTN;
typedef u64  EFI_TPL;
typedef u64  EFI_PHYSICAL_ADDRESS;
typedef void *EFI_EVENT;
typedef void *EFI_HANDLE;
typedef u16  CHAR16;

struct EFI_GUID {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
};

#define EFI_SUCCESS              ((EFI_STATUS)0u)
#define EFIERR(n)                ((EFI_STATUS)(0x8000000000000000ULL | (u64)(n)))
#define EFI_LOAD_ERROR           EFIERR(1)
#define EFI_INVALID_PARAMETER    EFIERR(2)
#define EFI_UNSUPPORTED          EFIERR(3)
#define EFI_BAD_BUFFER_SIZE      EFIERR(4)
#define EFI_BUFFER_TOO_SMALL     EFIERR(5)
#define EFI_NOT_READY            EFIERR(6)
#define EFI_DEVICE_ERROR         EFIERR(7)
#define EFI_WRITE_PROTECTED      EFIERR(8)
#define EFI_OUT_OF_RESOURCES     EFIERR(9)
#define EFI_VOLUME_CORRUPTED     EFIERR(10)
#define EFI_NOT_FOUND            EFIERR(14)

static inline int EFI_ERROR(EFI_STATUS s)
{
    return (s & 0x8000000000000000ULL) != 0u;
}

#define EFI_PAGE_SIZE   0x1000ull
#define EFI_PAGE_MASK   0xFFFull
#define EFI_SIZE_TO_PAGES(n)  (((n) + EFI_PAGE_MASK) >> 12)

/* EFI_ALLOCATE_TYPE */
#define EFI_ALLOCATE_ANY_PAGES     0u
#define EFI_ALLOCATE_MAX_ADDRESS   1u
#define EFI_ALLOCATE_ADDRESS       2u

/* EFI_MEMORY_TYPE — значения совпадают с FBPI_EFI_* в fobos/boot_info.h */
#define EFI_RESERVED_MEMORY_TYPE        0u
#define EFI_LOADER_CODE                 1u
#define EFI_LOADER_DATA                 2u
#define EFI_BOOT_SERVICES_CODE          3u
#define EFI_BOOT_SERVICES_DATA          4u
#define EFI_RUNTIME_SERVICES_CODE       5u
#define EFI_RUNTIME_SERVICES_DATA       6u
#define EFI_CONVENTIONAL_MEMORY         7u
#define EFI_UNUSABLE_MEMORY             8u
#define EFI_ACPI_RECLAIM_MEMORY         9u
#define EFI_ACPI_MEMORY_NVS            10u
#define EFI_MEMORY_MAPPED_IO           11u
#define EFI_MEMORY_MAPPED_IO_PORT      12u
#define EFI_PAL_CODE                   13u

struct EFI_MEMORY_DESCRIPTOR {
    u32 type;
    u32 pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    EFI_PHYSICAL_ADDRESS virtual_start;
    u64 number_of_pages;
    u64 attribute;
};

/* EFI_RESET_TYPE */
#define EFI_RESET_COLD      0u
#define EFI_RESET_WARM      1u
#define EFI_RESET_SHUTDOWN  2u

/* ------------------------------------------------------------------ */
/* Сигнатуры таблиц и размеры                                          */
/* ------------------------------------------------------------------ */

#define EFI_SYSTEM_TABLE_SIGNATURE      0x5453595320494249ULL /* "IBI SYST" */
#define EFI_BOOT_SERVICES_SIGNATURE     0x56524553544f4f42ULL /* "BOOTSERV" */
#define EFI_RUNTIME_SERVICES_SIGNATURE  0x56524553544e5552ULL /* "RUNTSERV" */

struct EFI_TABLE_HEADER {
    u64 signature;
    u32 revision;
    u32 header_size;   /* размер всей таблицы в байтах (заполняется прошивкой) */
    u32 crc32;
    u32 reserved;
};

_Static_assert(sizeof(struct EFI_TABLE_HEADER) == 24, "EFI_TABLE_HEADER ABI");

/* ------------------------------------------------------------------ */
/* Протоколы                                                           */
/* ------------------------------------------------------------------ */

/* ВАЖНО: в отличие от системных таблиц (SystemTable/BootServices/RuntimeServices),
 * протоколы UEFI НЕ содержат EFI_TABLE_HEADER — раскладка задана спецификацией
 * и проверяется static_assert'ами в конце файла. */

/* EFI_VARIABLE_* атрибуты (UEFI 2.10, GetVariable/SetVariable). */
#define EFI_VARIABLE_NON_VOLATILE      0x00000001u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002u
#define EFI_VARIABLE_RUNTIME_ACCESS    0x00000004u

/* EFI_SCAN_* (UEFI 2.10, EFI_INPUT_KEY.ScanCode; только нужные). */
#define EFI_SCAN_NULL   0x0000u
#define EFI_SCAN_UP     0x0001u
#define EFI_SCAN_DOWN   0x0002u
#define EFI_SCAN_ESC    0x0017u

/* EFI_KEY_STATE / SimpleTextInputEx — определяются после EFI_INPUT_KEY ниже. */
struct EFI_KEY_STATE;
struct EFI_KEY_DATA;
struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *this, int extended_verification);
    EFI_STATUS (*read_key_stroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *this,
                                  struct EFI_INPUT_KEY *key);
};

struct EFI_INPUT_KEY {
    u16    scan_code;
    CHAR16 unicode_char;
};

_Static_assert(sizeof(struct EFI_INPUT_KEY) == 4, "EFI_INPUT_KEY ABI");

/* SimpleTextInputEx (UEFI 2.10, раздел 12.5): состояния модификаторов. */
#define EFI_SHIFT_VALID              0x00000001u
#define EFI_LEFT_SHIFT_PRESSED       0x00000002u
#define EFI_RIGHT_SHIFT_PRESSED      0x00000004u
#define EFI_LEFT_CONTROL_PRESSED     0x00000010u
#define EFI_RIGHT_CONTROL_PRESSED    0x00000020u
#define EFI_LEFT_ALT_PRESSED         0x00000040u
#define EFI_RIGHT_ALT_PRESSED        0x00000080u

struct EFI_KEY_STATE {
    u32 key_shift_state;
    u8  key_toggle_state;
};

struct EFI_KEY_DATA {
    struct EFI_INPUT_KEY key;
    struct EFI_KEY_STATE key_state;
};

struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (*reset)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *this, int extended_verification);
    EFI_STATUS (*read_key_stroke_ex)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *this,
                                     struct EFI_KEY_DATA *key_data);
    void *wait_for_key_ex; /* EFI_EVENT */
    EFI_STATUS (*set_state)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *this, u8 *key_toggle_state);
    EFI_STATUS (*register_key_notify)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *this,
                                      struct EFI_KEY_DATA *key_data,
                                      void (*notify)(struct EFI_KEY_DATA *key_data, void *ctx),
                                      void **notify_handle);
    EFI_STATUS (*unregister_key_notify)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *this, void *notify_handle);
};

struct EFI_SIMPLE_TEXT_OUTPUT_MODE {
    s32 max_mode;
    s32 mode;
    s32 attribute;
    s32 cursor_column;
    s32 cursor_row;
    s32 cursor_visible;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, int extended_verification);
    EFI_STATUS (*output_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, CHAR16 *string);
    EFI_STATUS (*test_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, CHAR16 *string);
    EFI_STATUS (*query_mode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, u64 mode_number,
                             u64 *columns, u64 *rows);
    EFI_STATUS (*set_mode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, u64 mode_number);
    EFI_STATUS (*enable_cursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, int enable);
    struct EFI_SIMPLE_TEXT_OUTPUT_MODE *mode;
    EFI_STATUS (*clear_screen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this);
    EFI_STATUS (*set_attribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, u64 attribute);
    EFI_STATUS (*set_cursor_position)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this,
                                      u64 column, u64 row);
};

/* EFI_FILE_MODE_* (UEFI 2.10, EFI_FILE_PROTOCOL.Open). */
#define EFI_FILE_MODE_READ     0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE    0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE   0x8000000000000000ULL

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 revision;
    EFI_STATUS (*open_volume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *this,
                              struct EFI_FILE_PROTOCOL **root);
};

struct EFI_FILE_PROTOCOL {
    u64 revision;
    EFI_STATUS (*open)(struct EFI_FILE_PROTOCOL *this, struct EFI_FILE_PROTOCOL **new_handle,
                       CHAR16 *file_name, u64 open_mode, u64 attributes);
    EFI_STATUS (*close)(struct EFI_FILE_PROTOCOL *this);
    EFI_STATUS (*del)(struct EFI_FILE_PROTOCOL *this);
    EFI_STATUS (*read)(struct EFI_FILE_PROTOCOL *this, u64 *buffer_size, void *buffer);
    EFI_STATUS (*write)(struct EFI_FILE_PROTOCOL *this, u64 *buffer_size, const void *buffer);
    EFI_STATUS (*get_position)(struct EFI_FILE_PROTOCOL *this, u64 *position);
    EFI_STATUS (*set_position)(struct EFI_FILE_PROTOCOL *this, u64 position);
    EFI_STATUS (*get_info)(struct EFI_FILE_PROTOCOL *this, const struct EFI_GUID *info_type,
                           u64 *buffer_size, void *buffer);
    EFI_STATUS (*set_info)(struct EFI_FILE_PROTOCOL *this, const struct EFI_GUID *info_type,
                           u64 buffer_size, const void *buffer);
    EFI_STATUS (*flush)(struct EFI_FILE_PROTOCOL *this);
};

struct EFI_LOADED_IMAGE_PROTOCOL {
    u32 revision;
    EFI_HANDLE parent_handle;
    struct EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    struct EFI_DEVICE_PATH_PROTOCOL *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    u64 image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    EFI_STATUS (*unload)(EFI_HANDLE image_handle);
};

struct EFI_PIXEL_BITMASK {
    u32 red_mask;
    u32 green_mask;
    u32 blue_mask;
    u32 reserved_mask;
};

/* EFI_GRAPHICS_OUTPUT_PIXEL_FORMAT */
#define EFI_GOP_PIXEL_RGBX      0u
#define EFI_GOP_PIXEL_BGRX      1u
#define EFI_GOP_PIXEL_BITMASK   2u
#define EFI_GOP_PIXEL_BLTONLY   3u

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;
    struct EFI_PIXEL_BITMASK pixel_information;
    u32 pixels_per_scanline;
};

struct EFI_GOP_MODE {
    u32 max_mode;
    u32 mode;
    struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    u64 size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    u64 frame_buffer_size;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*query_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *this, u32 mode_number,
                             u64 *size_of_info, struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
    EFI_STATUS (*set_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *this, u32 mode_number);
    EFI_STATUS (*blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *this, void *buffer,
                      u32 operation, u64 source_x, u64 source_y, u64 dest_x, u64 dest_y,
                      u64 width, u64 height, u64 delta);
    struct EFI_GOP_MODE *mode;
};

/* ------------------------------------------------------------------ */
/* Block I/O и Device Path (установщик, FND-3b)                        */
/* ------------------------------------------------------------------ */

/* EFI_BLOCK_IO_MEDIA (UEFI 2.10, раздел 13.4). Поля Revision в Media НЕТ:
 * Revision (u64) — первое поле самого протокола, а Media — ВТОРОЕ поле
 * протокола (смещение 8), перед указателями на функции. LastBlock —
 * EFI_LBA (UINT64), поэтому ёмкость диска > 2 ТиБ не усекается.
 * Прошивка может вернуть объект большего размера (Rev 2/3) — читаемы
 * первые 32 байта, этого достаточно. */
struct EFI_BLOCK_IO_MEDIA {
    u32 media_id;            /* смещение 0  */
    u8  removable_media;     /* 4 */
    u8  media_present;       /* 5 */
    u8  logical_partition;   /* 6; 1 = раздел (дочерний handle) */
    u8  read_only;           /* 7 */
    u8  write_caching;       /* 8 */
    u8  pad0[3];             /* 9..11 */
    u32 block_size;          /* 12 */
    u32 io_align;            /* 16 */
    u32 pad1;                /* 20 */
    u64 last_block;          /* 24; ёмкость = (last_block+1)*block_size */
};

struct EFI_BLOCK_IO_PROTOCOL {
    u64 revision;                        /* 0  */
    struct EFI_BLOCK_IO_MEDIA *media;    /* 8  — ВТОРОЕ поле, до функций */
    EFI_STATUS (*reset)(struct EFI_BLOCK_IO_PROTOCOL *this, int extended_verification);          /* 16 */
    EFI_STATUS (*read_blocks)(struct EFI_BLOCK_IO_PROTOCOL *this, u32 media_id,
                              u64 lba, u64 buffer_size, void *buffer);                           /* 24 */
    EFI_STATUS (*write_blocks)(struct EFI_BLOCK_IO_PROTOCOL *this, u32 media_id,
                               u64 lba, u64 buffer_size, const void *buffer);                    /* 32 */
    EFI_STATUS (*flush_blocks)(struct EFI_BLOCK_IO_PROTOCOL *this);                              /* 40 */
};

/* Заголовок узла Device Path (UEFI 2.10, раздел 10). Достаточно для
 * обхода и сравнения путей (поиск родительского диска раздела). */
struct EFI_DEVICE_PATH_PROTOCOL {
    u8 type;
    u8 subtype;
    u16 length;
};

#define EFI_DP_TYPE_END          0x7Fu
#define EFI_DP_SUBTYPE_END_ALL   0xFFu

/* EFI_TIME (UEFI 2.10, Runtime Services GetTime) — время установленных
 * файлов через общий FAT32-драйвер в установщике. */
struct EFI_TIME {
    u16 year;
    u8  month;
    u8  day;
    u8  hour;
    u8  minute;
    u8  second;
    u8  pad1;
    u32 nanosecond;
    s16 time_zone;
    u8  daylight;
    u8  pad2;
};

/* ------------------------------------------------------------------ */
/* EFI Boot Services Table (UEFI 2.10, полный порядок членов)          */
/* ------------------------------------------------------------------ */

struct EFI_BOOT_SERVICES {
    struct EFI_TABLE_HEADER hdr;
    EFI_TPL (*raise_tpl)(EFI_TPL new_tpl);
    void (*restore_tpl)(EFI_TPL old_tpl);
    EFI_STATUS (*allocate_pages)(u32 type, u32 memory_type, u64 pages, EFI_PHYSICAL_ADDRESS *memory);
    EFI_STATUS (*free_pages)(EFI_PHYSICAL_ADDRESS memory, u64 pages);
    EFI_STATUS (*get_memory_map)(u64 *memory_map_size, struct EFI_MEMORY_DESCRIPTOR *memory_map,
                                 u64 *map_key, u64 *descriptor_size, u32 *descriptor_version);
    EFI_STATUS (*allocate_pool)(u32 pool_type, u64 size, void **buffer);
    EFI_STATUS (*free_pool)(void *buffer);
    EFI_STATUS (*create_event)(u32 type, EFI_TPL notify_tpl, void (*notify_function)(void *event, void *context),
                               void *notify_context, EFI_EVENT *event);
    EFI_STATUS (*set_timer)(EFI_EVENT event, u32 type, u64 trigger_time);
    EFI_STATUS (*wait_for_event)(u64 number_of_events, EFI_EVENT *event, u64 *index);
    EFI_STATUS (*signal_event)(EFI_EVENT event);
    EFI_STATUS (*close_event)(EFI_EVENT event);
    EFI_STATUS (*check_event)(EFI_EVENT event);
    EFI_STATUS (*install_protocol_interface)(EFI_HANDLE *handle, const struct EFI_GUID *protocol,
                                             u32 interface_type, void *interface);
    EFI_STATUS (*reinstall_protocol_interface)(EFI_HANDLE handle, const struct EFI_GUID *protocol,
                                               void *old_interface, void *new_interface);
    EFI_STATUS (*uninstall_protocol_interface)(EFI_HANDLE handle, const struct EFI_GUID *protocol,
                                               void *interface);
    EFI_STATUS (*handle_protocol)(EFI_HANDLE handle, const struct EFI_GUID *protocol, void **interface);
    EFI_STATUS (*reserved)(void);
    EFI_STATUS (*register_protocol_notify)(const struct EFI_GUID *protocol, EFI_EVENT event,
                                           void **registration);
    EFI_STATUS (*locate_handle)(u32 search_type, const struct EFI_GUID *protocol, void *search_key,
                                u64 *buffer_size, EFI_HANDLE *buffer);
    EFI_STATUS (*locate_device_path)(const struct EFI_GUID *protocol,
                                     struct EFI_DEVICE_PATH_PROTOCOL **device_path, EFI_HANDLE *device);
    EFI_STATUS (*install_configuration_table)(const struct EFI_GUID *guid, void *table);
    EFI_STATUS (*load_image)(int boot_policy, EFI_HANDLE parent_image_handle,
                             struct EFI_DEVICE_PATH_PROTOCOL *device_path, void *source_buffer,
                             u64 source_size, EFI_HANDLE *image_handle);
    EFI_STATUS (*start_image)(EFI_HANDLE image_handle, u64 *exit_data_size, CHAR16 **exit_data);
    EFI_STATUS (*exit)(EFI_HANDLE image_handle, EFI_STATUS exit_status,
                       u64 exit_data_size, CHAR16 *exit_data);
    EFI_STATUS (*unload_image)(EFI_HANDLE image_handle);
    EFI_STATUS (*exit_boot_services)(EFI_HANDLE image_handle, u64 map_key);
    EFI_STATUS (*get_next_monotonic_count)(u64 *count);
    EFI_STATUS (*stall)(u64 microseconds);
    EFI_STATUS (*set_watchdog_timer)(u64 timeout, u64 watchdog_code, u64 data_size, const CHAR16 *watchdog_data);
    EFI_STATUS (*connect_controller)(EFI_HANDLE controller_handle, EFI_HANDLE *driver_image_handle,
                                     struct EFI_DEVICE_PATH_PROTOCOL *remaining_device_path, int recursive);
    EFI_STATUS (*disconnect_controller)(EFI_HANDLE controller_handle, EFI_HANDLE driver_image_handle,
                                        EFI_HANDLE child_handle);
    EFI_STATUS (*open_protocol)(EFI_HANDLE handle, const struct EFI_GUID *protocol, void **interface,
                                EFI_HANDLE agent_handle, EFI_HANDLE controller_handle, u32 attributes);
    EFI_STATUS (*close_protocol)(EFI_HANDLE handle, const struct EFI_GUID *protocol,
                                 EFI_HANDLE agent_handle, EFI_HANDLE controller_handle);
    EFI_STATUS (*open_protocol_information)(EFI_HANDLE handle, const struct EFI_GUID *protocol,
                                            void **entry_buffer, u64 *entry_count);
    EFI_STATUS (*protocols_per_handle)(EFI_HANDLE handle, struct EFI_GUID ***protocol_buffer,
                                       u64 *protocol_buffer_count);
    EFI_STATUS (*locate_handle_buffer)(u32 search_type, const struct EFI_GUID *protocol, void *search_key,
                                       u64 *no_handles, EFI_HANDLE **buffer);
    EFI_STATUS (*locate_protocol)(const struct EFI_GUID *protocol, void *registration, void **interface);
    EFI_STATUS (*install_multiple_protocol_interfaces)(EFI_HANDLE *handle, ...);
    EFI_STATUS (*uninstall_multiple_protocol_interfaces)(EFI_HANDLE handle, ...);
    EFI_STATUS (*calculate_crc32)(void *data, u64 data_size, u32 *crc32);
    void (*copy_mem)(void *destination, const void *source, u64 length);
    void (*set_mem)(void *buffer, u64 size, u8 value);
    EFI_STATUS (*create_event_ex)(u32 type, EFI_TPL notify_tpl,
                                  void (*notify_function)(void *event, void *context),
                                  const struct EFI_GUID *event_group, void *notify_context,
                                  EFI_EVENT *event);
};

/* ------------------------------------------------------------------ */
/* EFI Runtime Services Table                                          */
/* ------------------------------------------------------------------ */

struct EFI_RUNTIME_SERVICES {
    struct EFI_TABLE_HEADER hdr;
    EFI_STATUS (*get_time)(void *time, void *capabilities);
    EFI_STATUS (*set_time)(void *time);
    EFI_STATUS (*get_wakeup_time)(int *enabled, int *pending, void *time);
    EFI_STATUS (*set_wakeup_time)(int enable, void *time);
    EFI_STATUS (*set_virtual_address_map)(u64 descriptor_size, u32 descriptor_version,
                                          u32 descriptor_version_pad,
                                          struct EFI_MEMORY_DESCRIPTOR *virtual_map);
    EFI_STATUS (*convert_pointer)(u64 debug_disposition, void **address);
    EFI_STATUS (*get_variable)(CHAR16 *variable_name, const struct EFI_GUID *vendor_guid,
                               u32 *attributes, u64 *data_size, void *data);
    EFI_STATUS (*get_next_variable_name)(u64 *variable_name_size, CHAR16 *variable_name,
                                         struct EFI_GUID *vendor_guid);
    EFI_STATUS (*set_variable)(CHAR16 *variable_name, const struct EFI_GUID *vendor_guid,
                               u32 attributes, u64 data_size, const void *data);
    EFI_STATUS (*get_next_high_monotonic_count)(u32 *high_count);
    void (*reset_system)(u32 reset_type, EFI_STATUS reset_status, u64 data_size, const CHAR16 *reset_data);
    EFI_STATUS (*update_capsule)(void **capsule_header_array, u64 capsule_count,
                                 EFI_PHYSICAL_ADDRESS scatter_gather_list);
    EFI_STATUS (*query_capsule_capabilities)(void **capsule_header_array, u64 capsule_count,
                                             u64 *maximum_capsule_size, u32 *reset_type);
    EFI_STATUS (*query_variable_info)(u32 attributes, u64 *maximum_variable_storage_size,
                                      u64 *remaining_variable_storage_size,
                                      u64 *maximum_variable_size);
};

/* ------------------------------------------------------------------ */
/* EFI System Table                                                    */
/* ------------------------------------------------------------------ */

struct EFI_CONFIGURATION_TABLE {
    struct EFI_GUID vendor_guid;
    void *vendor_table;
};

struct EFI_SYSTEM_TABLE {
    struct EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    u32 firmware_revision;
    EFI_HANDLE console_in_handle;
    struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *con_in;
    EFI_HANDLE console_out_handle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE standard_error_handle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *std_err;
    struct EFI_RUNTIME_SERVICES *runtime_services;
    struct EFI_BOOT_SERVICES *boot_services;
    u64 number_of_table_entries;
    struct EFI_CONFIGURATION_TABLE *configuration_table;
};

/* ------------------------------------------------------------------ */
/* Защита раскладки: смещения соответствуют UEFI 2.10                  */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(struct EFI_MEMORY_DESCRIPTOR) == 40, "EFI_MEMORY_DESCRIPTOR ABI");
_Static_assert(offsetof(struct EFI_SYSTEM_TABLE, runtime_services) == 88, "EFI_SYSTEM_TABLE ABI");
_Static_assert(offsetof(struct EFI_SYSTEM_TABLE, boot_services) == 96, "EFI_SYSTEM_TABLE ABI");
_Static_assert(offsetof(struct EFI_SYSTEM_TABLE, configuration_table) == 112, "EFI_SYSTEM_TABLE ABI");

_Static_assert(offsetof(struct EFI_BOOT_SERVICES, allocate_pages) == 40, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, get_memory_map) == 56, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, handle_protocol) == 152, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, exit_boot_services) == 232, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, stall) == 248, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, set_watchdog_timer) == 256, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, locate_protocol) == 320, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, copy_mem) == 352, "EFI_BOOT_SERVICES ABI");
_Static_assert(offsetof(struct EFI_BOOT_SERVICES, set_mem) == 360, "EFI_BOOT_SERVICES ABI");

_Static_assert(offsetof(struct EFI_RUNTIME_SERVICES, set_virtual_address_map) == 56, "EFI_RUNTIME_SERVICES ABI");
_Static_assert(offsetof(struct EFI_RUNTIME_SERVICES, get_variable) == 72, "EFI_RUNTIME_SERVICES ABI");
_Static_assert(offsetof(struct EFI_RUNTIME_SERVICES, set_variable) == 88, "EFI_RUNTIME_SERVICES ABI");
_Static_assert(offsetof(struct EFI_SYSTEM_TABLE, con_in) == 48, "EFI_SYSTEM_TABLE ABI");
_Static_assert(offsetof(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL, read_key_stroke) == 8, "SIMPLE_TEXT_INPUT ABI");
_Static_assert(offsetof(struct EFI_RUNTIME_SERVICES, reset_system) == 104, "EFI_RUNTIME_SERVICES ABI");

_Static_assert(offsetof(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, output_string) == 8, "SIMPLE_TEXT_OUTPUT ABI");
_Static_assert(offsetof(struct EFI_FILE_PROTOCOL, open) == 8, "EFI_FILE_PROTOCOL ABI");
_Static_assert(offsetof(struct EFI_FILE_PROTOCOL, read) == 32, "EFI_FILE_PROTOCOL ABI");
_Static_assert(offsetof(struct EFI_FILE_PROTOCOL, set_position) == 56, "EFI_FILE_PROTOCOL ABI");
_Static_assert(offsetof(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, open_volume) == 8, "SIMPLE_FILE_SYSTEM ABI");
_Static_assert(offsetof(struct EFI_LOADED_IMAGE_PROTOCOL, device_handle) == 24, "LOADED_IMAGE ABI");
_Static_assert(offsetof(struct EFI_LOADED_IMAGE_PROTOCOL, image_base) == 64, "LOADED_IMAGE ABI");
_Static_assert(offsetof(struct EFI_GRAPHICS_OUTPUT_PROTOCOL, mode) == 24, "GOP ABI");

_Static_assert(offsetof(struct EFI_BLOCK_IO_MEDIA, media_id) == 0, "BLOCK_IO_MEDIA ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_MEDIA, media_present) == 5, "BLOCK_IO_MEDIA ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_MEDIA, block_size) == 12, "BLOCK_IO_MEDIA ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_MEDIA, io_align) == 16, "BLOCK_IO_MEDIA ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_MEDIA, last_block) == 24, "BLOCK_IO_MEDIA ABI");
_Static_assert(sizeof(struct EFI_BLOCK_IO_MEDIA) == 32, "BLOCK_IO_MEDIA ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_PROTOCOL, media) == 8, "BLOCK_IO ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_PROTOCOL, reset) == 16, "BLOCK_IO ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_PROTOCOL, read_blocks) == 24, "BLOCK_IO ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_PROTOCOL, write_blocks) == 32, "BLOCK_IO ABI");
_Static_assert(offsetof(struct EFI_BLOCK_IO_PROTOCOL, flush_blocks) == 40, "BLOCK_IO ABI");
_Static_assert(sizeof(struct EFI_DEVICE_PATH_PROTOCOL) == 4, "DEVICE_PATH ABI");
_Static_assert(sizeof(struct EFI_TIME) == 16, "EFI_TIME ABI");

/* GUID протоколов (определены в boot/uefi/efi_guids.c). */
extern const struct EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_BLOCK_IO_PROTOCOL_GUID;
extern const struct EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID;

#endif /* FOBOS_UEFI_EFI_H */
