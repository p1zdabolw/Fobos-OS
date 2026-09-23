#include "i18n.h"
#include "../lib/string.h"

static int g_lang = LANG_EN;

static const char *g_en[STR_COUNT] = {
    [STR_EMPTY]                = "",

    [STR_START_TITLE]          = "Applications",
    [STR_APP_TERMINAL]         = "Terminal",
    [STR_APP_BROWSER]          = "Browser",
    [STR_APP_NOTEPAD]          = "Notepad",
    [STR_APP_FILES]            = "Files",
    [STR_APP_PHOTOS]           = "Photos",
    [STR_APP_MEDIA]            = "Media Player",
    [STR_APP_WALLPAPER]        = "Wallpaper",
    [STR_APP_SETTINGS]         = "Settings",
    [STR_APP_ABOUT]            = "About FOS",

    [STR_CTX_NEW_TERMINAL]     = "New Terminal",
    [STR_CTX_NEW_BROWSER]      = "New Browser",
    [STR_CTX_NEW_NOTEPAD]      = "New Notepad",
    [STR_CTX_NEW_FILES]        = "New Files",
    [STR_CTX_PHOTOS]           = "Photos",
    [STR_CTX_MEDIA]            = "Media Player",
    [STR_CTX_WALLPAPER]        = "Change Wallpaper",
    [STR_CTX_SETTINGS]         = "Settings",

    [STR_SETTINGS_DISPLAY]     = "Display Resolution",
    [STR_SETTINGS_CURRENT]     = "Current: %ux%u 32bpp",
    [STR_SETTINGS_LANG]        = "Keyboard Language",
    [STR_SETTINGS_LANG_HINT]   = "Toggle: press Shift + Alt together",
    [STR_SETTINGS_UI_LANG]     = "Interface Language",
    [STR_SETTINGS_UI_HINT]     = "Applies immediately to menus, buttons and windows",
    [STR_SETTINGS_KB_TEST]     = "Keyboard test -- click the field and type:",
    [STR_SETTINGS_CLEAR]       = "Clear",
    [STR_SETTINGS_LASTKEY]     = "Last key byte: 0x%02x   total: %d",
    [STR_SETTINGS_WALLPAPER]   = "Wallpaper",
    [STR_SETTINGS_OPEN_WP]     = "Open Wallpaper (%s)",
    [STR_SETTINGS_CLOCK]       = "Clock Format",
    [STR_SETTINGS_12H]         = "12-hour",
    [STR_SETTINGS_24H]         = "24-hour",
    [STR_SETTINGS_MOUSE]       = "Mouse Speed",
    [STR_SETTINGS_SLOW]        = "Slow",
    [STR_SETTINGS_NORMAL]      = "Normal",
    [STR_SETTINGS_FAST]        = "Fast",
    [STR_SETTINGS_SYSINFO]     = "System Information",
    [STR_SETTINGS_MEMORY]      = "Memory: %u KB free / %u KB total",
    [STR_SETTINGS_UPTIME]      = "Uptime: %u:%02u:%02u",
    [STR_SETTINGS_NETWORK]     = "Network: %u.%u.%u.%u",
    [STR_SETTINGS_NO_NET]      = "Network: no device",
    [STR_SETTINGS_KERNEL]      = "Kernel: FOS 0.3 x86_64",
    [STR_SETTINGS_RES_FAIL]    = "Resolution not supported by VBE",

    [STR_WP_TITLE]             = "Wallpaper",
    [STR_WP_SOLID]             = "Solid",
    [STR_WP_GRAD_V]            = "Gradient V",
    [STR_WP_GRAD_H]            = "Gradient H",
    [STR_WP_CHECKER]           = "Checker",
    [STR_WP_PLASMA]            = "Plasma",
    [STR_WP_STARS]             = "Stars",
    [STR_WP_DIAGONAL]          = "Diagonal",
    [STR_WP_PHOTO]             = "Photo",
    [STR_WP_CURRENT]           = "Current: %s",

    [STR_NP_NAME]              = "Name:",
    [STR_NP_SAVE]              = "Save",
    [STR_NP_SAVED]             = "Saved",
    [STR_NP_UNTITLED]          = "untitled.txt",

    [STR_FILES_NAME]           = "Name",
    [STR_FILES_COUNT]          = "%d files",

    [STR_PHOTOS_PREV]          = "< Prev",
    [STR_PHOTOS_NEXT]          = "Next >",
    [STR_PHOTOS_SET]           = "Set Wallpaper",
    [STR_PHOTOS_SLIDE]         = "Slideshow",
    [STR_PHOTOS_STOP]          = "Stop",
    [STR_PHOTO_SKY]            = "Sky",
    [STR_PHOTO_SUNSET]         = "Sunset",
    [STR_PHOTO_PLASMA]         = "Plasma",
    [STR_PHOTO_CHESS]          = "Checkerboard",
    [STR_PHOTO_MANDELBROT]     = "Mandelbrot",
    [STR_PHOTO_STARS]          = "Stars",
    [STR_PHOTO_RAINBOW]        = "Rainbow",
    [STR_PHOTO_WAVES]          = "Waves",

    [STR_MEDIA_NOW]            = "Now Playing",
    [STR_MEDIA_PLAY]           = "Play",
    [STR_MEDIA_PAUSE]          = "Pause",
    [STR_MEDIA_PREV]           = "< Prev",
    [STR_MEDIA_NEXT]           = "Next >",
    [STR_MEDIA_PLAYING]        = "Playing",
    [STR_MEDIA_STOPPED]        = "Stopped",
    [STR_TRACK_SCALE]          = "Ascending Scale",
    [STR_TRACK_TWINKLE]        = "Twinkle",
    [STR_TRACK_ODE]            = "Ode to Joy",
    [STR_TRACK_FUR]            = "Fur Elise",
    [STR_TRACK_BEEP]           = "Modem Beep",

    [STR_BR_FILE]              = "File",
    [STR_BR_EDIT]              = "Edit",
    [STR_BR_VIEW]              = "View",
    [STR_BR_HISTORY]           = "History",
    [STR_BR_HELP]              = "Help",
    [STR_BR_HTTPS_NO]          = "(HTTPS: no)",
    [STR_BR_DONE]              = "Done",
    [STR_BR_FETCHING]          = "Fetching...",
    [STR_BR_COOKIES]           = "Cookies: %d",
    [STR_BR_BACK]              = "<",
    [STR_BR_FWD]               = ">",
    [STR_BR_REF]               = "R",
    [STR_BR_HOME]              = "H",
    [STR_BR_GO]                = "->",

    [STR_TERM_HELP] =
        "FOS Terminal\n"
        "Files:     ls cat touch write rm cp mv head tail\n"
        "           wc grep file stat tree df\n"
        "Shell:     echo clear pwd cd which history\n"
        "System:    ver uname mem free uptime date hostname\n"
        "           whoami sleep\n"
        "Network:   ifconfig ping nslookup wget curl\n"
        "Language:  lang [en|ru]\n"
        "Scripting: run FILE, bash FILE",
};

static const char *g_ru[STR_COUNT] = {
    [STR_EMPTY]                = "",

    [STR_START_TITLE]          = "\xCF\xF0\xE8\xEB\xEE\xE6\xE5\xED\xE8\xFF",
    [STR_APP_TERMINAL]         = "\xD2\xE5\xF0\xEC\xE8\xED\xE0\xEB",
    [STR_APP_BROWSER]          = "\xC1\xF0\xE0\xF3\xE7\xE5\xF0",
    [STR_APP_NOTEPAD]          = "\xC1\xEB\xEE\xEA\xED\xEE\xF2",
    [STR_APP_FILES]            = "\xD4\xE0\xE9\xEB\xFB",
    [STR_APP_PHOTOS]           = "\xD4\xEE\xF2\xEE\xE3\xF0\xE0\xF4\xE8\xE8",
    [STR_APP_MEDIA]            = "\xCC\xE5\xE4\xE8\xE0\xEF\xEB\xE5\xE5\xF0",
    [STR_APP_WALLPAPER]        = "\xCE\xE1\xEE\xE8",
    [STR_APP_SETTINGS]         = "\xCD\xE0\xF1\xF2\xF0\xEE\xE9\xEA\xE8",
    [STR_APP_ABOUT]            = "\xCE \xF1\xE8\xF1\xF2\xE5\xEC\xE5 FOS",

    [STR_CTX_NEW_TERMINAL]     = "\xCD\xEE\xE2\xFB\xE9 \xF2\xE5\xF0\xEC\xE8\xED\xE0\xEB",
    [STR_CTX_NEW_BROWSER]      = "\xCD\xEE\xE2\xFB\xE9 \xE1\xF0\xE0\xF3\xE7\xE5\xF0",
    [STR_CTX_NEW_NOTEPAD]      = "\xCD\xEE\xE2\xFB\xE9 \xE1\xEB\xEE\xEA\xED\xEE\xF2",
    [STR_CTX_NEW_FILES]        = "\xCD\xEE\xE2\xFB\xE5 \xF4\xE0\xE9\xEB\xFB",
    [STR_CTX_PHOTOS]           = "\xD4\xEE\xF2\xEE\xE3\xF0\xE0\xF4\xE8\xE8",
    [STR_CTX_MEDIA]            = "\xCC\xE5\xE4\xE8\xE0\xEF\xEB\xE5\xE5\xF0",
    [STR_CTX_WALLPAPER]        = "\xD1\xEC\xE5\xED\xE8\xF2\xFC \xEE\xE1\xEE\xE8",
    [STR_CTX_SETTINGS]         = "\xCD\xE0\xF1\xF2\xF0\xEE\xE9\xEA\xE8",

    [STR_SETTINGS_DISPLAY]     = "\xD0\xE0\xE7\xF0\xE5\xF8\xE5\xED\xE8\xE5 \xFD\xEA\xF0\xE0\xED\xE0",
    [STR_SETTINGS_CURRENT]     = "\xD2\xE5\xEA\xF3\xF9\xE5\xE5: %ux%u 32bpp",
    [STR_SETTINGS_LANG]        = "\xFF\xE7\xFB\xEA \xEA\xEB\xE0\xE2\xE8\xE0\xF2\xF3\xF0\xFB",
    [STR_SETTINGS_LANG_HINT]   = "\xCF\xE5\xF0\xE5\xEA\xEB\xFE\xF7\xE5\xED\xE8\xE5: Shift + Alt",
    [STR_SETTINGS_UI_LANG]     = "\xFF\xE7\xFB\xEA \xE8\xED\xF2\xE5\xF0\xF4\xE5\xE9\xF1\xE0",
    [STR_SETTINGS_UI_HINT]     = "\xCF\xF0\xE8\xEC\xE5\xED\xFF\xE5\xF2\xF1\xFF \xF1\xF0\xE0\xE7\xF3 \xE6\xE5",
    [STR_SETTINGS_KB_TEST]     = "\xCF\xF0\xEE\xE2\xE5\xF0\xEA\xE0 \xEA\xEB\xE0\xE2\xE8\xE0\xF2\xF3\xF0\xFB:",
    [STR_SETTINGS_CLEAR]       = "\xCE\xF7\xE8\xF1\xF2\xE8\xF2\xFC",
    [STR_SETTINGS_LASTKEY]     = "\xCF\xEE\xF1\xEB\xE5\xE4\xED\xE8\xE9 \xE1\xE0\xE9\xF2: 0x%02x   \xE2\xF1\xE5\xE3\xEE: %d",
    [STR_SETTINGS_WALLPAPER]   = "\xCE\xE1\xEE\xE8",
    [STR_SETTINGS_OPEN_WP]     = "\xCE\xF2\xEA\xF0\xFB\xF2\xFC \xEE\xE1\xEE\xE8 (%s)",
    [STR_SETTINGS_CLOCK]       = "\xD4\xEE\xF0\xEC\xE0\xF2 \xF7\xE0\xF1\xEE\xE2",
    [STR_SETTINGS_12H]         = "12 \xF7\xE0\xF1\xEE\xE2",
    [STR_SETTINGS_24H]         = "24 \xF7\xE0\xF1\xE0",
    [STR_SETTINGS_MOUSE]       = "\xD1\xEA\xEE\xF0\xEE\xF1\xF2\xFC \xEC\xFB\xF8\xE8",
    [STR_SETTINGS_SLOW]        = "\xCC\xE5\xE4\xEB\xE5\xED\xED\xEE",
    [STR_SETTINGS_NORMAL]      = "\xCE\xE1\xFB\xF7\xED\xEE",
    [STR_SETTINGS_FAST]        = "\xC1\xFB\xF1\xF2\xF0\xEE",
    [STR_SETTINGS_SYSINFO]     = "\xD1\xE8\xF1\xF2\xE5\xEC\xED\xE0\xFF \xE8\xED\xF4\xEE\xF0\xEC\xE0\xF6\xE8\xFF",
    [STR_SETTINGS_MEMORY]      = "\xCF\xE0\xEC\xFF\xF2\xFC: %u \xCA\xC1 \xF1\xE2\xEE\xE1\xEE\xE4\xED\xEE / %u \xCA\xC1 \xE2\xF1\xE5\xE3\xEE",
    [STR_SETTINGS_UPTIME]      = "\xC2\xF0\xE5\xEC\xFF \xF0\xE0\xE1\xEE\xF2\xFB: %u:%02u:%02u",
    [STR_SETTINGS_NETWORK]     = "\xD1\xE5\xF2\xFC: %u.%u.%u.%u",
    [STR_SETTINGS_NO_NET]      = "\xD1\xE5\xF2\xFC: \xED\xE5\xF2 \xF3\xF1\xF2\xF0\xEE\xE9\xF1\xF2\xE2\xE0",
    [STR_SETTINGS_KERNEL]      = "\xFF\xE4\xF0\xEE: FOS 0.3 x86_64",
    [STR_SETTINGS_RES_FAIL]    = "\xD0\xE0\xE7\xF0\xE5\xF8\xE5\xED\xE8\xE5 \xED\xE5 \xEF\xEE\xE4\xE4\xE5\xF0\xE6\xE8\xE2\xE0\xE5\xF2\xF1\xFF VBE",

    [STR_WP_TITLE]             = "\xCE\xE1\xEE\xE8",
    [STR_WP_SOLID]             = "\xD1\xEF\xEB\xEE\xF8\xED\xEE\xE9",
    [STR_WP_GRAD_V]            = "\xC3\xF0\xE0\xE4\xE8\xE5\xED\xF2 \xC2",
    [STR_WP_GRAD_H]            = "\xC3\xF0\xE0\xE4\xE8\xE5\xED\xF2 \xC3",
    [STR_WP_CHECKER]           = "\xD8\xE0\xF5\xEC\xE0\xF2\xFB",
    [STR_WP_PLASMA]            = "\xCF\xEB\xE0\xE7\xEC\xE0",
    [STR_WP_STARS]             = "\xC7\xE2\xB8\xE7\xE4\xFB",
    [STR_WP_DIAGONAL]          = "\xC4\xE8\xE0\xE3\xEE\xED\xE0\xEB\xFC",
    [STR_WP_PHOTO]             = "\xD4\xEE\xF2\xEE",
    [STR_WP_CURRENT]           = "\xD2\xE5\xEA\xF3\xF9\xE5\xE5: %s",

    [STR_NP_NAME]              = "\xC8\xEC\xFF:",
    [STR_NP_SAVE]              = "\xD1\xEE\xF5\xF0\xE0\xED\xE8\xF2\xFC",
    [STR_NP_SAVED]             = "\xD1\xEE\xF5\xF0\xE0\xED\xE5\xED\xEE",
    [STR_NP_UNTITLED]          = "\xE1\xE5\xE7\xFB\xEC\xFF\xED\xED\xFB\xE9.txt",

    [STR_FILES_NAME]           = "\xC8\xEC\xFF",
    [STR_FILES_COUNT]          = "\xF4\xE0\xE9\xEB\xEE\xE2: %d",

    [STR_PHOTOS_PREV]          = "< \xCF\xF0\xE5\xE4",
    [STR_PHOTOS_NEXT]          = "\xD1\xEB\xE5\xE4 >",
    [STR_PHOTOS_SET]           = "\xCD\xE0 \xF0\xE0\xE1\xEE\xF7\xE8\xE9 \xF1\xF2\xEE\xEB",
    [STR_PHOTOS_SLIDE]         = "\xD1\xEB\xE0\xE9\xE4-\xF8\xEE\xF3",
    [STR_PHOTOS_STOP]          = "\xD1\xF2\xEE\xEF",
    [STR_PHOTO_SKY]            = "\xCD\xE5\xE1\xEE",
    [STR_PHOTO_SUNSET]         = "\xC7\xE0\xEA\xE0\xF2",
    [STR_PHOTO_PLASMA]         = "\xCF\xEB\xE0\xE7\xEC\xE0",
    [STR_PHOTO_CHESS]          = "\xD8\xE0\xF5\xEC\xE0\xF2\xFB",
    [STR_PHOTO_MANDELBROT]     = "\xCC\xE0\xED\xE4\xE5\xEB\xFC\xE1\xF0\xEE\xF2",
    [STR_PHOTO_STARS]          = "\xC7\xE2\xB8\xE7\xE4\xFB",
    [STR_PHOTO_RAINBOW]        = "\xD0\xE0\xE4\xF3\xE3\xE0",
    [STR_PHOTO_WAVES]          = "\xC2\xEE\xEB\xED\xFB",

    [STR_MEDIA_NOW]            = "\xD1\xE5\xE9\xF7\xE0\xF1 \xE8\xE3\xF0\xE0\xE5\xF2",
    [STR_MEDIA_PLAY]           = "\xC8\xE3\xF0\xE0\xF2\xFC",
    [STR_MEDIA_PAUSE]          = "\xCF\xE0\xF3\xE7\xE0",
    [STR_MEDIA_PREV]           = "< \xCF\xF0\xE5\xE4",
    [STR_MEDIA_NEXT]           = "\xD1\xEB\xE5\xE4 >",
    [STR_MEDIA_PLAYING]        = "\xC8\xE3\xF0\xE0\xE5\xF2",
    [STR_MEDIA_STOPPED]        = "\xCE\xF1\xF2\xE0\xED\xEE\xE2\xEB\xE5\xED\xEE",
    [STR_TRACK_SCALE]          = "\xC2\xEE\xF1\xF5\xEE\xE4\xFF\xF9\xE0\xFF \xE3\xE0\xEC\xEC\xE0",
    [STR_TRACK_TWINKLE]        = "\xC7\xE2\xB8\xE7\xE4\xEE\xF7\xEA\xE0",
    [STR_TRACK_ODE]            = "\xCE\xE4\xE0 \xEA \xF0\xE0\xE4\xEE\xF1\xF2\xE8",
    [STR_TRACK_FUR]            = "\xCA \xDD\xEB\xE8\xE7\xE5",
    [STR_TRACK_BEEP]           = "\xCC\xEE\xE4\xE5\xEC\xED\xFB\xE9 \xF1\xE8\xE3\xED\xE0\xEB",

    [STR_BR_FILE]              = "\xD4\xE0\xE9\xEB",
    [STR_BR_EDIT]              = "\xCF\xF0\xE0\xE2\xEA\xE0",
    [STR_BR_VIEW]              = "\xC2\xE8\xE4",
    [STR_BR_HISTORY]           = "\xC6\xF3\xF0\xED\xE0\xEB",
    [STR_BR_HELP]              = "\xD1\xEF\xF0\xE0\xE2\xEA\xE0",
    [STR_BR_HTTPS_NO]          = "(HTTPS: \xED\xE5\xF2)",
    [STR_BR_DONE]              = "\xC3\xEE\xF2\xEE\xE2\xEE",
    [STR_BR_FETCHING]          = "\xC7\xE0\xE3\xF0\xF3\xE7\xEA\xE0...",
    [STR_BR_COOKIES]           = "\xCA\xF3\xEA\xE8: %d",
    [STR_BR_BACK]              = "<",
    [STR_BR_FWD]               = ">",
    [STR_BR_REF]               = "R",
    [STR_BR_HOME]              = "H",
    [STR_BR_GO]                = "->",

    [STR_TERM_HELP] =
        "\xD2\xE5\xF0\xEC\xE8\xED\xE0\xEB FOS\n"
        "\xD4\xE0\xE9\xEB\xFB:     ls cat touch write rm cp mv\n"
        "           head tail wc grep file stat tree df\n"
        "\xCE\xE1\xEE\xEB\xEE\xF7\xEA\xE0:  echo clear pwd cd which history\n"
        "\xD1\xE8\xF1\xF2\xE5\xEC\xE0:    ver uname mem free uptime date\n"
        "           hostname whoami sleep\n"
        "\xD1\xE5\xF2\xFC:      ifconfig ping nslookup wget curl\n"
        "\xFF\xE7\xFB\xEA:      lang [en|ru]\n"
        "\xD1\xEA\xF0\xE8\xEF\xF2\xFB:  run FILE, bash FILE",
};

void i18n_init(void) {
    g_lang = LANG_EN;
}

void i18n_set(int lang) {
    if (lang == LANG_EN || lang == LANG_RU) g_lang = lang;
}

int i18n_get(void) { return g_lang; }

const char *i18n_name(void) {
    return (g_lang == LANG_RU) ? "\xD0\xF3\xF1\xF1\xEA\xE8\xE9" : "English";
}

const char *tr(int id) {
    if (id <= 0 || id >= STR_COUNT) return "";
    if (g_lang == LANG_RU) return g_ru[id] ? g_ru[id] : "";
    return g_en[id] ? g_en[id] : "";
}