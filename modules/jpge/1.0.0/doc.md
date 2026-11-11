<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# jpge — camera frames → JPEG or raw RGB565 out the stream (the first codec module)

kind: codec
release: production
version: 1.0.0
abi: 2

The camera-path codec: frames in (camera), encoded or raw bytes out
(stream ch=2). It vendors the exact JPEG encoder the board's native
path used (esp32-camera 2.1.8 jpge; per-scanline RGB565→RGB888
conversion, H2V2, the `frame2jpg` quality semantics) behind the abi=2
host face — `cam_frame_get` hands the geometry out with the frame,
`stream_send` (host-stamped `"jpge"`, ch=2) is the egress:

```lua
bench.wasm.call("jpge", "grab")                 -- {off,len,w,h}
bench.wasm.call("jpge", "encode", {quality=80}) -- {off,len} (last grab)
bench.wasm.call("jpge", "shot",   {quality=80}) -- {len,w,h,enc_ms} + one ch=2 JPEG frame
bench.wasm.call("jpge", "stream_raw")           -- {len,w,h} + one ch=2 raw frame
```

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| grab | [cfg?] | {off, len, w, h} \| {error} | one cam_frame_get into the module's frame slot (camera must be on — caller's responsibility, gc2145's stream); no frame → {error='no frame'} |
| encode | [{quality=i:1..100}?] | {off, len} \| {error} | encode the LAST grab in place; encoder overflow / non-JPEG output → {error} maps |
| shot | [{quality=i:1..100}?] | {len, w, h, enc_ms} + one ch=2 JPEG frame \| {error,...} | grab + encode (enc_ms spans the encode only) + SOI/EOI gate + one stream_send |
| stream_raw | [] | {len, w, h} + one ch=2 raw RGB565 frame \| {error,...} | grab + one stream_send of the RAW frame (len = w*h*2) — the generic CV egress, no JPEG |

The host chunks payloads at 65,526 − 5B (the name stamp) = 65,521B;
VGA JPEGs and `stream_raw` VGA (614,400B = 10 chunks) are first-class
shapes, natively tested against a chunk-emulating sink. A TX-full/OOM
mid-stream truncation answers the honest numbers
`{error, len, sent, w, h[, enc_ms]}` — exactly how much of the frame
survived the wire. Host refusals (camera off, frame over cap) raise
`rt` as usual.

## Measured

- `wasm_bytes` — 28642 — deliverable .wasm (interp lane)
- `aot_bytes` — 83524 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 917504 — 14 pages: VGA frame 614.4KB + JPEG out 128KB + encoder MCU pool 30.7KB + scanline 1.9KB + heap/args/out + the 64KB stack (static VGA-capable layout; the one dynamic allocation — the MCU line pool — is a fixed arena with its high-water tracked)
- `aot_text_bytes` — 76416 — surgery TEXT (PSRAM exec); board-verified info() trio
- `aot_pool_bytes` — 1708 — xuansi.literal split pool (board-verified)
- `mcu_pool_highwater_qvga_bytes` — 15360 — asserted natively (test_jpge.c T09)
- `mcu_pool_highwater_vga_bytes` — 30720 — asserted natively
- `interp_vga_q80_ms` — over 15000 — the VM-task watchdog kills a single wasm call > 5s: VGA interp encodes WILL trip it (measured ~15s+); interp-safe class = QVGA q<=60 (~3.1-3.9s)
- `aot_vga_q80_ms` — 1229 — surgery-.aot lane median, n=3, 2026-11-11 P3 board run (q60: 1179ms) — the recipe's VGA shape rides this lane
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve); the ABI 1 freeze pins initial==max on the W3 rebuild

## Notes

- Source layout differs from the pure-C modules by exactly one thing:
  `src/jpge/jpge.cpp` (the vendored C++ encoder core,
  `-fno-rtti -fno-exceptions` — the one sanctioned C-flag-set
  deviation, see build.sh) linked beside the C wrapper; the native
  test is a two-TU build.
- Imports: `xuansi.{cam_frame_get, millis, stream_send}` plus
  `env.{memcpy,memset}` (WAMR libc-builtin).
- `shot` deliberately stays one call; the resolution for interp-lane
  VGA is the AOT track, not slice APIs.
- The 0.1.0 calibration artifact (never indexed) proved the cost
  model; this is its formal successor.
