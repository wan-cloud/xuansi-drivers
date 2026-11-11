<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# st7789 — ST7789V 240x240 TFT panel driver (the ABI's first driver module)

kind: driver
release: production
version: 1.0.0
abi: 1

Lua asks, the module drives real hardware (SPI + GPIO) through the
frozen host table — board facts compiled in: SCL=41, SDA(MOSI)=42,
RES=47, DC=21, backlight tied to 3V3, **no CS** (the bus is dedicated
to the panel — exactly the abi=1 CS-less engine, `no_cs=1`). MISO is
passed as **pin 14, legal but unwired**: the v1 engine's `pins_valid`
wants 0..47 on all three bus pins, so the frozen doc's "miso=-1 =
absent" convention is not actually accepted (`rc=-3`, hit and recorded
by B10-T6's on-board conflict vector); tx-only transfers never read
MISO. (Fix-round-1: was pin 13 — that is the camera PCLK, and the
engine holds the claim for the module's whole loaded lifetime, so
MISO=13 made st7789 and the camera mutually exclusive; 14 is free
outside the camera epoch's pin set.)

```lua
bench.wasm.call("st7789", "init", {x_offset=0, mode=3, invon=true})
bench.wasm.call("st7789", "clear", 0xF800)       -- solid red
bench.wasm.call("st7789", "pattern", "border")   -- offset probe
bench.wasm.call("st7789", "blit", buf_off)       -- full frame (115,200B)
bench.wasm.call("st7789", "deinit")
```

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| init | [cfg?] | {ok, x_offset, mode, invon, xfer_lo, ms} | spi_config(slot0, SCL=41, 40MHz, MOSI=42, MISO=14 legal-unwired, mode, no_cs=1) + hardware RES pulse + SLPOUT/COLMOD(16bit)/MADCTL(0x00)/INVON/CASET/RASET/DISPON. cfg keys all optional: x_offset 0..80 clamped (default 0 — the glass start offset; lands on RASET at MADCTL=0x00 since a 240x240 glass bonds all 240 sources and 240 of 320 gates), mode 0..3 clamped (default 3 = CPOL1/CPHA2, the ST7789 4-line sample edge), invon bool (default true — common 240x240 IPS glass needs inversion for true colors; eyeball-tunable). ~250ms of delays inside one call. A held spi slot surfaces as the host's rt exception (owner conflict). The reply publishes xfer_lo (P3-T2): the overlap-free floor for external frames |
| clear | [color565?] | {ok, bytes, chunks, fill_ms, xfer_ms, total_ms} | fb fill (wire-order RGB565 hi byte first) + full-frame stream; color default 0 (black); 115,200B in 29 spi_xfer chunks (engine cap 4096B/xfer) |
| blit | [buf_off?] | {ok, bytes, chunks, xfer_ms, total_ms} | stream one full frame (115,200B wire-order RGB565) from linear memory at buf_off; 0/absent = the module fb (pure re-stream, the SPI timing path). Nonzero must satisfy xfer_lo <= buf_off and buf_off+115200 <= 262144, where xfer_lo is the self-derived statics ceiling published by init (P3-T2, T8 FINDING-B: the historical floor of 8 covered the module's own statics — a legal-looking full-frame xfer there wrote the frame over .rodata and the state scalars). The frame span may cover the wasm stack above the statics: inter-call stack bytes are dead by wasm semantics. Same byte order as camera RGB565 frames (hi byte first) — a QVGA crop can be blitted verbatim. Violations → {error='blit offset out of bounds'} |
| pattern | [kind] | {ok, kind, windows, bytes, chunks, fill_ms, xfer_ms, total_ms} | kind: 'full' solid white (fb path); 'border' four 8px white partial-window strips at the glass edge (offset probe: flush at edge = x_offset right, inset ~80 rows = wrong candidate); 'quad' 120x120 quadrants TL=red TR=green BL=blue BR=white (partial windows; orientation/RGB-order eyeball); 'red_rect' black frame + centered 100x100 red at (70,70) (fb path; camera-contrast eyeball). Unknown/missing kind → {error}. Partial-window kinds report fill_ms 0 (stage fills counted in xfer_ms) |
| deinit | [] | {ok, ms} | DISPOFF + 20ms + SLPIN (panel to sleep). The spi slot and RES/DC pin claims REMAIN (abi=1 has no release host fn) — released by session_reset(loader) / vm.reopen, like every wasm claim |

Draw ops before a successful init return `{error="not initialized"}`;
domain errors are clean `{error=...}` maps; host-level refusals
(e.g. the spi-slot owner conflict) surface as the host's wasm
exception, class=rt.

**External frames** (P3-T2): `init`'s reply publishes `xfer_lo` — the
overlap-free floor. Host-side, xfer the frame to
`[xfer_lo, xfer_lo+115200)` (e.g.
`bench.wasm.xfer(src, src_off, "st7789", xfer_lo, 115200)`), then
`blit(xfer_lo)`.

## Measured

- `wasm_bytes` — 13487 — deliverable .wasm (interp lane)
- `aot_bytes` — 24012 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 262144 — 4 wasm pages: fb 115200 + stage 4096 + out 2048 + heap 8192 + args 128 + cmd 8 ~= 130KB + the 64KB stack; statics+stack end at 196,480; hello's budget class
- `aot_text_bytes` — 20484 — surgery TEXT (PSRAM exec); board-verified info() trio
- `aot_pool_bytes` — 1244 — xuansi.literal split pool (board-verified)
- `full_frame_bytes` — 115200 — one 240x240 RGB565 frame
- `full_frame_chunks` — 29 — at the engine's 4096B/xfer cap
- `blit_window` — [130960, 146944] — the external-frame window (overlap-free by statics_end() arithmetic; pinned against the built wasm by tools/verify_st7789_blit.mjs)
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve); the ABI 1 freeze pins initial==max on the W3 rebuild

## Wiring

- panel: ST7789V 240x240 IPS, 7-pin module, NO CS (bus dedicated)
- pins: SCL=41 SDA(MOSI)=42 RES=47 DC=21 BLK=3V3 (always lit)
- bus: SPI slot 0 @ 40MHz, mode 3 default (cfg), CS-less (no_cs=1);
  MISO=14 legal-unwired (claim held for the module's loaded lifetime)
- x_offset verdict: 0 CONFIRMED by owner eyeball 2026-11-08 — border
  pattern flush to the panel edges, white as designed (the
  x_offset=80 candidate would show the frame inset ~80 rows)

## HIL recipe notes

- The display-function HIL (B10-T6 shape, `st7789-hil-b10t6.py`)
  walks init → patterns → clear → blit → deinit against the owner's
  eyeball; the geometry/offset probe is the `border` pattern.
- **S6 color-vector environment sensitivity** (B14 minors ⑦, recorded
  2026-11-11): the S6 camera-vs-panel color-vector check is
  ENVIRONMENT-SENSITIVE — ambient light and panel warm-up move the
  measured vectors enough to flip a strict verdict. The future
  canonical recipe is the **s6redo form**: draw a BLACK panel (no
  ambient glare), then a FRESH `red_rect`, and match the camera's
  color vectors against that fresh draw (the B14 run's s6redo passed
  3/3 where the original form flaked). Prefer the redo form whenever
  a color-vector assertion is needed.
- st7789 on board #2 (no display wired): load/info/plan/unload +
  init/clear/deinit over SPI-to-air are electrically safe and are the
  wave-2 ABI-1 migration smoke (W2.T3); the full display face runs on
  board #1 (the USB window).
