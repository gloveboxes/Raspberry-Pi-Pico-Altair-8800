/* Stub header - static HTML embedding disabled */
#ifndef STATIC_HTML_HEX_H
#define STATIC_HTML_HEX_H

#include <stddef.h>

/* Empty HTML fallback */
static const unsigned char static_html_gz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

static const size_t static_html_gz_len = sizeof(static_html_gz);

#endif /* STATIC_HTML_HEX_H */
