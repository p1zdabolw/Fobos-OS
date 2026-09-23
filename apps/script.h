#ifndef FOS_APP_SCRIPT_H
#define FOS_APP_SCRIPT_H

typedef void (*script_out_fn)(void *ctx, const char *line);
typedef void (*script_cmd_fn)(void *ctx, const char *cmd);

int script_run(const char *filename,
               script_out_fn out, void *out_ctx,
               script_cmd_fn cmd, void *cmd_ctx);

#endif