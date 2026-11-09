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
"ledwatch" below) — `st7789@1.0.0` — the ABI's first `driver`
module (ST7789V 240x240 TFT panel over CS-less SPI; see "st7789"
below) — `gc2145@1.0.0` — the first `sensor-profile` module (the
GC2145 bring-up sequence as module data + code; see "gc2145" below)
— and `jpge@1.0.0` — the first `codec` module (camera frames →
JPEG or raw RGB565 out the stream; see "jpge" below).

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

### abi = 2 (camera migration, 2026-11-09)

The camera-migration wave revised the symbol table — a one-time
exemption while the project had no external users (from abi=3 on the
table is strictly additive again):

- **new (9)**: `i2c_config`, `i2c_write`, `i2c_read`, `i2c_writeread`
  (self-configured I2C master, pins claimed prim=i2c) and `cam_xclk`,
  `cam_start`, `cam_stop`, `cam_frame_get`, `cam_frame_info` (the
  parallel-capture DVP stream; `cam_frame_get` hands the frame's
  geometry out through w/h outs).
- **retired (2)**: `camera_frame_get` / `camera_info`, renamed onto
  `cam_frame_get` (geometry now rides the frame call) /
  `cam_frame_info` (reshaped to w/h/pixfmt/on). An abi=1 binary
  importing the retired names is refused at link time; the rest of
  the abi=1 face is untouched (st7789@1 loads as-is; ledwatch
  recompiled to 2.0.0 for the rename).

A module declares abi=2 with `#define XUANSI_ABI 2` before including
`src/xuansi_host.h`; the loader gate accepts abi∈{1,2}.

Further symbol additions or semantic changes mean a new abi version —
from abi=3 on the table is strictly additive (the abi=2 retirements
were the one-time exemption above).

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
`max_regions`. All means are integers 0..255 (the frozen int-only
value bridge carries no floats): RGB565 components replicated into
8 bits, means rounded to nearest, luma = BT.601 integer approximation
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
stack. Through the abi=1 waves the WAMR pool was 1MB: one ledwatch +
one hello co-load fit it; two ledwatch instances did not (the loader
refuses with an honest out-of-memory error). The abi=2 wave widened
the pool to 2MB — jpge's 917,504B static layout beside ledwatch no
longer fits 1MB — and co-load budgets now read against 2MB (see
"Agent camera recipe" below).

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
- **external frames** (P3-T2): `init`'s reply publishes `xfer_lo` —
  the overlap-free floor. Host-side, xfer the frame to
  `[xfer_lo, xfer_lo+115200)` (e.g. `bench.wasm.xfer(src, src_off,
  "st7789", xfer_lo, 115200)`), then `blit(xfer_lo)`. The window
  `[xfer_lo, 146944]` is provably disjoint from the module's own
  statics (the floor is self-derived from the statics' addresses at
  build time); the historical `[8, 146944]` window covered .rodata and
  the state scalars — a legal-looking full-frame xfer there corrupted
  the consumer instance (B12-P1 T8 FINDING-B; fixed in place). The
  span may cover the wasm stack above the statics: inter-call stack
  bytes are dead by wasm semantics (frames are rebuilt per call).
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

## gc2145 (sensor-profile module — the first abi=2 kind)

`gc2145@1.0.0` is the B12 camera-migration wave's sensor profile: the
GC2145 bring-up sequence as **module data + module code** — the whole
"driver lives in wasm" thesis. The host supplies only the raw abi=2
natives (i2c×4 + cam×5); every register decision is module-owned:

```lua
bench.wasm.call("gc2145", "init", {framesize="QVGA"})  -- or "VGA"
bench.wasm.call("gc2145", "info")                      -- passthrough + state
bench.wasm.call("gc2145", "off")                       -- cam_stop, idempotent
```

- **init sequence**: (cam_stop + >=1000ms settle if a stream is on) →
  `cam_xclk(20MHz)` → 2000ms warmup in 100ms slices →
  `i2c_config(slot0, SDA=4, SCL=5, 100kHz)` → PID probe
  (`i2c_writeread {0xF0}`/`{0xF1}` @0x3C; mismatch → `{error, got,
  want}` carrying the READ pid) → **soft reset first** (`0xFE<-0xE0`,
  100ms — first-class module knowledge; the vendored esp32-camera
  driver hides it in its probe path) → the full 582-pair init table
  (per-register `i2c_write`, wire-faithful; transcribed from
  espressif/esp32-camera 2.1.8, Apache-2.0, guarded by count+hash in
  the native test) → 100ms → per-framesize subsample geometry deltas
  (QVGA 320×240 / VGA 640×480, both 1/2-subsample) → RGB565 format
  regs → `cam_start(w,h,0)`.
- **framesize switch** re-runs the whole sequence (soft reset
  included) after cam_stop + settle; **off** is cam_stop (host
  idempotent) + module off; **info** is `cam_frame_info` passthrough
  plus the module's last framesize.
- **known claim asymmetry (F-3)**: the SCCB pins 4/5 i2c claim
  persists across `off()` (the i2c face has no release); it clears on
  session reset — meanwhile an op-path capture wanting those pins is
  refused with a message naming the pin and owner.
- Static memory only (instance heap_size=0): table in .rodata, §3.1
  args-slot reuse; default 256KB initial memory (~4KB used).
- `fw_compat: {wasm>=0.3.0}` — needs the abi=2 host table (fw 0.3.0);
  the retired abi=1 camera names are NOT imported.
- Board exercise is B12-T8 (the bench camera is stalled on a physical
  issue; the module is native-tested against a fake i2c host +
  programmable PID + trap channel: 40 checks).

## jpge (codec module — the first of its kind)

`jpge@1.0.0` is the camera-path **codec**: frames in (camera), encoded
or raw bytes out (stream ch=2). It vendors the exact JPEG encoder the
board's native path used (esp32-camera 2.1.8 jpge; per-scanline
RGB565→RGB888 conversion, H2V2, the `frame2jpg` quality semantics)
behind the abi=2 host face — `cam_frame_get` hands the geometry out
with the frame, `stream_send` (host-stamped `"jpge"`, ch=2) is the
egress. The B12-P0 calibration artifact (0.1.0, never indexed) proved
the cost model; this is its formal successor. Source layout differs
from the pure-C modules by exactly one thing: `src/jpge/jpge.cpp`
(the vendored C++ encoder core, `-fno-rtti -fno-exceptions` — the one
sanctioned C-flag-set deviation, see `build.sh`) linked beside the C
wrapper, and the native test is a two-TU build (see `test_jpge.c`'s
header). The module imports `xuansi.{cam_frame_get,millis,
stream_send}` plus `env.{memcpy,memset}` (WAMR libc-builtin — LLVM
lowers the wrapper's byte-loop mem helpers to those imports).

```lua
bench.wasm.call("jpge", "grab")                 -- {off,len,w,h}
bench.wasm.call("jpge", "encode", {quality=80}) -- {off,len} (last grab)
bench.wasm.call("jpge", "shot",   {quality=80}) -- {len,w,h,enc_ms}
                                                --   + one ch=2 JPEG frame
bench.wasm.call("jpge", "stream_raw")           -- {len,w,h}
                                                --   + one ch=2 raw frame
```

`shot` = grab + encode (`enc_ms` spans the encode only) + SOI/EOI gate
+ one `stream_send`. `stream_raw` = grab + one `stream_send` of the
RAW RGB565 frame (`len = w*h*2`) — the generic CV egress, no JPEG.
The host chunks payloads at 65,526 − 5B (the name stamp) = 65,521B;
VGA JPEGs and `stream_raw` VGA (614,400B = 10 chunks) are first-class
shapes, natively tested against a chunk-emulating sink. Errors are
`{error=...}` maps (no frame, encoder overflow, non-JPEG output); a
TX-full/OOM mid-stream truncation answers the honest numbers
`{error, len, sent, w, h[, enc_ms]}` — exactly how much of the frame
survived the wire. Host refusals (camera off, frame over cap) raise
rt as usual.

**Honest interp risk** (B12-P0 measured; the VM-task watchdog kills a
single wasm call > 5s): VGA q80 encode ~15s+, QVGA q60 ~3.9s — VGA
shots WILL trip the TWDT on the interpreter; QVGA q<=60 is the
measured interp-safe class. `shot` deliberately stays one call; the
resolution is the AOT track (auxiliary `.aot`, hash-recorded), not
slice APIs.

Linear memory: static VGA-capable layout pinned at
`--initial-memory=917504` (frame 614.4KB + JPEG out 128KB + encoder
MCU pool 30.7KB + misc + 64KB stack); the one dynamic allocation inside
the encoder (the MCU line pool) is a fixed arena with its high-water
tracked — 15,360B at QVGA / 30,720B at VGA, asserted natively in
`test_jpge.c` (T09).

## Repository layout

```
index.json                  # the module index (schema below; single
                            # source for "what is published")
toolchain-index.json        # the pinned-toolchain index (see
                            # "Toolchain"); served raw to the daemon
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
tools/verify_tcindex.py     # toolchain-index.json structure check
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

`kind` is `"driver" | "sensor-profile" | "algorithm" | "codec"`.

**`codec`** — landed with `jpge@1.0.0` (B12-T6, the first of its
kind): transform modules — frames or bytes in (camera), encoded or
re-serialized bytes out (stream ch=2), no hardware claim of their own.
The index schema passes `kind` through verbatim (the daemon's
driver-fetch chain treats it as an opaque label); `codec` entries
carry `fw_compat` like `sensor-profile` (`{"wasm": ">=0.3.0"}` — the
abi=2 host table).

**`sensor-profile`** — landed with `gc2145@1.0.0` (B12-T5, the first
abi=2 module): hot-loadable sensor description modules (SCCB register
sequences, timing parameters, frame geometry, pixel format) so a new
DVP sensor becomes a data file instead of a firmware rebuild. The
gc2145 entry additionally carries `fw_compat` (the fw floor the
module's imports need — `{"wasm": ">=0.3.0"}` for the abi=2 host
table); the daemon's driver-fetch chain reads it (B12-T8).

## Agent camera recipe (photo walkthrough)

The camera-path modules compose into the end-to-end photo recipe —
every step an MCP tool call, no shell anywhere. This block is the
canonical recipe text; the B12-T8 HIL scenario pins it verbatim. It
is the primitive chain, not literal JSON (each tool call carries its
board/code arguments as usual):

```
xuansi_driver_fetch{name:"gc2145"} / {name:"jpge"}     # 缺省 latest
xuansi_driver_push ×2
xuansi_run_lua{budget_ms:10000, code: load×2 → gc2145.init{framesize:"VGA"}
              → jpge.shot{quality:80} → gc2145.off()}   # 冷板收尾
  ↳ frames=[{ch:2,name:"jpge",data_b64}] → agent 存檔＋sha256
```

Line by line: `xuansi_driver_fetch` pulls each module from this repo's
`index.json` (version omitted = latest), `xuansi_driver_push` deploys
both to `/drv/<name>.wasm` under the held lease, and the single eval
call chains `bench.wasm.load` (gc2145, jpge) → `gc2145.init` →
`jpge.shot` → `gc2145.off`. Two reply-side primitives carry it:

- **`budget_ms`** (1..30000) — a per-call eval budget: it overrides
  the session budget for this one call (omit it, or pass 0, to ride
  the session budget as before; `xuansi_submit` takes the same key
  and applies it to the queued job while its ack stays immediate).
  The recipe declares 10000 because the modules need it honestly:
  `init` alone has a ~2.5s wall (sensor power-up + AE/AWB convergence
  — not interpreter overhead) and the shot's encode rides on top; the
  daemon's reply wait derives from the effective budget (budget + 2s,
  under its 60s per-request ceiling). Honest limit: it does not
  exempt a single long module call from the VM-task watchdog (> 5s
  one-call still trips) — which is exactly why the interp window
  below pins QVGA.
- **`frames`** — the reply carries `frames:[{ch, name, data_b64}]`:
  the stream frames that arrived on the connection during the call,
  any channel, `name` the host-stamped module name (a jpge `shot`
  lands as `{ch:2, name:"jpge"}`; a submit job's frames ride its job
  event instead). Bounded at 16 frames / 1 MiB cumulative;
  `frames_dropped` counts the honest losses (chunk gaps included) and
  appears only when something was dropped; when nothing streamed the
  whole field is absent — the reply shape existing callers know is
  unchanged. There is no server-side capture path: the agent decodes
  the base64, writes the file and takes its own sha256.

**冷板收尾 (cold-board finish) — `gc2145.off()` is deliberately the
last step**: it stops the stream, releases the camera pins (the DVP
set plus XCLK) and hands the frame-buffer memory back; the call is
idempotent. The documented F-3 exception still holds — the SCCB i2c
claim on pins 4/5 persists across `off()` and clears on session reset
(see "gc2145" above).

**The interp transition window pins QVGA** (until the AOT track — P3 —
lands): the recipe is written at its target shape (VGA, in-module
encode), but on today's interpreter a VGA `shot` is a single ~15s+
encode that WILL trip the 5s VM-task watchdog (measured on the board —
task-wdt panic, self-healed; the interp tax is 30-50× over native).
QVGA is the interp-window framesize on two hard legs of evidence: a
q60 encode measured ~3s on scene frames (~3.9s on noisy ones — green
even against the 4s session-budget ceiling), and the VGA watchdog trip
above. Pass `framesize:"QVGA"` and keep quality in the measured-safe
class (q ≤ 60 — the block's `quality:80` is the AOT-era value) until
P3 lands; going back to VGA then is a one-token recipe change.

**Co-load budgets — measured, not summed** (2026-11-09 board run):
the WAMR pool is 2MB since the abi=2 wave — the 1MB pool could not
hold jpge's 917,504B static layout beside ledwatch's 720,896B, which
is why it was widened. On the board, all four published modules load
and run together — gc2145 (262,144B initial) + jpge (917,504B) +
ledwatch (720,896B) + st7789 (262,144B), an initial-memory sum of
2,162,688B against the pool's 2,097,152B — measured stable across
both load orders plus unload/reload cycles: the pool's bookkeeping
is not a simple sum-of-initial-memories ceiling, and an earlier
revision of this paragraph that ruled four-way co-residency out was
wrong (corrected here). Two caveats remain, honestly: the loader
still refuses with an out-of-memory error when it does run out —
never design against the pool's last byte — and a VGA scene
additionally parks the ~600KB DVP frame buffer on the same PSRAM
(allocated firmware-side, outside the pool), so four-module
co-residency under a VGA scene is unmeasured; in the interp window
the recipe pins QVGA anyway (previous note), which is the shape the
four-module runs were measured at.

**No camera tool on the MCP face — assemble state from the modules**:
the dedicated camera status/capture/off tools are gone; camera
knowledge now lives entirely in the modules. Whatever an agent (or a
HIL check) wants to know about camera state, it assembles from the
two existing paths: `bench.wasm.call("gc2145", "info")` — the host
camera-state passthrough plus the module's last framesize — and the
`diag.pins` bench op, the pin-claim table (which owner holds which
pin).

## Toolchain (pinned)

| piece | pin |
|---|---|
| wasi-sdk | **24.0** (LLVM 18.1.2) — [release tarball](https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz), sha256 `c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5` |
| wamrc | **WAMR-2.4.0** (tag `WAMR-2.4.0`, commit `47c7c85a…`) built from source against [espressif/llvm-project](https://github.com/espressif/llvm-project) `xtensa_release_18.1.2` (commit `344b928e…`) — prebuilt wamrc releases lack the xtensa backend |

The machine-readable pin is [`toolchain-index.json`](toolchain-index.json)
(structure-checked by `tools/verify_tcindex.py`) — the pinned source the
daemon's `xuansi_toolchain_fetch` tool fetches, served raw from this
repository's `main`. The wamrc leg-matrix is still rolling out:
platforms without a built wamrc carry `null` there (linux-arm64 and
win-x64 today — wasi-sdk is pinned regardless, and the wamrc entries
land as those legs are built). win-x64 wamrc: best-effort leg failed
(MSVC probe); tracked P2.

Build flags are frozen in `build.sh` (`-O2 -fno-jump-tables -nostdlib`,
`--no-entry --initial-memory=<per module> --max-memory=1048576
--global-base=1024`, `--strip-debug --export-dynamic`; hello pins the
ABI-freeze 256KB initial, ledwatch pins 11 pages — see
`init_mem_for()`); exports are declared with `export_name` attributes.
Rebuilds are deterministic — CI re-derives every hash and compares it
against `index.json` and each `meta.json` on every push.

## User-compiled modules (agent path)

Who needs the toolchain at all — the pin above is only paid by the last
row:

| you… | toolchain on your host | how modules arrive |
|---|---|---|
| only use the board (GPIO/UART/camera, Lua drivers) | none — zero | nothing to install: the npm package never grows, the daemon fetches nothing |
| use the published prebuilt modules | none — zero | `xuansi_driver_fetch {name}` pulls this repo's `index.json` artifacts (the board never touches the network) |
| compile your own C modules | wasi-sdk (one `xuansi_toolchain_fetch`) | this section |

The self-compile chain is MCP tools end to end — no shell, no manual SDK
install (the toolchain face is deliberately zero-CLI; humans ride the
same tools through their agent):

1. **`xuansi_toolchain_status {}`** — one read-only view: per piece
   (`wasi_sdk`, `wamrc`) whether the local cache records it AND its
   files still exist, the installed generation vs the index's latest,
   and the board-fw pairing matrix. Missing pieces self-guide — the
   `guidance` field names the next step (`xuansi_toolchain_fetch`).
   No lease, no daemon call.
2. **`xuansi_toolchain_fetch {}`** — installs this host's pieces from
   `toolchain-index.json` (platform auto-selected from the host; there
   is no platform input). Optional: `generation` (omit = latest),
   `index_url`, `timeout_sec` (bounds the whole fetch; default 600s,
   max 3600s — the first wasi-sdk pull is ~120MB on linux, ~495MB on
   windows). Every archive streams sha256-pinned against the index,
   lands in a random staging dir, is re-verified, then renames into
   `~/.cache/xuansi-wasm/` with one atomic `toolchain-state.json`
   commit — any failure at any step leaves the cache byte-for-byte
   untouched. Idempotent: a piece whose pinned sha is already on disk
   is `skipped`, so re-calling is the safe sync verb.
3. **`xuansi_wasm_build {name:"mymod", source:"…"}`** — compiles the
   C source ON THIS HOST with the frozen flag block above and lands
   `drivers/mymod@<version>.wasm` in the daemon-side workspace. The
   full source rides the tool call (≤ 1 MiB; over is a refusal naming
   the cap) and `#include "xuansi_host.h"` resolves to the frozen host
   table embedded in the daemon. Knobs: `version` `[a-z0-9.]+`, omitted
   = `"0.0.1"` (the first-build convention — the build face has no
   index whose highest to take, unlike `driver_fetch`; rebuilds version
   explicitly); `initial_memory` in **bytes**, omitted = the freeze
   default 262144 (4 × 64 KiB pages — hello's budget class), an
   explicit value must be a whole multiple of 65536 and at most the
   frozen 1048576 ceiling (ledwatch-scale working sets pin 11 pages,
   `720896`); `fw` = an explicit early-refusal pre-check against the
   0.2.0 wasm-loader floor (omit = no check — bytecode is
   version-less). The reply carries `file`/`sha256`/`bytes` plus
   clang's stdout/stderr verbatim as `warnings`; a failed compile
   refuses with the stderr tail and nothing lands. Storing is NOT
   activating — the manifest's `Active` only moves on a verified push.
4. **lease the board, then `xuansi_driver_push {board:"bench1",
   name:"mymod", version:"0.0.1"}`** — deploys the artifact to the
   board's `/drv/mymod.wasm` (magic-gated, sha read-back verified).
   The FIRST push of a name states its version; once a version is
   active, the bare `{board, name}` re-deploys it (rollback = push the
   older version explicitly).
5. exercise it over eval — `xuansi_run_lua {board:"bench1", code:
   "…"}` riding the same lease:

   ```lua
   bench.wasm.load("mymod")
   bench.wasm.call("mymod", "echo", {1, 2, 3})
   ```

`.aot` today: the toolchain's aot domain is empty in P1 — `.aot` is
refused for every fw (the current board stack cannot execute it), and
`wamrc_path` is the explicit experimental developer lane whose build
still stops at an honest not-yet refusal. When the A1 runtime lands,
the index's aot domain opens with zero module-side changes.

### Manual fallback (documentation-level)

The fetcher above is the path; these pinned-sha steps are the
documented fallback for hosts where it cannot run — and the same cache
feeds this repo's own `build.sh`. Always read the URL and sha256 for
YOUR platform key (`linux-x64` | `linux-arm64` | `macos-x64` |
`macos-arm64` | `win-x64`) from
[`toolchain-index.json`](toolchain-index.json) — never from this
README:

sh (linux / macOS):

```sh
IDX=https://raw.githubusercontent.com/wan-cloud/xuansi-drivers/main/toolchain-index.json
PLAT=linux-x64   # your platform key: linux-x64 | linux-arm64 | macos-x64 | macos-arm64
URL=$(curl -fsSL "$IDX" | python3 -c "import json,sys; print(json.load(sys.stdin)['generations']['toolchain-v2']['platforms']['$PLAT']['wasi_sdk']['url'])")
SHA=$(curl -fsSL "$IDX" | python3 -c "import json,sys; print(json.load(sys.stdin)['generations']['toolchain-v2']['platforms']['$PLAT']['wasi_sdk']['sha256'])")
curl -fL -o sdk.tar.gz "$URL"
echo "$SHA  sdk.tar.gz" | sha256sum -c -        # must say OK (two spaces; macOS: shasum -a 256 -c -)
CACHE="${XUANSI_WASM_CACHE:-$HOME/.cache/xuansi-wasm}"
mkdir -p "$CACHE" && tar -xzf sdk.tar.gz -C "$CACHE"
```

PowerShell (windows):

```powershell
$plat = 'win-x64'   # your platform key
$a = (Invoke-RestMethod https://raw.githubusercontent.com/wan-cloud/xuansi-drivers/main/toolchain-index.json).generations.'toolchain-v2'.platforms.$plat.wasi_sdk
Invoke-WebRequest $a.url -OutFile sdk.tar.gz
if ((Get-FileHash sdk.tar.gz -Algorithm SHA256).Hash -notin @($a.sha256, $a.sha256.ToUpper())) { throw 'sha256 mismatch' }
$cache = if ($env:XUANSI_WASM_CACHE) { $env:XUANSI_WASM_CACHE } else { "$env:USERPROFILE\.cache\xuansi-wasm" }
New-Item -ItemType Directory -Force $cache | Out-Null
tar -xzf sdk.tar.gz -C $cache
```

Consumers of a manual install: this repo's `build.sh` reads
`WASI_SDK=<extracted tree>`; `xuansi_wasm_build` takes the same tree
as `wasi_sdk_path` (the §6.4 developer override — it deliberately
bypasses the cache's pairing evidence, so the manifest records kind
`wasm-dev` instead of `wasm`: visibly a dev artifact). Honest note: a
manual install writes no `toolchain-state.json` — only
`xuansi_toolchain_fetch` commits that pairing evidence — so the
build's default cache resolution will not auto-discover a hand-unpacked
tree; the two consumer routes above are the honest ways to use one.

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
