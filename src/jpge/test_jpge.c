/*
 * test_jpge.c — native unit test for the jpge calibration module
 * (stub host). B12-P0.
 *
 * Build & run (host toolchain; the vendored encoder core is C++ so the
 * native seam is two TUs — the ONE place this deviates from the
 * single-cc line the other modules use, documented in the module
 * header and ci.yml):
 *   cc  -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/jpge \
 *       src/jpge/test_jpge.c -o build/test_jpge.o -c
 *   c++ -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/jpge \
 *       src/jpge/jpge.cpp -o build/test_jpge_cpp.o -c
 *   c++ build/test_jpge.o build/test_jpge_cpp.o -o build/test_jpge \
 *       && ./build/test_jpge
 *
 * Same seam as test_ledwatch.c: the test includes jpge.c directly (one
 * C translation unit), provides the `xuansi` host namespace as stubs,
 * and drives grab/encode/shot through the same u32-offset msgpack
 * marshalling the wasm host uses — the test owns the "linear memory"
 * arena via xuansi_native_init().
 *
 * The stub camera serves SYNTHETIC RGB565 frames (gradient + colored
 * rects) at a switchable geometry (QVGA and VGA — the P0 calibration
 * framesizes), counts camera_frame_get calls, and raises exactly like
 * the real host when the camera is off. The stub stream sink captures
 * what shot() stream_sends so the wire bytes (not just the module's
 * own buffer) are asserted: SOI/EOI, SOF geometry, determinism and
 * quality monotonicity all run against the captured JPEG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "jpge.c"     /* unity build: the module under test (C TU) */

/* RED seam: these arrive with the module; fallbacks keep this file
 * compilable against a stub so the linker failure names the missing
 * module symbols (the RED evidence is the missing jpge.c itself). */

/* ---- stub host state ---- */

static int s_camera_on = 1;
static uint32_t s_w = 320, s_h = 240;         /* switchable geometry */
static uint32_t s_frame_calls;
static uint32_t s_ms;                          /* fake clock (+1/call) */

/* staged frame (served on every camera_frame_get; last committed) */
#define MAXW 640u
#define MAXH 480u
static uint8_t s_canvas[MAXW * MAXH * 2u];

/* stream sink: captures shot()'s stream_send bytes */
static uint8_t s_sink[160 * 1024];
static uint32_t s_sink_len, s_sink_calls;

/* trap emulation: the host refusal channel (same as test_hello/ledwatch) */
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

/* ---- the `xuansi` namespace stubs (only what jpge imports) ---- */

void xuansi_millis(uint32_t *out)
{
    s_ms += 1;                    /* deterministic: every read advances */
    *out = s_ms;
}

void xuansi_camera_frame_get(uint32_t *out, uint32_t buf_off, uint32_t cap)
{
    if (!s_camera_on) stub_raise("xuansi.camera_frame_get: camera not on");
    uint32_t len = s_w * s_h * 2u;
    if (len > cap) stub_raise("xuansi.camera_frame_get: frame over cap");
    memcpy(xuansi_lin_base + buf_off, s_canvas, len);
    s_frame_calls++;
    *out = len;
}

void xuansi_camera_info(uint32_t *out)
{
    out[0] = s_camera_on ? s_w : 0u;
    out[1] = s_camera_on ? s_h : 0u;
    out[2] = (s_w == 640u) ? 1u : 0u;   /* fs_code 1=VGA 0=QVGA */
}

void xuansi_stream_send(uint32_t *out, uint32_t buf_off, uint32_t len)
{
    if (len > sizeof s_sink - s_sink_len) {
        stub_raise("xuansi.stream_send: sink overflow (test harness)");
    }
    memcpy(s_sink + s_sink_len, xuansi_lin_base + buf_off, len);
    s_sink_len += len;
    s_sink_calls++;
    *out = len;
}

/* ---- synthetic canvas -> staged frame ----------------------------
 * RGB565 pixel layout as the module decodes it: hi byte first
 * (fb[2i] = R4..G3, fb[2i+1] = G2..B0) — the GC2145 DVP wire order,
 * identical to the fw's to_jpg rgb565_big_endian=true path. */

static void canvas_px(uint32_t x, uint32_t y, uint16_t c)
{
    if (x >= s_w || y >= s_h) return;
    uint32_t i = (y * s_w + x) * 2u;
    s_canvas[i] = (uint8_t)(c >> 8);
    s_canvas[i + 1] = (uint8_t)(c & 0xFF);
}

/* gradient background + colored rects: the brief's synthetic pattern */
static void canvas_pattern(void)
{
    for (uint32_t y = 0; y < s_h; y++)
        for (uint32_t x = 0; x < s_w; x++)
            canvas_px(x, y, (uint16_t)(((x * 31u / (s_w - 1)) << 11)
                                     | ((y * 63u / (s_h - 1)) << 5)
                                     | (((x + y) * 31u / (s_w + s_h)))));
    /* three solid color rects (scaled to geometry) */
    uint32_t rw = s_w / 8, rh = s_h / 6;
#define RECT(c, x0, y0) do { \
        for (uint32_t y = (y0); y < (y0) + rh; y++) \
            for (uint32_t x = (x0); x < (x0) + rw; x++) canvas_px(x, y, (c)); \
    } while (0)
    RECT(0xF800u, s_w / 8,     s_h / 6);      /* red   */
    RECT(0x07E0u, s_w / 2,     s_h / 6);      /* green */
    RECT(0x001Fu, s_w * 3 / 8, s_h / 2);      /* blue  */
#undef RECT
}

static void canvas_solid(uint16_t c)
{
    for (uint32_t y = 0; y < s_h; y++)
        for (uint32_t x = 0; x < s_w; x++) canvas_px(x, y, c);
}

static void set_geom(uint32_t w, uint32_t h)
{
    s_w = w;
    s_h = h;
}

static void sink_reset(void)
{
    s_sink_len = 0;
    s_sink_calls = 0;
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

/* ---- msgpack result decoding (independent of the module's encoder) */

static const uint8_t *s_res_p;
static uint32_t s_res_len, s_res_pos;

static int rd_u8(uint8_t *out)
{
    if (s_res_pos >= s_res_len) return -1;
    *out = s_res_p[s_res_pos++];
    return 0;
}

static int rd_i64(int64_t *out)   /* fixint + u8/u16/u32 family */
{
    uint8_t t;
    if (rd_u8(&t) != 0) return -1;
    if ((t & 0x80) == 0) { *out = t; return 0; }
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }
    uint32_t n = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b;
        if (rd_u8(&b) != 0) return -1;
        v = (v << 8) | b;
    }
    *out = (int64_t)v;
    return 0;
}

/* fixstr key helper removed: res_decode reads keys directly (the old
 * try-each-key restore dance is unneeded — keys arrive in order). */

typedef struct {
    int64_t off, len, w, h, enc_ms;
    char error[64];
    int has_error;
} shot_res_t;

/* decode a flat fixmap of {off,len,w,h,enc_ms,error} — the module's
 * whole result face (int values + one optional fixstr error) */
static int res_decode(uint64_t packed, shot_res_t *res)
{
    memset(res, 0, sizeof *res);
    s_res_p = arena + (uint32_t)(packed >> 32);
    s_res_len = (uint32_t)packed;
    s_res_pos = 0;
    uint8_t t;
    if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80) return -1;   /* fixmap */
    uint32_t entries = t & 0x0f;
    for (uint32_t e = 0; e < entries; e++) {
        /* key is always a fixstr; read it into buf */
        uint8_t kt;
        if (rd_u8(&kt) != 0 || (kt & 0xe0) != 0xa0) return -1;
        uint32_t kn = kt & 0x1f;
        char key[32];
        if (kn >= sizeof key || s_res_pos + kn > s_res_len) return -1;
        memcpy(key, s_res_p + s_res_pos, kn);
        key[kn] = 0;
        s_res_pos += kn;
        if (strcmp(key, "error") == 0) {
            uint8_t st;
            if (rd_u8(&st) != 0 || (st & 0xe0) != 0xa0) return -1;
            uint32_t sn = st & 0x1f;
            if (sn >= sizeof res->error || s_res_pos + sn > s_res_len)
                return -1;
            memcpy(res->error, s_res_p + s_res_pos, sn);
            res->error[sn] = 0;
            res->has_error = 1;
            continue;
        }
        static const char *ikeys[5] = { "off", "len", "w", "h", "enc_ms" };
        int64_t *dst[5] = { &res->off, &res->len, &res->w, &res->h,
                            &res->enc_ms };
        int matched = -1;
        for (int k = 0; k < 5; k++)
            if (strcmp(key, ikeys[k]) == 0) { matched = k; break; }
        if (matched < 0) return -1;                        /* unknown key */
        int64_t v;
        if (rd_i64(&v) != 0) return -1;
        *dst[matched] = v;
    }
    return 0;
}

/* ---- JPEG byte-level checks (C port of camera-hil.py jpeg_sof_dim) ---- */

static int jpeg_sof_dim(const uint8_t *jpg, uint32_t n, uint32_t *h, uint32_t *w)
{
    if (n < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return -1;
    uint32_t i = 2;
    while (i + 4 <= n) {
        if (jpg[i] != 0xFF) return -1;
        uint8_t m = jpg[i + 1];
        if (m == 0xD9 || m == 0xDA) return -1;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (i + 9 > n) return -1;
            *h = ((uint32_t)jpg[i + 5] << 8) | jpg[i + 6];
            *w = ((uint32_t)jpg[i + 7] << 8) | jpg[i + 8];
            return 0;
        }
        uint32_t seglen = ((uint32_t)jpg[i + 2] << 8) | jpg[i + 3];
        if (seglen < 2) return -1;
        i += 2 + seglen;
    }
    return -1;
}

static int jpeg_soi_eoi(const uint8_t *jpg, uint32_t n)
{
    return n >= 4 && jpg[0] == 0xFF && jpg[1] == 0xD8
        && jpg[n - 2] == 0xFF && jpg[n - 1] == 0xD9;
}

/* the wasm host's arg-writing step: alloc, then write msgpack bytes */
static uint32_t host_write_args(const uint8_t *bytes, uint32_t n)
{
    uint32_t off = xuansi_alloc(n);
    if (off == 0) { fprintf(stderr, "FATAL: alloc refused\n"); exit(2); }
    memcpy(arena + off, bytes, n);
    return off;
}

/* cfg msgpack builder (test_ledwatch.c's compact spec, int keys only):
 *   "quality=i:80"  or  "" for no args at all */
static uint32_t build_args(uint8_t *buf, const char *kvs)
{
    uint32_t pos = 0, nkv = 0;
    const char *p = kvs;
    while (*p) { nkv++; while (*p && *p != ',') p++; if (*p) p++; }
    if (nkv > 15) { fprintf(stderr, "FATAL: cfg nkv\n"); exit(2); }
    buf[pos++] = 0x91;                              /* fixarray(1) */
    buf[pos++] = (uint8_t)(0x80 | nkv);             /* fixmap(nkv) */
    p = kvs;
    for (uint32_t i = 0; i < nkv; i++) {
        const char *eq = strchr(p, '=');
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        if (!eq || !colon || eq > colon || (comma && colon > comma)) {
            fprintf(stderr, "FATAL: cfg spec syntax\n"); exit(2);
        }
        size_t klen = (size_t)(eq - p);
        const char *vend = comma ? comma : (p + strlen(p));
        size_t vlen = (size_t)(vend - colon - 1);
        char val[32];
        if (klen > 31 || vlen >= sizeof val) {
            fprintf(stderr, "FATAL: cfg len\n"); exit(2);
        }
        buf[pos++] = (uint8_t)(0xa0 | klen);
        memcpy(buf + pos, p, klen); pos += (uint32_t)klen;
        memcpy(val, colon + 1, vlen); val[vlen] = 0;
        long iv = strtol(val, NULL, 10);
        if (iv >= 0 && iv < 128) buf[pos++] = (uint8_t)iv;
        else if (iv < 0x100) { buf[pos++] = 0xcc; buf[pos++] = (uint8_t)iv; }
        else if (iv < 0x10000) {
            buf[pos++] = 0xcd;
            buf[pos++] = (uint8_t)(iv >> 8); buf[pos++] = (uint8_t)iv;
        } else {
            buf[pos++] = 0xce;
            buf[pos++] = (uint8_t)(iv >> 24); buf[pos++] = (uint8_t)(iv >> 16);
            buf[pos++] = (uint8_t)(iv >> 8);  buf[pos++] = (uint8_t)iv;
        }
        p = comma ? comma + 1 : p + strlen(p);
    }
    return pos;
}

typedef uint64_t (*mod_fn_t)(uint32_t, uint32_t);

static uint64_t call_mod(mod_fn_t fn, const char *kvs)
{
    uint8_t abuf[128];
    uint32_t n = 0, off = 0;
    if (kvs && kvs[0]) {
        n = build_args(abuf, kvs);
        off = host_write_args(abuf, n);
    }
    return fn(off, n);
}

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

static void reset_state(void)
{
    s_camera_on = 1;
    s_frame_calls = 0;
    s_ms = 0;
    memset(arena, 0, sizeof arena);
    xuansi_native_init(arena);
    sink_reset();
}

int main(void)
{
    shot_res_t res;
    char detail[256];

    reset_state();
    set_geom(320, 240);
    canvas_pattern();

    /* ---- T01 abi + allocator ---- */
    check(xuansi_abi() == 1, "T01 xuansi_abi()==1", "");
    check(xuansi_alloc(0) == 0, "T01b alloc(0) refused", "");

    /* ---- T02 shot q80: full face + wire bytes (QVGA) ---- */
    {
        sink_reset();
        uint64_t r = call_mod(shot, "quality=i:80");
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail,
                 "dec=%d err=%s len=%ld w=%ld h=%ld enc_ms=%ld sink=%u/%u",
                 dec, res.error, (long)res.len, (long)res.w, (long)res.h,
                 (long)res.enc_ms, s_sink_len, s_sink_calls);
        uint32_t sh = 0, sw = 0;
        int sof = jpeg_sof_dim(s_sink, s_sink_len, &sh, &sw);
        int ok = dec == 0 && !res.has_error
          && res.len > 0 && res.w == 320 && res.h == 240 && res.enc_ms >= 1
          && s_sink_calls == 1 && s_sink_len == (uint32_t)res.len
          && jpeg_soi_eoi(s_sink, s_sink_len)
          && sof == 0 && sh == 240 && sw == 320;
        check(ok, "T02 shot{q80} QVGA: face + SOI/EOI + SOF 320x240 on wire",
              detail);
    }

    /* ---- T03 determinism: two shots, same input -> byte-equal ---- */
    {
        sink_reset();
        call_mod(shot, "quality=i:80");
        uint8_t a[160 * 1024];
        uint32_t alen = s_sink_len;
        memcpy(a, s_sink, alen);
        sink_reset();
        call_mod(shot, "quality=i:80");
        int ok = alen == s_sink_len && memcmp(a, s_sink, alen) == 0;
        snprintf(detail, sizeof detail, "len %u vs %u", alen, s_sink_len);
        check(ok, "T03 two encodes of same input byte-equal", detail);
    }

    /* ---- T04 quality monotonicity: q60 bytes <= q90 bytes ---- */
    {
        sink_reset();
        call_mod(shot, "quality=i:60");
        uint32_t l60 = s_sink_len;
        sink_reset();
        call_mod(shot, "quality=i:90");
        uint32_t l90 = s_sink_len;
        snprintf(detail, sizeof detail, "q60=%uB q90=%uB", l60, l90);
        check(l60 > 0 && l90 > 0 && l60 <= l90, "T04 q60 <= q90 byte count",
              detail);
    }

    /* ---- T05 grab face ---- */
    {
        uint64_t r = call_mod(grab, "");
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "dec=%d off=%ld len=%ld w=%ld h=%ld",
                 dec, (long)res.off, (long)res.len, (long)res.w, (long)res.h);
        check(dec == 0 && !res.has_error && res.len == 320L * 240L * 2L
              && res.w == 320 && res.h == 240 && res.off == LIN_FRAME_OFF,
              "T05 grab -> {off,len,w,h} (off==frame linear offset)", detail);
        check(s_frame_calls >= 1, "T05b grab consumed a frame", "");
    }

    /* ---- T06 encode face (on grabbed frame; and the no-frame error) */
    {
        uint64_t r = call_mod(encode, "quality=i:80");
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "dec=%d off=%ld len=%ld",
                 dec, (long)res.off, (long)res.len);
        check(dec == 0 && !res.has_error && res.len > 0
              && res.off == LIN_JPG_OFF,
              "T06 encode{q80} on grabbed frame -> {off,len}", detail);

        reset_state();                       /* clears the arena, NOT module
                                               * statics — g_flen is module
                                               * state, clear it explicitly */
        g_flen = 0;                          /* no frame grabbed yet */
        canvas_pattern();
        r = call_mod(encode, "quality=i:80");
        dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "dec=%d err=%s", dec, res.error);
        check(dec == 0 && res.has_error && strstr(res.error, "no frame"),
              "T06b encode before any grab -> error map", detail);
    }

    /* ---- T07 quality domain clamp + unknown-key tolerance ---- */
    {
        call_mod(grab, "");
        uint64_t r0 = call_mod(encode, "quality=i:0");      /* -> clamp 1 */
        uint64_t r200 = call_mod(encode, "quality=i:200");  /* -> clamp 100 */
        uint64_t rbogus = call_mod(encode, "bogus=i:7");
        shot_res_t r0r, r200r, rbr;
        int d0 = res_decode(r0, &r0r), d200 = res_decode(r200, &r200r);
        int db = res_decode(rbogus, &rbr);
        snprintf(detail, sizeof detail, "q0=%ld q200=%ld bogus=%ld(dec %d/%d/%d)",
                 (long)r0r.len, (long)r200r.len, (long)rbr.len, d0, d200, db);
        check(d0 == 0 && !r0r.has_error && r0r.len > 0
              && d200 == 0 && !r200r.has_error && r200r.len > 0
              && db == 0 && !rbr.has_error && rbr.len > 0,
              "T07 quality clamped into 1..100; unknown cfg key ignored",
              detail);
    }

    /* ---- T08 camera off: host refusal propagates ---- */
    {
        s_camera_on = 0;
        EXPECT_TRAP(call_mod(shot, "quality=i:80"),
                    "T08 camera-off refusal propagates");
        EXPECT_TRAP(call_mod(grab, ""), "T08b camera-off grab refusal");
        s_camera_on = 1;
    }

    /* ---- T09 memory high-water per framesize (the P0 number) ---- */
    {
        uint32_t hw0 = jpge_pool_hw();
        call_mod(grab, "");
        call_mod(encode, "quality=i:80");
        uint32_t hw_qvga = jpge_pool_hw();
        snprintf(detail, sizeof detail, "QVGA mcu alloc high-water=%u (want %u)",
                 hw_qvga, 320u * 3u * 16u);
        check(hw_qvga == 320u * 3u * 16u,
              "T09 QVGA jpge mcu pool high-water == 15,360B", detail);

        set_geom(640, 480);                  /* VGA geometry */
        canvas_pattern();
        sink_reset();
        uint64_t r = call_mod(shot, "quality=i:80");
        int dec = res_decode(r, &res);
        uint32_t hw_vga = jpge_pool_hw();
        uint32_t sh = 0, sw = 0;
        int sof = jpeg_sof_dim(s_sink, s_sink_len, &sh, &sw);
        snprintf(detail, sizeof detail,
                 "dec=%d len=%ld w=%ld h=%ld hw=%u (want %u) sof=%d %ux%u",
                 dec, (long)res.len, (long)res.w, (long)res.h, hw_vga,
                 640u * 3u * 16u, sof, sw, sh);
        check(dec == 0 && !res.has_error && res.len > 0 && res.w == 640
              && res.h == 480 && s_sink_len == (uint32_t)res.len
              && jpeg_soi_eoi(s_sink, s_sink_len) && sof == 0
              && sh == 480 && sw == 640
              && hw_vga == 640u * 3u * 16u,
              "T09b shot VGA green + mcu pool high-water == 30,720B", detail);
        set_geom(320, 240);
        canvas_pattern();
        (void)hw0;
    }

    /* ---- T10 uniform frame compresses tiny (content sanity) ---- */
    {
        canvas_solid(0xFFFF);                /* all white */
        sink_reset();
        call_mod(shot, "quality=i:80");
        snprintf(detail, sizeof detail, "white QVGA q80 -> %uB", s_sink_len);
        check(s_sink_len > 0 && s_sink_len < 4096,
              "T10 all-white QVGA q80 encodes < 4KB", detail);
        canvas_pattern();
    }

    /* ---- T11 args-slot budget: repeated cfg'd calls, stable heap ---- */
    {
        canvas_pattern();
        uint32_t hw0 = heap_off;
        uint64_t r = 0;
        for (int i = 0; i < 1000; i++) {
            sink_reset();                    /* sink holds ONE shot's wire */
            r = call_mod(shot, "quality=i:80");
        }
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "hw %u->%u dec=%d len=%ld",
                 hw0, heap_off, dec, (long)res.len);
        check(dec == 0 && !res.has_error && res.len > 0 && heap_off == hw0,
              "T11 1000 cfg'd shot calls: alloc high-water stable", detail);
    }

    printf("---- test_jpge: %d checks, %d fail ----\n", checks, fails);

    /* ground-truth dump (B12-P0 pixel-level verification): the module-
     * encoded JPEG of the synthetic pattern, for host-side PIL pixel
     * checks (see tools/b12-p1-hil evidence). Rects at QVGA: red at
     * (40,30)+80x40, green at (160,30)+80x40, blue at (120,120)+80x40
     * on the gradient. */
    if (getenv("JPGE_DUMP")) {
        FILE *f = fopen(getenv("JPGE_DUMP"), "wb");
        if (f != NULL) {
            set_geom(320, 240);
            canvas_pattern();
            sink_reset();
            call_mod(shot, "quality=i:80");
            fwrite(s_sink, 1, s_sink_len, f);
            fclose(f);
            printf("dumped %uB -> %s\n", s_sink_len, getenv("JPGE_DUMP"));
        }
    }
    return fails ? 1 : 0;
}
