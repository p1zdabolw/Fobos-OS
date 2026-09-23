#ifndef FOS_HTTP_H
#define FOS_HTTP_H

#include "types.h"

#define HTTP_MAX_BODY        131072
#define HTTP_MAX_COOKIES     12
#define HTTP_MAX_COOKIE_LEN  160
#define HTTP_MAX_LOCATION    256
#define HTTP_MAX_REDIRECTS   6

struct http_response {
    int   status;
    char  content_type[64];
    char  location[HTTP_MAX_LOCATION];
    char  cookies[HTTP_MAX_COOKIES][HTTP_MAX_COOKIE_LEN];
    int   cookie_count;
    int   chunked;
    int   redirect_count;
    char  redirect_chain[HTTP_MAX_REDIRECTS][HTTP_MAX_LOCATION];
    usize body_len;
    u8   *body;
};

void cookie_jar_init(void);
void cookie_jar_clear(void);
int  cookie_jar_count(void);

int http_get_follow(char *url_io, usize url_cap,
                    struct http_response *out, u8 *body_buf, usize body_cap);

int http_get_raw(const char *host, u16 port, const char *path,
                 const char *extra_headers,
                 struct http_response *out, u8 *body_buf, usize body_cap);

const char *http_status_text(int code);

#endif