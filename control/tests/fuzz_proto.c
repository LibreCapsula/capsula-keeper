/* Fuzz harness for the frame parser: random garbage, split feeds and
 * adversarial headers, with ASan/UBSan watching. Not part of `make check`
 * (needs a fuzzer or long loops); build standalone:
 *   cc -fsanitize=address,undefined -g tests/fuzz_proto.c src/proto.c -o build/fuzz_proto
 */
#include "keeper.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long frames_seen;
static unsigned long bytes_seen;

static void count_frames(uint8_t type, const uint8_t *payload, size_t len,
                         void *user)
{
    (void)user;
    if (type != PKT_TYPE_LOG && type != PKT_TYPE_CTRL) {
        fprintf(stderr, "fuzz: illegal type %u delivered\n", type);
        exit(2);
    }
    if (len > PKT_LEN_MAX) {
        fprintf(stderr, "fuzz: oversize payload %zu\n", len);
        exit(2);
    }
    frames_seen++;
    bytes_seen += len;
}

static uint32_t rng_state = 0x12345678;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 20000;
    size_t chunk_max = argc > 2 ? (size_t)atol(argv[2]) : 300;
    uint8_t buf[4096];
    parser_t p;

    for (long it = 0; it < iterations; it++) {
        parser_init(&p);
        size_t n = 0;
        switch (rng() % 4) {
        case 0: /* pure random garbage */
            n = rng() % sizeof(buf);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (uint8_t)rng();
            }
            break;
        case 1: /* garbage seeded with SOF bytes */
            n = rng() % sizeof(buf);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (rng() % 8 == 0) ? PKT_SOF : (uint8_t)rng();
            }
            break;
        case 2: { /* valid frame wrapped in garbage with a hostile header */
            size_t plen = rng() % (PKT_LEN_MAX + 4); /* sometimes > PKT_LEN_MAX */
            uint8_t type = (rng() % 2) ? PKT_TYPE_LOG : (uint8_t)(rng() & 0xFF);
            size_t frame_len = 4 + (plen <= PKT_LEN_MAX ? plen : 0);
            size_t pre = rng() % 64;
            n = 0;
            for (size_t i = 0; i < pre && n < sizeof(buf); i++) {
                buf[n++] = (uint8_t)rng();
            }
            if (frame_len <= sizeof(buf) - n) {
                buf[n++] = PKT_SOF;
                buf[n++] = type;
                buf[n++] = (uint8_t)(plen & 0xFF);
                buf[n++] = (uint8_t)(plen >> 8);
                for (size_t i = 0; i < plen && plen <= PKT_LEN_MAX; i++) {
                    buf[n++] = (uint8_t)rng();
                }
            }
            while (n < sizeof(buf)) {
                buf[n++] = (uint8_t)rng();
            }
            break;
        }
        default: /* two valid frames plus noise */
            size_t n1 = rng() % 512;
            size_t n2 = rng() % 512;
            if (n1 + n2 + 16 > sizeof(buf)) {
                n1 = n2 = 0;
            }
            buf[0] = PKT_SOF;
            buf[1] = PKT_TYPE_LOG;
            buf[2] = (uint8_t)n1;
            buf[3] = (uint8_t)(n1 >> 8);
            for (size_t i = 0; i < n1; i++) {
                buf[4 + i] = (uint8_t)rng();
            }
            buf[4 + n1] = PKT_SOF;
            buf[5 + n1] = PKT_TYPE_CTRL;
            buf[6 + n1] = (uint8_t)n2;
            buf[7 + n1] = (uint8_t)(n2 >> 8);
            for (size_t i = 0; i < n2; i++) {
                buf[8 + n1 + i] = (uint8_t)rng();
            }
            n = 8 + n1 + n2;
            break;
        }

        /* feed in random-size chunks, sometimes single bytes */
        size_t off = 0;
        while (off < n) {
            size_t chunk = (rng() % 16 == 0) ? 1 : rng() % chunk_max + 1;
            if (chunk > n - off) {
                chunk = n - off;
            }
            parser_feed(&p, buf + off, chunk, count_frames, NULL);
            off += chunk;
        }
    }
    printf("fuzz done: %ld iterations, %lu frames, %lu payload bytes\n",
           iterations, frames_seen, bytes_seen);
    return 0;
}
