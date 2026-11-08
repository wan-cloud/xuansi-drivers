/*
 * jpge.c — B12-P0 interp JPEG-encode calibration module (abi=1).
 *
 * THROWAWAY CALIBRATION ARTIFACT — deliberately NOT in index.json.
 * The B12 camera-migration wave (fw 0.3.0 = camera driver fully moved
 * to wasm) needs the wasm-interp cost of a software JPEG encode before
 * the abi=2 engine is designed; this module vendors the very encoder
 * the current fw uses (esp32-camera 2.1.8 jpge — byte-identical
 * conversion + subsampling + quality semantics to the fw-native
 * frame2jpg path) and measures it through the CURRENT abi=1 host face
 * on the current fw. Its numbers (enc_ms per framesize x quality, the
 * linear-memory budget) feed the later phase decisions; the formal
 * abi=2 module is a later task and does NOT reuse this code beyond
 * the vendored encoder itself.
 *
 * Canonical home: wan-cloud/xuansi-drivers (src/jpge/).
 *
 * Calling face (module_abi=1 marshalling — one msgpack array in, one
 * msgpack result out; see README "Module ABI"):
 *
 *   bench.wasm.call("jpge", "grab")            -- probe the camera
 *   bench.wasm.call("jpge", "encode", {quality})  -- re-encode last grab
 *   bench.wasm.call("jpge", "shot", {quality})     -- the P0 vehicle
 *
 *   cfg (all keys optional; unknown keys ignored — forward compat
 *   mirroring the op face):
 *     quality   int   jpge quality 1..100 (default 80, clamped —
 *                    same clamp as the fw's to_jpg convert_image)
 *
 *   grab()          -> { off, len, w, h }      off = linear offset of
 *                     the RGB565 frame just fetched from the camera
 *   encode{quality} -> { off, len }            off = linear offset of
 *                     the JPEG re-encoded from the LAST grabbed frame
 *                     ({error:"no frame"} before any grab)
 *   shot{quality}   -> { len, w, h, enc_ms }   grab + encode + one
 *                     stream_send (ch=2, host stamps "jpge")
 *
 * shot pipeline: camera_info -> camera_frame_get (one fresh RGB565
 * frame, hi byte first — the GC2145 DVP wire order) -> millis ->
 * jpge_encode_rgb565 (per-scanline RGB565->RGB888 conversion, the
 * to_jpg convert_line_format big-endian path; H2V2; one static MCU
 * line pool) -> millis (enc_ms spans the encode ONLY — the honest
 * interp cost, sensor read excluded) -> SOI/EOI health check (the
 * fw's own xs_camera.c gate: first/last two bytes FFD8/FFD9) ->
 * stream_send. Encoder overflow of the 128KB output buffer, a
 * non-JPEG encoder result, or a short stream_send all surface as
 * {error:...} maps (module-side errors; host refusals — camera off,
 * frame over cap — still raise the host's rt exception).
 *
 * Linear-memory budget (VGA-capable static layout; the P0 readout):
 *   frame RGB565 614,400 + JPEG out 131,072 (the fw fmt2jpg buffer
 *   size) + jpge MCU pool 30,720 (VGA H2V2: 640*3*16) + scanline
 *   1,920 + heap 8,192 + args slot 128 + result 512 + wasm-ld 64KB
 *   stack ~= 852KB -> builds pin --initial-memory=917504 (14 pages;
 *   max stays the frozen 1MB). Everything is static .bss — the
 * per-framesize "high-water" is the layout itself; the one dynamic
 * allocation inside the encoder (the MCU line pool) is carved from a
 * fixed arena with its high-water tracked (jpge_pool_hw()): QVGA
 * 15,360B / VGA 30,720B, asserted natively by test_jpge.c.
 */
#include <stdint.h>
#include <stddef.h>

#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* vendored encoder core (C++; native tests compile it as a second TU
 * with the C++ compiler — see test_jpge.c's build note) */
size_t jpge_encode_rgb565(const unsigned char *src, size_t src_len,
                          int w, int h, int quality,
                          unsigned char *out, size_t out_cap);

/* ---- native-test seam (same discipline as hello.c/ledwatch.c) -----
 * Unit tests compile this file with the host toolchain against a stub
 * host. The test owns a "linear memory" arena and passes u32 offsets
 * exactly like the wasm host does; the buffers the HOST touches
 * (frame buffer, JPEG out buffer, result marshal buffer, args slot)
 * live at fixed arena offsets. Internal working arrays are ordinary
 * statics in both builds. The wasm32 build pre-processes this seam
 * away. */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF    0x4000u             /* result marshal buffer */
#define LIN_ARGS_OFF   0x6000u             /* reused args slot (§3.1) */
#define LIN_FRAME_OFF  0x10000u            /* VGA frame buffer */
#define LIN_JPG_OFF    0xA8000u            /* JPEG output buffer */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)((uintptr_t)(p)))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- in-linear-memory buffers (bump allocator; offset 0 reserved) ----
 * §3.1 args-slot convention (module-ABI.md): the host calls
 * xuansi_alloc on EVERY call-with-args — small args (<= ARGS_SLOT_SZ)
 * reuse one fixed slot (safe: the fw host serializes every wasm call);
 * larger allocations keep the bump arena. */

#define HEAP_SZ 8192
#define ARGS_SLOT_SZ 128
#ifdef XUANSI_NATIVE_TEST
static uint32_t heap_off = 8;
static uint8_t *args_slot;                 /* placed by xuansi_native_init() */
#else
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8), unused));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint32_t heap_off = 8;
#endif

/* ---- geometry caps: QVGA..VGA (the P0 calibration framesizes) ---- */
#define MAX_W 640u
#define MAX_H 480u
#define FRAME_CAP (MAX_W * MAX_H * 2u)     /* 614,400B RGB565 */
#ifdef XUANSI_NATIVE_TEST
static uint8_t *frame_buf;                 /* placed by xuansi_native_init() */
#else
static uint8_t frame_buf[FRAME_CAP] __attribute__((aligned(8)));
#endif

/* JPEG output: 128KB — the same buffer size the fw's fmt2jpg pins for
 * its own path (VGA q80 lands in the tens of KB; overflow fails the
 * SOI/EOI gate honestly rather than truncating). */
#define JPG_CAP (128u * 1024u)
#ifdef XUANSI_NATIVE_TEST
static uint8_t *jpg_buf;                   /* placed by xuansi_native_init() */
#else
static uint8_t jpg_buf[JPG_CAP] __attribute__((aligned(8)));
#endif

#ifdef XUANSI_NATIVE_TEST
static uint8_t *out_buf;                   /* result marshal buffer */
#else
static uint8_t out_buf[512] __attribute__((aligned(8)));
#endif

/* ---- jpge's single allocation: a fixed MCU-line pool --------------
 * jpg_open allocates exactly one block (m_image_bpl_mcu * m_mcu_y =
 * w*3*16 bytes at H2V2: QVGA 15,360B, VGA 30,720B) and frees it at
 * deinit. A fixed arena + high-water mark serves it without any
 * allocator; the high-water is the P0 per-framesize memory readout
 * (asserted natively in test_jpge.c T09). */
#define JPGE_POOL_SZ (MAX_W * 3u * 16u)    /* 30,720B (VGA H2V2 worst) */
static uint8_t jpge_pool[JPGE_POOL_SZ] __attribute__((aligned(8)));
static uint32_t jpge_pool_used;            /* live bytes (0 = free) */
static uint32_t jpge_pool_hwm;             /* high-water mark */

void *jpge_wrap_alloc(size_t n)
{
    if (jpge_pool_used != 0 || n == 0 || n > sizeof jpge_pool) return NULL;
    jpge_pool_used = (uint32_t)n;
    if (jpge_pool_used > jpge_pool_hwm) jpge_pool_hwm = jpge_pool_used;
    return jpge_pool;
}

void jpge_wrap_free(void *p)
{
    (void)p;                                /* single-block pool */
    jpge_pool_used = 0;
}

/* native-test readout hook (the calibration memory number) */
uint32_t jpge_pool_hw(void)
{
    return jpge_pool_hwm;
}

/* no libc: the encoder core's mem needs (byte loops — none of them
 * sit in the per-pixel hot path; memcpy only touches headers/scanline
 * tails) */
void *xs_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *xs_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

/* ---- exported face ---- */

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* §3.1 reuse */
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return off;
}

XUANSI_EXPORT("xuansi_abi")
uint32_t xuansi_abi(void)
{
    return XUANSI_ABI_OVERRIDE;
}

static uint64_t pack(uint32_t ptr, uint32_t len)
{
    return ((uint64_t)ptr << 32) | (uint64_t)len;
}

/* ---- minimal msgpack face (the ledwatch subset) ---- */

static int mp_read_int(const uint8_t *p, uint32_t len, uint32_t *pos,
                       int64_t *out)
{
    if (*pos >= len) return -1;
    uint8_t t = p[(*pos)++];
    if ((t & 0x80) == 0) { *out = t; return 0; }
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }
    uint32_t n = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else return -1;
    if (*pos + n > len) return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = (v << 8) | p[(*pos)++];
    *out = (int64_t)v;
    return 0;
}

static uint32_t mp_put_int(uint8_t *p, uint32_t pos, uint64_t v)
{
    if (v < 128) { p[pos++] = (uint8_t)v; return pos; }
    if (v < 0x10000) {
        p[pos++] = 0xcd; p[pos++] = (uint8_t)(v >> 8); p[pos++] = (uint8_t)v;
        return pos;
    }
    p[pos++] = 0xce;
    p[pos++] = (uint8_t)(v >> 24); p[pos++] = (uint8_t)(v >> 16);
    p[pos++] = (uint8_t)(v >> 8);  p[pos++] = (uint8_t)v;
    return pos;
}

static uint32_t mp_put_str(uint8_t *p, uint32_t pos, const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    p[pos++] = (uint8_t)(0xa0 | n);               /* fixstr (n <= 31) */
    for (uint32_t i = 0; i < n; i++) p[pos++] = (uint8_t)s[i];
    return pos;
}

/* read one fixstr key into buf (<=31 chars); returns length or -1 */
static int rd_fixstr(const uint8_t *p, uint32_t len, uint32_t *pos,
                     char *buf, uint32_t cap)
{
    if (*pos >= len) return -1;
    uint8_t t = p[*pos];
    if ((t & 0xe0) != 0xa0) return -1;
    uint32_t n = t & 0x1f;
    if (*pos + 1 + n > len || n >= cap) return -1;
    (*pos)++;
    for (uint32_t i = 0; i < n; i++) buf[i] = (char)p[(*pos)++];
    buf[n] = 0;
    return (int)n;
}

static int jw_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- cfg: {quality} (int; default 80, clamped 1..100) ---- */

#define DEF_QUALITY 80

static int64_t jw_cfg_quality(const uint8_t *p, uint32_t len)
{
    int64_t q = DEF_QUALITY;
    if (p == NULL || len == 0) return q;
    uint32_t pos = 0;
    uint8_t t;
    if (pos >= len) return q;
    t = p[pos++];
    uint32_t n;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;          /* fixarray */
    else if (t == 0xdc) {                          /* array16 */
        if (pos + 2 > len) return q;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return q;
    if (n < 1) return q;                           /* [] -> default */
    if (pos >= len) return q;
    t = p[pos++];
    uint32_t entries;
    if ((t & 0xf0) == 0x80) entries = t & 0x0f;    /* fixmap */
    else if (t == 0xde) {                          /* map16 */
        if (pos + 2 > len) return q;
        entries = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return q;                               /* not a map -> default */

    char key[32];
    for (uint32_t e = 0; e < entries; e++) {
        int klen = rd_fixstr(p, len, &pos, key, sizeof key);
        if (klen < 0) return q;
        if (pos >= len) return q;
        uint8_t vt = p[pos];
        int64_t iv = 0;
        if (vt == 0xc3 || vt == 0xc2) {            /* bool: ignored */
            pos++;
            continue;
        }
        if (mp_read_int(p, len, &pos, &iv) != 0) return q;
        if (jw_streq(key, "quality")) q = iv;
        /* unknown int keys: ignored */
    }
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    return q;
}

/* ---- result encoders ---- */

static uint32_t put_err(uint8_t *p, uint32_t pos, const char *msg)
{
    p[pos++] = 0x81;                               /* fixmap(1) */
    pos = mp_put_str(p, pos, "error");
    pos = mp_put_str(p, pos, msg);
    return pos;
}

/* ---- module state ---- */

static uint32_t g_flen;        /* last grabbed frame's byte count (0=none) */
static uint32_t g_fw, g_fh;    /* its geometry */
static uint32_t g_cam_info[3]; /* camera_info landing spot: linear .bss
                                * (abi=1 frozen note — wasm stack slots
                                * are on the execution page, out of the
                                * host's write reach) */
static uint32_t g_t0, g_t1;    /* millis landing spots (same note) */

/* encode the current frame_buf at quality q into jpg_buf;
 * returns JPEG length or 0 (encode failure) */
static uint32_t do_encode(int64_t q)
{
    return (uint32_t)jpge_encode_rgb565(frame_buf, g_flen,
                                        (int)g_fw, (int)g_fh, (int)q,
                                        jpg_buf, JPG_CAP);
}

XUANSI_EXPORT("grab")
uint64_t grab(uint32_t ptr, uint32_t len)
{
    (void)jw_cfg_quality(XUANSI_IN(ptr), len);    /* tolerate stray args */
    camera_info(g_cam_info);
    uint32_t w = g_cam_info[0], h = g_cam_info[1];
    uint32_t flen = 0;
    camera_frame_get(&flen, XUANSI_OFF(frame_buf), FRAME_CAP);

    uint32_t pos = 0;
    if (w == 0 || h == 0 || w > MAX_W || h > MAX_H || flen != w * h * 2u) {
        /* unreachable on the frozen host face (frame_get raises first);
         * defensive: honest error map */
        pos = put_err(out_buf, pos, "bad frame geometry");
        return pack(XUANSI_OFF(out_buf), pos);
    }
    g_flen = flen;
    g_fw = w;
    g_fh = h;

    out_buf[pos++] = 0x84;                         /* fixmap(4) */
    pos = mp_put_str(out_buf, pos, "off");
    pos = mp_put_int(out_buf, pos, XUANSI_OFF(frame_buf));
    pos = mp_put_str(out_buf, pos, "len");
    pos = mp_put_int(out_buf, pos, flen);
    pos = mp_put_str(out_buf, pos, "w");
    pos = mp_put_int(out_buf, pos, w);
    pos = mp_put_str(out_buf, pos, "h");
    pos = mp_put_int(out_buf, pos, h);
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("encode")
uint64_t encode(uint32_t ptr, uint32_t len)
{
    int64_t q = jw_cfg_quality(XUANSI_IN(ptr), len);
    uint32_t pos = 0;
    if (g_flen == 0) {
        pos = put_err(out_buf, pos, "no frame (grab first)");
        return pack(XUANSI_OFF(out_buf), pos);
    }
    uint32_t jlen = do_encode(q);
    if (jlen == 0) {
        pos = put_err(out_buf, pos, "encode failed (output overflow?)");
        return pack(XUANSI_OFF(out_buf), pos);
    }
    out_buf[pos++] = 0x82;                         /* fixmap(2) */
    pos = mp_put_str(out_buf, pos, "off");
    pos = mp_put_int(out_buf, pos, XUANSI_OFF(jpg_buf));
    pos = mp_put_str(out_buf, pos, "len");
    pos = mp_put_int(out_buf, pos, jlen);
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("shot")
uint64_t shot(uint32_t ptr, uint32_t len)
{
    int64_t q = jw_cfg_quality(XUANSI_IN(ptr), len);
    camera_info(g_cam_info);
    uint32_t w = g_cam_info[0], h = g_cam_info[1];
    uint32_t flen = 0;
    camera_frame_get(&flen, XUANSI_OFF(frame_buf), FRAME_CAP);

    uint32_t pos = 0;
    if (w == 0 || h == 0 || w > MAX_W || h > MAX_H || flen != w * h * 2u) {
        pos = put_err(out_buf, pos, "bad frame geometry");
        return pack(XUANSI_OFF(out_buf), pos);
    }
    g_flen = flen;
    g_fw = w;
    g_fh = h;

    millis(&g_t0);
    uint32_t jlen = do_encode(q);
    millis(&g_t1);
    uint32_t enc_ms = g_t1 - g_t0;

    /* SOI/EOI health check — the fw's own gate (xs_camera.c): a
     * zero return or a bad marker pair must not reach the stream. */
    if (jlen < 4 || jpg_buf[0] != 0xFF || jpg_buf[1] != 0xD8
        || jpg_buf[jlen - 2] != 0xFF || jpg_buf[jlen - 1] != 0xD9) {
        pos = put_err(out_buf, pos,
                      "encoder output not JPEG (SOI/EOI absent)");
        return pack(XUANSI_OFF(out_buf), pos);
    }

    uint32_t sent = 0;
    stream_send(&sent, XUANSI_OFF(jpg_buf), jlen);
    if (sent != jlen) {
        /* honest numbers alongside the error (enc_ms survives the
         * stream failure — the P0 calibration table needs it). The
         * board-side cause (B12-P0 finding): fw n_stream_send's
         * pay=malloc(len) — sub-16KB mallocs are internal-only
         * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384) and the
         * post-camera internal heap can't reliably serve them; ≥16KB
         * payloads go PSRAM-eligible and stream fine. */
        out_buf[pos++] = 0x86;                     /* fixmap(6) */
        pos = mp_put_str(out_buf, pos, "error");
        pos = mp_put_str(out_buf, pos, "stream_send truncated");
        pos = mp_put_str(out_buf, pos, "len");
        pos = mp_put_int(out_buf, pos, jlen);
        pos = mp_put_str(out_buf, pos, "sent");
        pos = mp_put_int(out_buf, pos, sent);
        pos = mp_put_str(out_buf, pos, "w");
        pos = mp_put_int(out_buf, pos, w);
        pos = mp_put_str(out_buf, pos, "h");
        pos = mp_put_int(out_buf, pos, h);
        pos = mp_put_str(out_buf, pos, "enc_ms");
        pos = mp_put_int(out_buf, pos, enc_ms);
        return pack(XUANSI_OFF(out_buf), pos);
    }

    out_buf[pos++] = 0x84;                         /* fixmap(4) */
    pos = mp_put_str(out_buf, pos, "len");
    pos = mp_put_int(out_buf, pos, jlen);
    pos = mp_put_str(out_buf, pos, "w");
    pos = mp_put_int(out_buf, pos, w);
    pos = mp_put_str(out_buf, pos, "h");
    pos = mp_put_int(out_buf, pos, h);
    pos = mp_put_str(out_buf, pos, "enc_ms");
    pos = mp_put_int(out_buf, pos, enc_ms);
    return pack(XUANSI_OFF(out_buf), pos);
}

/* ---- native seam init: place the host-touched buffers in the arena ---- */
#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    frame_buf = base + LIN_FRAME_OFF;
    jpg_buf = base + LIN_JPG_OFF;
}
#endif
