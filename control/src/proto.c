#include "keeper.h"

#include <string.h>

void parser_init(parser_t *p)
{
    p->len = 0;
}

void parser_feed(parser_t *p, const uint8_t *data, size_t n,
                 void (*cb)(uint8_t type, const uint8_t *payload, size_t len,
                            void *user),
                 void *user)
{
    while (n > 0) {
        size_t space = sizeof(p->buf) - p->len;
        size_t take = n < space ? n : space;
        memcpy(p->buf + p->len, data, take);
        p->len += take;
        data += take;
        n -= take;

        for (;;) {
            uint8_t *sof = memchr(p->buf, PKT_SOF, p->len);
            if (sof == NULL) {
                p->len = 0; // no frame start in sight: drop garbage
                break;
            }
            if (sof != p->buf) {
                size_t skip = (size_t)(sof - p->buf);
                memmove(p->buf, sof, p->len - skip);
                p->len -= skip;
            }
            if (p->len < 4) {
                break;
            }
            uint8_t type = p->buf[1];
            size_t plen = (size_t)p->buf[2] | ((size_t)p->buf[3] << 8);
            if ((type != PKT_TYPE_LOG && type != PKT_TYPE_CTRL) ||
                plen > PKT_LEN_MAX) {
                // bogus header: skip this SOF byte and resync
                memmove(p->buf, p->buf + 1, p->len - 1);
                p->len--;
                continue;
            }
            if (p->len < 4 + plen) {
                break; // wait for the rest of the payload
            }
            cb(type, p->buf + 4, plen, user);
            size_t consumed = 4 + plen;
            memmove(p->buf, p->buf + consumed, p->len - consumed);
            p->len -= consumed;
        }
    }
}
