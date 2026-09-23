#ifndef FOS_HTML_H
#define FOS_HTML_H

#include "types.h"

#define HTML_MAX_LINES 260
#define HTML_LINE_LEN  96
#define HTML_LINK_LEN  320
#define HTML_LIST_DEPTH 8

struct html_line {
    char text[HTML_LINE_LEN];
    char link[HTML_LINK_LEN];
    int  is_header;
    int  is_link;
    int  is_pre;
};

struct html_doc {
    char title[160];
    char base_host[128];
    char base_scheme[8];
    struct html_line lines[HTML_MAX_LINES];
    int  line_count;
    int  truncated;
};

int html_parse(const char *html, usize len, const char *base_url, struct html_doc *out);

#endif