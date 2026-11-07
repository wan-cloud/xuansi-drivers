# xuansi-drivers

Public library of **WASM driver modules** for the xuansi bench (an
ESP32-S3 hardware probe driven by an agent over the
[xuansi-mcp](https://github.com/wan-cloud/xuansi-mcp) daemon). The
firmware hot-loads these modules onto the board's WAMR runtime, where
they drive GPIO/SPI/camera hardware through a small frozen host API.

Current contents: `hello@1.0.0` — the ABI's reference module (full
host-surface tour: echo/add marshalling, GPIO, timing, stream, SPI
loopback, camera frame) — `ledwatch@1.0.0` — the ABI's first
`algorithm` module (camera frame → LED on/off region detection; see
"ledwatch" below) — and `st7789@1.0.0` — the ABI's first `driver`
module (ST7789V 240x240 TFT panel over CS-less SPI; see "st7789"
below).

## Module ABI (abi = 1, frozen)

A module is a `wasm32` build (wasi-sdk, no WASI imports) that:

- exports `xuansi_abi() -> u32` returning `1` (the loader calls it at
  instantiate; any other value is refused), and
- exports `xuansi_alloc(u32 n) -> u32` handing out linear-memory
  offsets (0 = refusal), and
- exports its entry functions as `fn(ptr: u32, len: u32) -> u64`:
  arguments arrive as **one msgpack array** written at `ptr`; the
  return is a single packed `u64 = (result_ptr << 32) | result_len`
  pointing at the module's own msgpack result (C has no multi-value
  returns — the pair rides in one i64).
- imports host functions only from the `xuansi` namespace (9 symbols —
  see `src/xuansi_host.h`, the per-symbol template). Every host
  function is `void`: results are written through out-parameters into
  the module's linear memory; errors surface as wasm exceptions.

Limits (frozen): args/result msgpack ≤ 8192B, container depth ≤ 8,
module file ≤ 256KB, module memory 256KB initial / 1MB max.

Adding symbols or changing semantics means `abi = 2` — existing symbol
semantics never move.

## Delivery track: `.wasm` bytecode

The deliverable artifact is the **interpreter bytecode** (`.wasm`),
listed in `index.json` and deployed to `/drv/<name>.wasm` on the board.
`.aot` files are still built by CI (wamrc, hash-recorded) as
**auxiliary** artifacts — future-ready, but the current board stack
honestly refuses to *execute* them (an upstream AOT-in-PSRAM execution
defect on ESP32-S3; when it is fixed upstream, the `.aot` track turns
back on with zero module-side changes). The `.aot` is therefore never
listed as a deployable file in `index.json`.

## ledwatch (algorithm module)

`ledwatch@1.0.0` turns QVGA RGB565 camera frames into LED on/off
regions — the analyze() output face is consumed verbatim by the
bench's display and closed-loop tasks:

```lua
bench.wasm.call("ledwatch", "analyze", cfg)
-- cfg (all optional, unknown keys ignored):
--   threshold      0..255, default 128  (lit = luma >= threshold)
--   min_area       default 16            (smaller regions dropped)
--   max_regions    1..32, default 8      (result cap)
--   discard_first  default true          (consume one frame first —
--                                         unconverged AWB after camera-on;
--                                         stateless: true = exactly two
--                                         camera_frame_get calls per analyze)
-- returns:
--   { regions = { {x,y,w,h,area,luma_mean,r_mean,g_mean,b_mean}, ... },
--     frame   = { w = 320, h = 240 } }
```

Regions are sorted by area (largest first) and capped at
`max_regions`. All means are integers 0..255 (the abi=1 value bridge
carries no floats): RGB565 components replicated into 8 bits, means
rounded to nearest, luma = BT.601 integer approximation
`(77R + 150G + 29B) >> 8` (pure red is luma 76 — pass a lower
threshold when hunting red-only LEDs). The camera must already be on
at QVGA (caller's responsibility, same contract as hello's
`cam_frame`); camera-off or larger framesizes surface as the host's
`rt` exception.

Pipeline: luma → threshold → two-pass connected-component labelling
(4-connectivity, union-find with path compression, u16 label map in
linear memory) → per-label area → top-N selection → stats pass
(bbox + means). The u16 label space is overflow-proof at QVGA: labels
are only created by lit pixels whose left/up neighbours are dark (a
4-independent set, at most w·h/2 = 38,400 < 65,535) — the
checkerboard vector pins the worst case.

ledwatch pins `--initial-memory=720896` (11 pages): its CCL working
set (~627KB: frame + label map + per-label arrays) plus the wasm
stack. One ledwatch + one hello co-load fit the board's 1MB WAMR
pool; two ledwatch instances do not (the loader refuses with an
honest out-of-memory error).

## st7789 (driver module)

`st7789@1.0.0` drives the bench's ST7789V 240x240 TFT panel — the
first `driver` module: Lua asks, the module drives real hardware
(SPI + GPIO) through the frozen host table. Board facts compiled in:
SCL=41, SDA(MOSI)=42, RES=47, DC=21, backlight tied to 3V3, **no CS**
(the bus is dedicated to the panel — exactly the abi=1 CS-less
engine, `no_cs=1`). MISO is passed as **pin 14, legal but unwired**:
the v1 engine's `pins_valid` wants 0..47 on all three bus pins, so the
frozen doc's "miso=-1 = absent" convention is not actually accepted
(`rc=-3`, hit and recorded by B10-T6's on-board conflict vector);
tx-only transfers never read MISO. (Fix-round-1: was pin 13 — that is
the camera PCLK, and the engine holds the claim for the module's whole
loaded lifetime, so MISO=13 made st7789 and the camera mutually
exclusive; 14 is free outside the camera epoch's pin set.)

```lua
bench.wasm.call("st7789", "init", {x_offset=0, mode=3, invon=true})
bench.wasm.call("st7789", "clear", 0xF800)       -- solid red
bench.wasm.call("st7789", "pattern", "border")   -- offset probe
bench.wasm.call("st7789", "blit", buf_off)       -- full frame (115,200B)
bench.wasm.call("st7789", "deinit")
```

- **init cfg** (all optional): `x_offset` 0..80 clamped — the glass
  start offset. Geometry: the ST7789V RAM is 240 sources (CASET) x 320
  gates (RASET); a 240x240 glass bonds all 240 sources and 240 of the
  320 gates, so the panel-specific offset lands on **RASET** at
  MADCTL=0x00 portrait (the "x offset 80" lore from landscape-default
  libraries is the same offset after an axis swap). `mode` 0..3
  (default 3 = CPOL1/CPHA2, the ST7789 4-line sample edge);
  `invon` (default true — the common 240x240 IPS glass needs display
  inversion for true colors). ~250ms of delays inside one call.
- **Wire order**: frames are RGB565 **hi byte first** — the same order
  the GC2145 camera delivers, so a QVGA crop can be blitted verbatim.
  Full frame = 115,200B in 29 `spi_xfer` chunks (engine cap 4096B).
- **patterns**: `full` (white, fb path) / `border` (four 8px white
  partial-window strips at the glass edge — the offset probe: flush at
  the edge = right candidate, inset ~80 rows = wrong) / `quad` (R/G/B/W
  quadrants) / `red_rect` (black + centered 100x100 red). Every draw op
  returns `{fill_ms, xfer_ms, total_ms, bytes, chunks}` measured with
  `millis()` — under fast-interp the wasm-side fill loop dominates,
  the engine moves the bytes DMA-side.
- **deinit** sleeps the panel but keeps the spi/gpio claims (abi=1 has
  no release host fn) — `session_reset` of the loader session releases
  them, like every wasm claim. A held spi slot makes `init` raise the
  host's rt exception (owner conflict) — the conflict vector is pinned
  natively (T14) and on the board (HIL S50).
- Errors are clean `{error=...}` maps (draw-before-init, blit bounds,
  unknown pattern kind); host-level refusals surface as rt exceptions.
- st7789 uses the default 256KB initial memory (fb 115KB + stage/out/
  heap + the wasm stack) — hello's budget class.

## Repository layout

```
index.json                  # the module index (schema below; single
                            # source for "what is published")
modules/<name>/<ver>/       # built artifacts, committed:
                            #   <name>.wasm           deliverable (in index files[])
                            #   <name>.aot            auxiliary (hash in meta only)
                            #   <name>.meta.json      entry-function table + provenance
src/xuansi_host.h           # shared ABI declarations (module-side)
src/<name>/<name>.c         # module source (canonical home)
src/<name>/test_<name>.c    # native unit test + stub host
src/<name>/VERSION          # e.g. "1.0.0"
build.sh                    # wasi-sdk + wamrc build (all modules or by name)
tools/verify_index.py       # rebuild-vs-index hash comparison
.github/workflows/ci.yml    # CI: build / verify / native tests
```

`hello.c` was migrated from the firmware tree at the ABI-freeze batch;
the firmware's copy is frozen there as build evidence — this repository
is the canonical home for module sources from that point on.

## index.json schema

```json
{
  "index_ver": 1,
  "modules": [
    {
      "name": "hello",
      "kind": "driver",
      "version": "1.0.0",
      "abi": 1,
      "files": [
        { "path": "modules/hello/1.0.0/hello.wasm",
          "sha256": "<hex>", "bytes": 5691 }
      ],
      "src_commit": "<git commit of src/hello/hello.c>"
    }
  ]
}
```

`kind` is `"driver" | "sensor-profile" | "algorithm"`.

**Reserved: `sensor-profile`** (planned 0.3.0) — hot-loadable sensor
description modules (SCCB register sequences, timing parameters, frame
buffer geometry, pixel format) so a new DVP sensor becomes a data file
instead of a firmware rebuild. It is the first planned `abi = 2`
addition; the `kind` field is reserved now.

## Toolchain (pinned)

| piece | pin |
|---|---|
| wasi-sdk | **24.0** (LLVM 18.1.2) — [release tarball](https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz), sha256 `c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5` |
| wamrc | **WAMR-2.4.0** (tag `WAMR-2.4.0`, commit `47c7c85a…`) built from source against [espressif/llvm-project](https://github.com/espressif/llvm-project) `xtensa_release_18.1.2` (commit `344b928e…`) — prebuilt wamrc releases lack the xtensa backend |

Build flags are frozen in `build.sh` (`-O2 -fno-jump-tables -nostdlib`,
`--no-entry --initial-memory=<per module> --max-memory=1048576
--global-base=1024`, `--strip-debug --export-dynamic`; hello pins the
ABI-freeze 256KB initial, ledwatch pins 11 pages — see
`init_mem_for()`); exports are declared with `export_name` attributes.
Rebuilds are deterministic — CI re-derives every hash and compares it
against `index.json` and each `meta.json` on every push.

## Building and testing

```sh
./build.sh              # build all modules -> modules/<name>/<ver>/
./build.sh hello        # one module
./build.sh --check      # rebuild into a scratch dir, compare hashes
cc -DXUANSI_NATIVE_TEST -Wall -Wextra -I src -I src/hello \
   src/hello/test_hello.c -o /tmp/test_hello && /tmp/test_hello
```

The native test compiles the module with the host toolchain against a
stub host (`XUANSI_NATIVE_TEST` seam in `hello.c`/`xuansi_host.h`; the
wasm32 build is unchanged — the committed `hello.wasm` is
byte-identical to the pre-seam build).

## CI

Three legs on every push and PR (`.github/workflows/ci.yml`):

1. **build** — install the pinned toolchain (cached), `./build.sh`,
   upload the artifacts;
2. **verify** — rebuild from scratch and compare every hash against
   `index.json` (hard gate on deliverable `.wasm`) and each
   `meta.json` (auxiliary `.aot`);
3. **native** — compile and run every `src/*/test_*.c` with the host
   gcc against its stub host.

The first run after a cache miss builds wamrc's LLVM from source
(~1h); cached afterward.

## License

Apache-2.0 (see `LICENSE`).
