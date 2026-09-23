#include "media.h"
#include "../gui/window.h"
#include "../gui/font.h"
#include "../gui/widget.h"
#include "../kernel/fb.h"
#include "../kernel/timer.h"
#include "../kernel/speaker.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../lib/i18n.h"

struct note {
    u32 hz;
    u32 ms;
};

static const struct note TRACK_SCALE[] = {
    {262, 220}, {294, 220}, {330, 220}, {349, 220},
    {392, 220}, {440, 220}, {494, 220}, {523, 320},
    {0, 120},
    {523, 220}, {494, 220}, {440, 220}, {392, 220},
    {349, 220}, {330, 220}, {294, 220}, {262, 420},
    {0, 240},
    {0, 0}
};

static const struct note TRACK_TWINKLE[] = {
    {262, 320}, {262, 320}, {392, 320}, {392, 320},
    {440, 320}, {440, 320}, {392, 640},
    {349, 320}, {349, 320}, {330, 320}, {330, 320},
    {294, 320}, {294, 320}, {262, 640},
    {0, 260},
    {0, 0}
};

static const struct note TRACK_ODE[] = {
    {330, 320}, {330, 320}, {349, 320}, {392, 320},
    {392, 320}, {349, 320}, {330, 320}, {294, 320},
    {262, 320}, {262, 320}, {294, 320}, {330, 320},
    {330, 460}, {294, 180}, {294, 660},
    {0, 240},
    {0, 0}
};

static const struct note TRACK_FUR[] = {
    {659, 160}, {622, 160}, {659, 160}, {622, 160},
    {659, 160}, {494, 160}, {587, 160}, {523, 160},
    {440, 420}, {0, 120},
    {262, 160}, {294, 160}, {330, 160}, {349, 160},
    {392, 160}, {440, 160}, {494, 160}, {523, 200},
    {0, 240},
    {0, 0}
};

static const struct note TRACK_BEEP[] = {
    {880, 100}, {0, 100}, {880, 100}, {0, 100},
    {880, 100}, {0, 100}, {880, 100}, {0, 100},
    {660, 600},
    {880, 100}, {880, 100}, {880, 100}, {0, 200},
    {0, 0}
};

struct track {
    int str_id;
    const struct note *notes;
    int len;
};

static const struct track TRACKS[] = {
    { STR_TRACK_SCALE,   TRACK_SCALE,   sizeof(TRACK_SCALE)   / sizeof(struct note) },
    { STR_TRACK_TWINKLE, TRACK_TWINKLE, sizeof(TRACK_TWINKLE) / sizeof(struct note) },
    { STR_TRACK_ODE,     TRACK_ODE,     sizeof(TRACK_ODE)     / sizeof(struct note) },
    { STR_TRACK_FUR,     TRACK_FUR,     sizeof(TRACK_FUR)     / sizeof(struct note) },
    { STR_TRACK_BEEP,    TRACK_BEEP,    sizeof(TRACK_BEEP)    / sizeof(struct note) },
};

#define TRACK_COUNT ((int)(sizeof(TRACKS) / sizeof(TRACKS[0])))
#define BARS 24

struct media_state {
    int  playing;
    int  track;
    int  note;
    u64  next_at;
    u32  bars[BARS];
    u64  last_bar_update;
};

static struct media_state g_media[MAX_WINDOWS];
static int g_active_slot = -1;

static u32 hash32_m(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void stop_all_but(int slot) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (i != slot) g_media[i].playing = 0;
    }
    g_active_slot = slot;
}

static void media_start_playback(struct media_state *st) {
    st->playing = 1;
    st->note = 0;
    st->next_at = timer_ticks();
    const struct track *t = &TRACKS[st->track];
    if (t->notes[0].hz) speaker_tone(t->notes[0].hz);
    else                speaker_off();
    st->next_at = timer_ticks() + t->notes[0].ms / 10 + 1;
}

static void media_advance(struct media_state *st) {
    const struct track *t = &TRACKS[st->track];
    st->note++;
    if (st->note >= t->len) {
        st->note = 0;
    }
    const struct note *n = &t->notes[st->note];
    if (n->hz == 0 && n->ms == 0) {
        st->playing = 0;
        st->note = 0;
        speaker_off();
        return;
    }
    if (n->hz == 0) speaker_off();
    else            speaker_tone(n->hz);
    st->next_at = timer_ticks() + n->ms / 10 + 1;
}

static void media_update_bars(struct media_state *st) {
    u64 now = timer_ticks();
    if (now - st->last_bar_update < 2) return;
    st->last_bar_update = now;

    const struct track *t = &TRACKS[st->track];
    u32 hz = 0;
    if (st->playing && st->note < t->len) hz = t->notes[st->note].hz;

    for (int i = 0; i < BARS; i++) {
        int target = 0;
        if (st->playing && hz != 0) {
            u32 hv = hash32_m((u32)i, (u32)(now / 2));
            target = 25 + (int)((hv >> 8) % 90);
        }
        int cur = (int)st->bars[i];
        int d = target - cur;
        if (d > 14) d = 14;
        if (d < -14) d = -14;
        st->bars[i] = (u32)(cur + d);
    }
}

static void media_buttons(int wx, int wy, struct widget_rect *play,
                          struct widget_rect *prev, struct widget_rect *next) {
    int fy = wy + 22 + 40 + 140 + 8;
    play->x = wx + 8;   play->y = fy; play->w = 90; play->h = 24;
    prev->x = wx + 104; prev->y = fy; prev->w = 80; prev->h = 24;
    next->x = wx + 190; next->y = fy; next->w = 80; next->h = 24;
}

static void media_draw(struct window *win) {
    struct media_state *st = (struct media_state*)win->user;

    if (st->playing) {
        if (timer_ticks() >= st->next_at) {
            media_advance(st);
        } else {
            const struct track *t = &TRACKS[st->track];
            if (st->note < t->len && t->notes[st->note].hz != 0) {
                speaker_tone(t->notes[st->note].hz);
            }
        }
    }
    media_update_bars(st);

    int bx = win->x + 8;
    int by = win->y + 22 + 6;
    int w = win->w - 16;

    fb_fill_rect(bx, by, w, 34, 0x1A1E28);
    font_draw_string(bx + 6, by + 4, tr(STR_MEDIA_NOW), 0x8FB8E0, 0x1A1E28);
    char tbuf[80];
    snprintf(tbuf, sizeof(tbuf), "%s  [%d/%d]",
             tr(TRACKS[st->track].str_id), st->track + 1, TRACK_COUNT);
    font_draw_string(bx + 6, by + 18, tbuf, 0xE0E8F0, 0x1A1E28);

    int vx = bx;
    int vy = by + 40;
    int vw = w;
    int vh = 140;
    fb_fill_rect(vx, vy, vw, vh, 0x080C10);
    fb_fill_rect(vx, vy, vw, 1, 0x2A3A50);
    fb_fill_rect(vx, vy + vh - 1, vw, 1, 0x2A3A50);

    int bar_w = vw / BARS;
    if (bar_w < 2) bar_w = 2;
    for (int i = 0; i < BARS; i++) {
        int h = (int)st->bars[i];
        if (h > vh - 6) h = vh - 6;
        int col_x = vx + 2 + i * bar_w;
        int col_y = vy + vh - 4 - h;
        u32 top = 0x40D080;
        u32 bot = 0x105030;
        if (i > BARS * 2 / 3) { top = 0xF0D050; bot = 0x604010; }
        else if (i > BARS / 3) { top = 0x60C8F0; bot = 0x103050; }
        int seg_h = 4;
        for (int yy = 0; yy < h; yy += seg_h) {
            int sy = col_y + yy;
            if (sy < vy + 2) continue;
            int eh = seg_h - 1;
            if (sy + eh > vy + vh - 4) eh = vy + vh - 4 - sy;
            if (eh <= 0) continue;
            u32 c = (yy < h / 2) ? top : bot;
            fb_fill_rect(col_x, sy, bar_w - 1, eh, c);
        }
    }

    const char *state = st->playing ? tr(STR_MEDIA_PLAYING) : tr(STR_MEDIA_STOPPED);
    font_draw_string(vx + 6, vy + 4, state, 0xE0E8F0, 0x080C10);

    struct widget_rect bplay, bprev, bnext;
    media_buttons(win->x, win->y, &bplay, &bprev, &bnext);
    widget_draw_button(&bplay, st->playing ? tr(STR_MEDIA_PAUSE) : tr(STR_MEDIA_PLAY), 0);
    widget_draw_button(&bprev, tr(STR_MEDIA_PREV), 0);
    widget_draw_button(&bnext, tr(STR_MEDIA_NEXT), 0);
}

static void media_click(struct window *win, int x, int y) {
    struct media_state *st = (struct media_state*)win->user;
    struct widget_rect bplay, bprev, bnext;
    media_buttons(0, 0, &bplay, &bprev, &bnext);

    if (widget_point_in(&bplay, x, y)) {
        if (st->playing) {
            st->playing = 0;
            speaker_off();
            g_active_slot = -1;
        } else {
            stop_all_but((int)(st - g_media));
            media_start_playback(st);
        }
        return;
    }
    if (widget_point_in(&bprev, x, y)) {
        st->track = (st->track + TRACK_COUNT - 1) % TRACK_COUNT;
        if (st->playing) media_start_playback(st);
        return;
    }
    if (widget_point_in(&bnext, x, y)) {
        st->track = (st->track + 1) % TRACK_COUNT;
        if (st->playing) media_start_playback(st);
        return;
    }
}

static struct media_state *media_alloc(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_media[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) if (!used[i]) return &g_media[i];
    return NULL;
}

void media_tick(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_media[i].playing) continue;
        int found = 0;
        for (int w = 0; w < window_count(); w++) {
            struct window *win = window_get(w);
            if (win && win->visible && win->user == &g_media[i]) { found = 1; break; }
        }
        if (!found) {
            g_media[i].playing = 0;
            if (g_active_slot == i) {
                speaker_off();
                g_active_slot = -1;
            }
        }
    }
}

void apps_launch_media(void) {
    struct media_state *st = media_alloc();
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->track = 0;
    st->playing = 0;

    int win_w = 370;
    int win_h = 22 + 6 + 34 + 6 + 140 + 8 + 24 + 6;
    int id = window_create(220, 130, win_w, win_h, tr(STR_APP_MEDIA));
    if (id < 0) return;
    struct window *win = window_get(id);
    win->user = st;
    win->on_draw = media_draw;
    win->on_click = media_click;
}