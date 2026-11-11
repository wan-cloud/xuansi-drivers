<!-- doc.md — the per-module, per-version doc face (B15 W2.T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# gc2145 — the GC2145 bring-up sequence as module data + code (the first sensor-profile module)

kind: sensor-profile
release: production
version: 1.0.0
abi: 2

The camera-migration wave's sensor profile: the whole "driver lives in
wasm" thesis — the host supplies only the raw abi=2 natives (i2c×4 +
cam×5); every register decision is module-owned:

```lua
bench.wasm.call("gc2145", "init", {framesize="QVGA"})  -- or "VGA"
bench.wasm.call("gc2145", "info")                      -- passthrough + state
bench.wasm.call("gc2145", "off")                       -- cam_stop, idempotent
```

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| init | [{framesize="QVGA"\|"VGA"}?] | {ok, sensor, w, h, ms} \| {error, got?, want?} | full bring-up, default QVGA, unknown string = {error='unknown framesize'} BEFORE any hardware touch. Sequence: cam_stop + >=1000ms settle if a stream is on → cam_xclk(20MHz) → 2000ms warmup (100ms slices — GC2145 SCCB needs 1-2s after the clock starts) → i2c_config(slot0, SDA=4, SCL=5, 100kHz) → PID probe (i2c_writeread {0xF0}/{0xF1} @0x3C; mismatch → {error, got, want} carrying the READ pid, no side effects past the probe) → soft reset FIRST (0xFE<-0xE0, 100ms — first-class module knowledge the vendored esp32-camera driver hides in its probe path) → the full 582-pair init table as per-register i2c_write (wire-faithful — GC2145 SCCB has no documented burst auto-increment; transcribed from espressif/esp32-camera 2.1.8, Apache-2.0, guarded by count+hash in the native test) → 100ms → per-framesize subsample geometry deltas (QVGA 320x240 / VGA 640x480, both 1/2-subsample) → RGB565 format regs → cam_start(w,h,0). A framesize switch re-runs the WHOLE sequence (soft reset + PID probe included). ~2.4s of delay_ms inside one call (host cap 15000ms per call; slices keep the watchdog fed). Host failures (SCCB NACK, slot conflict, cam floor) propagate as the host's rt exception — never swallowed |
| off | [] | {ok, on=0, ms} | cam_stop (host idempotent: stop stream, release the 11 DVP + XCLK pin claims) + module off; safe before init and when repeated |
| info | [] | {sensor, framesize?, w, h, pixfmt, on} | cam_frame_info passthrough (ro, no sensor touch) plus the module's last framesize; stream off → w=h=0, on=0, pixfmt 0=RGB565 |

## Measured

- `wasm_bytes` — 6905 — deliverable .wasm (interp lane)
- `aot_bytes` — 11904 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 262144 — static memory only (instance heap_size=0: table in .rodata, §3.1 args-slot reuse; ~4KB used)
- `aot_text_bytes` — 8784 — surgery TEXT (PSRAM exec); board-verified info() trio
- `aot_pool_bytes` — 460 — xuansi.literal split pool (board-verified)
- `init_wall_ms` — ~2400 — sensor power-up + warmup delays (physics, not interpreter overhead); budget the eval call accordingly
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve); the ABI 1 freeze pins initial==max on the W3 rebuild

## Wiring

- sensor: GC2145 DVP camera (the bench camera epoch)
- SCCB: 7-bit addr 0x3C on i2c slot 0, SDA=4 SCL=5 @ 100kHz (pins claimed prim=i2c, owner=wasm — see the known claim asymmetry below)
- XCLK: 20MHz via cam_xclk (pin 15, LCD_CAM direct, 160MHz/8)
- PID: 0x2145 at regs 0xF0/0xF1 (hi/lo)
- DVP: 11 pins + XCLK claimed prim=camera owner=camera (op-face session) by cam_start; released by cam_stop

## Notes

- **Known claim asymmetry (F-3)**: the SCCB pins 4/5 i2c claim
  persists across `off()` (the i2c face has no release); it clears on
  session reset — meanwhile an op-path capture wanting those pins is
  refused with a message naming the pin and owner.
- `fw_compat: {wasm >= 0.3.0}` — needs the abi=2 host table; the
  retired abi=1 camera names are NOT imported.
- Board exercise: B12-T8 (the module is native-tested against a fake
  i2c host + programmable PID + trap channel: 40 checks; the bench
  camera recipe that composes gc2145 + jpge is the repo README's
  "Agent camera recipe").
