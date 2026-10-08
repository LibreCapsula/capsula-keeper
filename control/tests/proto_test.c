/* Unit tests for the keeper frame parser (no hardware needed): make check */
#include <stdio.h>
#include <string.h>

#include "../src/keeper.h"

static struct {
    uint8_t type;
    char payload[PKT_LEN_MAX + 1];
    size_t len;
} captured[64];
static int ncap;

static void test_cb(uint8_t type, const uint8_t *payload, size_t len,
                    void *user)
{
    (void)user;
    if (ncap < 64) {
        captured[ncap].type = type;
        captured[ncap].len = len;
        size_t n = len < PKT_LEN_MAX ? len : PKT_LEN_MAX;
        memcpy(captured[ncap].payload, payload, n);
        captured[ncap].payload[n] = '\0';
    }
    ncap++;
}

static size_t mkframe(uint8_t *out, uint8_t type, const void *payload,
                      size_t plen)
{
    out[0] = PKT_SOF;
    out[1] = type;
    out[2] = (uint8_t)(plen & 0xFF);
    out[3] = (uint8_t)((plen >> 8) & 0xFF);
    memcpy(out + 4, payload, plen);
    return 4 + plen;
}

static int fails = 0;

#define CHECK(cond, name)                                          \
    do {                                                           \
        if (cond) {                                                \
            printf("[ok] %s\n", name);                             \
        } else {                                                   \
            printf("[FAIL] %s (line %d)\n", name, __LINE__);       \
            fails++;                                               \
        }                                                          \
    } while (0)

static void reset(void)
{
    ncap = 0;
    memset(captured, 0, sizeof(captured));
}

int main(void)
{
    uint8_t buf[3 * PKT_LEN_MAX];
    parser_t p;

    /* 1. adjacent frames */
    reset();
    parser_init(&p);
    size_t n1 = mkframe(buf, PKT_TYPE_LOG, "hello", 5);
    size_t n2 = mkframe(buf + n1, PKT_TYPE_CTRL, "OK", 2);
    parser_feed(&p, buf, n1 + n2, test_cb, NULL);
    CHECK(ncap == 2 && captured[0].type == PKT_TYPE_LOG &&
              strcmp(captured[0].payload, "hello") == 0 &&
              captured[1].type == PKT_TYPE_CTRL &&
              strcmp(captured[1].payload, "OK") == 0,
          "adjacent frames");

    /* 2. byte-at-a-time delivery */
    reset();
    parser_init(&p);
    n1 = mkframe(buf, PKT_TYPE_LOG, "abc", 3);
    for (size_t i = 0; i < n1; i++) {
        parser_feed(&p, buf + i, 1, test_cb, NULL);
    }
    CHECK(ncap == 1 && captured[0].len == 3 &&
              strcmp(captured[0].payload, "abc") == 0,
          "byte-at-a-time");

    /* 3. garbage and a bogus header before a valid frame -> resync */
    reset();
    parser_init(&p);
    size_t off = 0;
    memcpy(buf + off, "\x00\x01\x02", 3);
    off += 3;
    off += mkframe(buf + off, 0x7F, "bad-type", 8); // unknown type
    // header claiming a huge payload (only the 4 header bytes exist)
    buf[off++] = PKT_SOF;
    buf[off++] = PKT_TYPE_LOG;
    buf[off++] = 0xFF;
    buf[off++] = 0xFF;
    n1 = mkframe(buf + off, PKT_TYPE_LOG, "x", 1);
    parser_feed(&p, buf, off + n1, test_cb, NULL);
    CHECK(ncap == 1 && captured[0].len == 1 &&
              strcmp(captured[0].payload, "x") == 0,
          "resync past garbage");

    /* 4. frame split across feeds */
    reset();
    parser_init(&p);
    n1 = mkframe(buf, PKT_TYPE_CTRL, "OK loader done", 14);
    parser_feed(&p, buf, 5, test_cb, NULL);
    CHECK(ncap == 0, "no premature frame on partial feed");
    parser_feed(&p, buf + 5, n1 - 5, test_cb, NULL);
    CHECK(ncap == 1 && strcmp(captured[0].payload, "OK loader done") == 0,
          "split frame");

    /* 5. log payload containing SOF bytes and fake headers */
    reset();
    parser_init(&p);
    static uint8_t payload[PKT_LEN_MAX];
    size_t plen = 0;
    for (int i = 0; i < 10; i++) {
        plen += mkframe(payload + plen, PKT_TYPE_LOG, "fake", 4);
    }
    memcpy(payload + plen, "real log line\r\n", 15);
    plen += 15;
    n1 = mkframe(buf, PKT_TYPE_LOG, payload, plen);
    parser_feed(&p, buf, n1, test_cb, NULL);
    CHECK(ncap == 1 && captured[0].len == plen &&
              memcmp(captured[0].payload, payload, plen) == 0,
          "log payload with embedded SOF bytes");

    /* 6. maximal-size frame */
    reset();
    parser_init(&p);
    memset(payload, 'A', PKT_LEN_MAX);
    n1 = mkframe(buf, PKT_TYPE_LOG, payload, PKT_LEN_MAX);
    parser_feed(&p, buf, n1, test_cb, NULL);
    CHECK(ncap == 1 && captured[0].len == PKT_LEN_MAX, "max-size frame");

    if (fails) {
        printf("\n%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("\nproto_test OK\n");
    return 0;
}
