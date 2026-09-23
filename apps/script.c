#include "script.h"
#include "../kernel/fs.h"
#include "../kernel/timer.h"
#include "../lib/string.h"
#include "../lib/mem.h"

#define MAX_SCRIPTS   4
#define MAX_LINES     128
#define LINE_MAX      200
#define MAX_VARS      16
#define VAR_NAME_MAX  32
#define VAR_VAL_MAX   64

struct script_var {
    char name[VAR_NAME_MAX];
    char value[VAR_VAL_MAX];
};

struct script_state {
    char lines[MAX_LINES][LINE_MAX];
    int  line_count;
    int  pc;
    int  echo_off;
    int  exited;
    int  rc;
    struct script_var vars[MAX_VARS];
    int  var_count;
    script_out_fn out;
    void *out_ctx;
    script_cmd_fn cmd;
    void *cmd_ctx;
};

static struct script_state g_scripts[MAX_SCRIPTS];
static int g_depth = 0;

static void split_lines(struct script_state *st, const char *content, usize len) {
    int col = 0;
    st->line_count = 0;
    for (usize i = 0; i < len && st->line_count < MAX_LINES; i++) {
        char c = content[i];
        if (c == '\r') continue;
        if (c == '\n') {
            st->lines[st->line_count][col] = 0;
            st->line_count++;
            col = 0;
        } else if (col < LINE_MAX - 1) {
            st->lines[st->line_count][col++] = c;
        }
    }
    if (col > 0 && st->line_count < MAX_LINES) {
        st->lines[st->line_count][col] = 0;
        st->line_count++;
    }
}

static const char *lookup_var(const struct script_state *st, const char *name) {
    for (int k = 0; k < st->var_count; k++) {
        if (strcmp(st->vars[k].name, name) == 0) return st->vars[k].value;
    }
    return "";
}

static void expand(const struct script_state *st, const char *in, char *out, int cap) {
    int o = 0;
    int i = 0;
    while (in[i] && o < cap - 1) {
        if (in[i] == '%') {
            int j = i + 1;
            char name[VAR_NAME_MAX];
            int n = 0;
            while (in[j] && in[j] != '%' && n < VAR_NAME_MAX - 1) {
                name[n++] = in[j++];
            }
            name[n] = 0;
            if (in[j] == '%' && n > 0) {
                const char *val = lookup_var(st, name);
                while (*val && o < cap - 1) out[o++] = *val++;
                i = j + 1;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

static void set_var(struct script_state *st, const char *name, const char *value) {
    for (int k = 0; k < st->var_count; k++) {
        if (strcmp(st->vars[k].name, name) == 0) {
            usize vl = strlen(value);
            if (vl > VAR_VAL_MAX - 1) vl = VAR_VAL_MAX - 1;
            memcpy(st->vars[k].value, value, vl);
            st->vars[k].value[vl] = 0;
            return;
        }
    }
    if (st->var_count >= MAX_VARS) return;
    struct script_var *v = &st->vars[st->var_count++];
    usize nl = strlen(name);
    if (nl > VAR_NAME_MAX - 1) nl = VAR_NAME_MAX - 1;
    memcpy(v->name, name, nl);
    v->name[nl] = 0;
    usize vl = strlen(value);
    if (vl > VAR_VAL_MAX - 1) vl = VAR_VAL_MAX - 1;
    memcpy(v->value, value, vl);
    v->value[vl] = 0;
}

static int find_label(struct script_state *st, const char *label) {
    for (int i = 0; i < st->line_count; i++) {
        const char *l = st->lines[i];
        if (l[0] != ':') continue;
        const char *p = l + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (strcmp(p, label) == 0) return i;
    }
    return -1;
}

static void emit(struct script_state *st, const char *line) {
    if (st->out) st->out(st->out_ctx, line);
}

static void run(struct script_state *st) {
    st->pc = 0;
    st->exited = 0;
    st->rc = 0;
    while (st->pc < st->line_count && !st->exited) {
        char *raw = st->lines[st->pc];
        while (*raw == ' ' || *raw == '\t') raw++;

        if (*raw == 0) { st->pc++; continue; }
        if (raw[0] == ':') { st->pc++; continue; }
        if (strncmp(raw, "rem ", 4) == 0 || strcmp(raw, "rem") == 0) { st->pc++; continue; }
        if (strncmp(raw, "::", 2) == 0) { st->pc++; continue; }

        char *line = raw;
        int quiet = 0;
        if (line[0] == '@') { quiet = 1; line++; }

        if (strncmp(line, "echo off", 8) == 0) { st->echo_off = 1; st->pc++; continue; }
        if (strncmp(line, "echo on", 7) == 0)  { st->echo_off = 0; st->pc++; continue; }

        char expanded[LINE_MAX];
        expand(st, line, expanded, (int)sizeof(expanded));

        if (!quiet && !st->echo_off) {
            char shown[LINE_MAX + 4];
            shown[0] = '>';
            shown[1] = ' ';
            usize n = strlen(expanded);
            if (n > LINE_MAX - 1) n = LINE_MAX - 1;
            memcpy(shown + 2, expanded, n);
            shown[2 + n] = 0;
            emit(st, shown);
        }

        if (strncmp(expanded, "echo ", 5) == 0) {
            emit(st, expanded + 5);
            st->pc++;
            continue;
        }
        if (strcmp(expanded, "echo") == 0) {
            emit(st, "");
            st->pc++;
            continue;
        }
        if (strncmp(expanded, "set ", 4) == 0) {
            const char *eq = expanded + 4;
            const char *p = eq;
            while (*p && *p != '=') p++;
            if (*p == '=') {
                char name[VAR_NAME_MAX];
                int n = (int)(p - eq);
                if (n > VAR_NAME_MAX - 1) n = VAR_NAME_MAX - 1;
                memcpy(name, eq, (usize)n);
                name[n] = 0;
                set_var(st, name, p + 1);
            }
            st->pc++;
            continue;
        }
        if (strncmp(expanded, "goto ", 5) == 0) {
            const char *lab = expanded + 5;
            while (*lab == ' ' || *lab == '\t') lab++;
            int tgt = find_label(st, lab);
            if (tgt < 0) {
                emit(st, "goto: label not found");
                st->pc++;
            } else {
                st->pc = tgt + 1;
            }
            continue;
        }
        if (strncmp(expanded, "pause", 5) == 0) {
            emit(st, "Press any key to continue . . .");
            u64 target = timer_ticks() + 150;
            while (timer_ticks() < target) { }
            st->pc++;
            continue;
        }
        if (strcmp(expanded, "exit") == 0 ||
            strncmp(expanded, "exit ", 5) == 0) {
            st->exited = 1;
            st->rc = 0;
            break;
        }

        if (st->cmd) st->cmd(st->cmd_ctx, expanded);
        st->pc++;
    }
}

int script_run(const char *filename,
               script_out_fn out, void *out_ctx,
               script_cmd_fn cmd, void *cmd_ctx) {
    if (g_depth >= MAX_SCRIPTS) {
        if (out) out(out_ctx, "script: too many nested runs");
        return -1;
    }
    usize sz = 0;
    void *data = fs_read(filename, &sz);
    if (!data || sz == 0) {
        if (out) out(out_ctx, "script: cannot open file");
        return -1;
    }
    struct script_state *st = &g_scripts[g_depth++];
    memset(st, 0, sizeof(*st));
    st->out = out;
    st->out_ctx = out_ctx;
    st->cmd = cmd;
    st->cmd_ctx = cmd_ctx;

    split_lines(st, (const char*)data, sz);
    run(st);
    int rc = st->rc;
    g_depth--;
    return rc;
}