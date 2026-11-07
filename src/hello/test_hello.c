/*
 * test_hello.c — native unit test for the hello module (stub host).
 *
 * Build & run (host toolchain, no wasm needed):
 *   cc -DXUANSI_NATIVE_TEST -Wall -Wextra -I src -I src/hello \
 *      src/hello/test_hello.c -o build/test_hello && ./build/test_hello
 *
 * The test includes hello.c directly (one translation unit) so the
 * module's static marshalling helpers are reachable, provides the
 * `xuansi` host namespace as stubs, and drives every entry function
 * through the same u32-offset marshalling the wasm host uses — the
 * test owns the "linear memory" arena via xuansi_native_init().
 *
 * Host-side refusals (wasm exceptions on the board) are emulated with
 * a trap longjmp: stub_raise() records the message and jumps out, the
 * same way the real host aborts the call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "hello.c"   /* unity build: the module under test */

/* ---- stub host state ---- */

static uint32_t s_millis;                 /* fake clock */
static uint32_t s_delay_acc;              /* cumulative delay budget (cap 15000) */
static uint8_t  s_gpio_level[64];         /* fake pin levels */
static uint8_t  s_stream_last[64];        /* last stream_send payload */
static uint32_t s_stream_last_len;
static uint32_t s_spi_xfers;              /* spi_xfer call count */
static int      s_camera_on;              /* camera stub state */

/* trap emulation: the host refusal channel */
static jmp_buf s_jmp;
static int     s_trap_armed;
static char    s_trap_msg[128];
static void stub_raise(const char *msg)
{
    if (!s_trap_armed) {
        fprintf(stderr, "FATAL: stub host raised outside an armed trap: %s\n", msg);
        exit(2);
    }
    snprintf(s_trap_msg, sizeof s_trap_msg, "%s", msg);
    longjmp(s_jmp, 1);
}

/* ---- the `xuansi` namespace stubs (signatures from xuansi_host.h) ---- */

void xuansi_millis(uint32_t *out)
{
    *out = s_millis;
}

void xuansi_delay_ms(uint32_t ms)
{
    /* mirrors the frozen host semantics: <=50ms slices, cumulative cap
     * 15000ms per call chain entry — over the cap raises. The check is
     * SUBTRACTIVE on purpose: an additive `acc + ms > cap` check wraps
     * on u32 (acc=100, ms=0xFFFFFFF8 -> 4) and would let a wrap attempt
     * through — exactly what the delay_wrap vector pins. */
    if (ms > 15000u - s_delay_acc)
        stub_raise("xuansi.delay_ms: cumulative delay budget exceeded");
    s_delay_acc += ms;
    s_millis += ms;
}

void xuansi_gpio_set(uint32_t pin, uint32_t level)
{
    if (pin >= 64) stub_raise("xuansi.gpio_set: pin out of range");
    s_gpio_level[pin] = (uint8_t)level;
    /* emulate the bench jumper wire: driving pin P reflects on pin P+1
     * (the gpio_echo default pair is out=1 -> in=2) */
    if (pin + 1 < 64) s_gpio_level[pin + 1] = (uint8_t)level;
}

void xuansi_gpio_get(uint32_t pin, uint32_t *out)
{
    if (pin >= 64) stub_raise("xuansi.gpio_get: pin out of range");
    *out = s_gpio_level[pin];
}

void xuansi_spi_config(uint32_t *out, uint32_t slot, uint32_t sclk_pin,
                       uint32_t sclk_hz, int32_t mosi, int32_t miso,
                       uint32_t mode, uint32_t no_cs)
{
    (void)sclk_pin; (void)sclk_hz; (void)mosi; (void)miso;
    if (slot > 1)      stub_raise("xuansi.spi_config: bad slot");
    if (mode > 3)      stub_raise("xuansi.spi_config: bad mode");
    if (no_cs == 0)    stub_raise("xuansi.spi_config: CS bus unsupported at abi=1");
    *out = 0;
}

void xuansi_spi_xfer(uint32_t *out, uint32_t slot, uint32_t wbuf_off,
                     uint32_t wlen, uint32_t rbuf_off, uint32_t rcap)
{
    (void)slot;
    if (wlen && rcap && wlen != rcap)
        stub_raise("xuansi.spi_xfer: wlen != rcap");
    if (wlen > 4096 || rcap > 4096)
        stub_raise("xuansi.spi_xfer: transfer over 4096B");
    if (wlen) memcpy(xuansi_lin_base + rbuf_off, xuansi_lin_base + wbuf_off, wlen);
    s_spi_xfers++;
    *out = wlen ? wlen : rcap;
}

void xuansi_camera_frame_get(uint32_t *out, uint32_t buf_off, uint32_t cap)
{
    if (!s_camera_on) stub_raise("xuansi.camera_frame_get: camera not on");
    uint32_t len = 320u * 240u * 2u;
    if (len > cap) stub_raise("xuansi.camera_frame_get: frame over cap");
    memset(xuansi_lin_base + buf_off, 0xA5, len);
    *out = len;
}

void xuansi_camera_info(uint32_t *out)
{
    out[0] = s_camera_on ? 320u : 0u;
    out[1] = s_camera_on ? 240u : 0u;
    out[2] = 0u;   /* fs_code 0 = QVGA */
}

void xuansi_stream_send(uint32_t *out, uint32_t buf_off, uint32_t len)
{
    if (len > sizeof s_stream_last) stub_raise("xuansi.stream_send: over stub cap");
    memcpy(s_stream_last, xuansi_lin_base + buf_off, len);
    s_stream_last_len = len;
    *out = len;
}

/* ---- harness ---- */

static int checks, fails;
static void check(int ok, const char *name, const char *detail)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s (%s)\n", name, detail ? detail : "");
    } else {
        printf("PASS %s\n", name);
    }
}

/* the module's linear-memory arena (test-owned) */
static uint8_t arena[1u << 20] __attribute__((aligned(4096)));

/* decode a msgpack fixarray of ints from a module result */
static int res_ints(uint64_t packed, int64_t *vals, int max_n, int *n_out)
{
    const uint8_t *p = arena + (uint32_t)(packed >> 32);
    uint32_t len = (uint32_t)packed, pos = 0;
    if (len == 0 || pos >= len) return -1;
    uint8_t t = p[pos++];
    uint32_t n = 0;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;
    else return -1;
    if ((int)n > max_n) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (mp_read_int(p, len, &pos, &vals[i]) != 0) return -1;
    }
    *n_out = (int)n;
    return 0;
}

/* the wasm host's arg-writing step: alloc, then write msgpack bytes */
static uint32_t host_write_args(const uint8_t *bytes, uint32_t n)
{
    uint32_t off = xuansi_alloc(n);
    if (off == 0) { fprintf(stderr, "FATAL: alloc refused\n"); exit(2); }
    memcpy(arena + off, bytes, n);
    return off;
}

/* run <expr> expecting the stub host to raise; returns 1 when it did */
#define EXPECT_TRAP(expr, msgfrag)                                     \
    do {                                                               \
        s_trap_armed = 1; s_trap_msg[0] = 0;                           \
        if (setjmp(s_jmp) == 0) {                                      \
            (void)(expr);                                              \
            s_trap_armed = 0;                                          \
            check(0, msgfrag, "no trap raised");                       \
        } else {                                                       \
            s_trap_armed = 0;                                          \
            check(strstr(s_trap_msg, "xuansi.") == s_trap_msg,         \
                  msgfrag, s_trap_msg);                                \
        }                                                              \
    } while (0)

int main(void)
{
    int64_t v[8];
    int n = 0;

    xuansi_native_init(arena);

    /* ---- T01 abi + allocator ---- */
    check(xuansi_abi() == 1, "T01 xuansi_abi()==1", "");
    check(xuansi_alloc(0) == 0, "T02a alloc(0) refused", "");
    check(xuansi_alloc(HEAP_SZ + 1) == 0, "T02b alloc(over cap) refused", "");
    uint32_t a1 = xuansi_alloc(8);
    check(a1 == XUANSI_OFF(args_slot),
          "T02c small args alloc -> fixed args slot (fix1)", "");
    uint32_t a2 = xuansi_alloc(1);
    check(a2 == a1, "T02d small allocs reuse the slot (serialized calls)",
          "");
    uint32_t a3 = xuansi_alloc(200);
    check(a3 == 8, "T02e large alloc bumps from 8 (one-shot path)", "");

    /* ---- T03 echo: byte-level msgpack round-trip ---- */
    {
        static const uint8_t args[] = { 0x92, 7, 35 };   /* [7,35] */
        uint32_t off = host_write_args(args, sizeof args);
        uint64_t r = echo(off, sizeof args);
        check((uint32_t)(r >> 32) == off && (uint32_t)r == sizeof args,
              "T03 echo returns (ptr,len) unchanged", "");
    }

    /* ---- T04 add: [7,35] -> [42] ---- */
    {
        static const uint8_t args[] = { 0x92, 7, 35 };
        uint32_t off = host_write_args(args, sizeof args);
        uint64_t r = add(off, sizeof args);
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 42,
              "T04 add [7,35] -> [42]", "");
    }

    /* ---- T05 add with uint16 msgpack encoding: [1000,23] -> [1023] ---- */
    {
        static const uint8_t args[] = { 0x92, 0xcd, 0x03, 0xe8, 23 };
        uint32_t off = host_write_args(args, sizeof args);
        uint64_t r = add(off, sizeof args);
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 1023,
              "T05 add uint16 domain -> [1023]", "");
    }

    /* ---- T06 gpio_echo: default pins (out=1, in=2), stub wire mirrors ---- */
    {
        memset(s_gpio_level, 0, sizeof s_gpio_level);
        uint64_t r = gpio_echo(0, 0);
        check(res_ints(r, v, 8, &n) == 0 && n == 2 && v[0] == 1 && v[1] == 0,
              "T06 gpio_echo -> [1,0]", "");
    }

    /* ---- T07 now / now2x ---- */
    {
        s_millis = 1234;
        uint64_t r = now(0, 0);
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 1234,
              "T07a now -> [1234]", "");
        r = now2x(0, 0);
        check(res_ints(r, v, 8, &n) == 0 && n == 3 && v[0] == 1234
              && v[1] == 1234 && v[2] == 1234,
              "T07b now2x -> three live snapshots", "");
    }

    /* ---- T08 timed_delay: stub advances the clock by the delay ---- */
    {
        s_millis = 0; s_delay_acc = 0;
        uint64_t r = timed_delay(0, 0);
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 120,
              "T08 timed_delay -> [120]", "");
    }

    /* ---- T09 delay_wrap: budget cap refuses the wrap attempt ---- */
    {
        s_delay_acc = 0;
        EXPECT_TRAP(delay_wrap(0, 0), "T09 delay_wrap budget refusal");
    }

    /* ---- T10 stream_out: stub records the stamped payload ---- */
    {
        s_stream_last_len = 0;
        uint64_t r = stream_out(0, 0);
        static const uint8_t want[8] = { 'W','A','S','M','!', 0xC2, 0xA1, 0x02 };
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 8
              && s_stream_last_len == 8
              && memcmp(s_stream_last, want, 8) == 0,
              "T10 stream_out -> [8] + payload bytes", "");
    }

    /* ---- T11 spi_selftest: loopback stub -> [1] ---- */
    {
        s_spi_xfers = 0;
        uint64_t r = spi_selftest(0, 0);
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 1
              && s_spi_xfers == 1,
              "T11 spi_selftest loopback -> [1]", "");
    }

    /* ---- T12 spi_neg: no_cs=0 raises (abi=1 has no CS bus) ---- */
    {
        EXPECT_TRAP(spi_neg(0, 0), "T12 spi_neg CS-bus refusal");
    }

    /* ---- T13 cam_frame: [320,240,153600] + pattern landing ---- */
    {
        s_camera_on = 1;
        uint64_t r = cam_frame(0, 0);
        int ok = res_ints(r, v, 8, &n) == 0 && n == 3
              && v[0] == 320 && v[1] == 240 && v[2] == 320u * 240u * 2u
              && arena[LIN_FRAME_OFF] == 0xA5
              && arena[LIN_FRAME_OFF + 320u * 240u * 2u - 1] == 0xA5;
        check(ok, "T13 cam_frame -> [320,240,153600] + frame bytes", "");
        s_camera_on = 0;
    }

    /* ---- T14 cam_frame with camera off: the not-on refusal propagates ---- */
    {
        EXPECT_TRAP(cam_frame(0, 0), "T14 cam_frame camera-off refusal");
    }

    /* ---- T15 (fix1) args-slot budget: polling-scale calls stay stable ----
     * The host calls xuansi_alloc on every call-with-args; a bump-only
     * allocator is a hidden ~230-call budget. 1000 cfg-scale add calls
     * must leave the bump high-water untouched (args fit the slot). */
    {
        static const uint8_t args[] = { 0x92, 7, 35 };   /* [7,35] */
        uint32_t hw0 = heap_off;               /* module static: same TU */
        uint64_t r = 0;
        for (int i = 0; i < 1000; i++) {
            uint32_t off = host_write_args(args, sizeof args);
            r = add(off, sizeof args);
        }
        check(res_ints(r, v, 8, &n) == 0 && n == 1 && v[0] == 42
              && heap_off == hw0,
              "T15 1000 args-carrying add calls: result intact, alloc "
              "high-water stable", "");
    }

    printf("---- test_hello: %d checks, %d fail ----\n", checks, fails);
    return fails ? 1 : 0;
}
