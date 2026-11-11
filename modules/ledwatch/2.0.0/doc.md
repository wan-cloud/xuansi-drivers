<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# ledwatch — camera frame → LED on/off regions (the ABI's first algorithm module)

kind: algorithm
release: production
version: 2.0.0
abi: 2

Turns QVGA RGB565 camera frames into LED on/off regions — the
analyze() output face is consumed verbatim by the bench's display and
closed-loop tasks:

```lua
bench.wasm.call("ledwatch", "analyze", cfg)
```

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| analyze | [cfg?] | {regions=[{x,y,w,h,area,luma_mean,r_mean,g_mean,b_mean}], frame={w=320,h=240}} | one QVGA frame → lit regions sorted by area (largest first), capped at max_regions; cfg (all optional, unknown keys ignored): threshold 0..255 default 128 (lit = luma >= threshold), min_area default 16 (smaller regions dropped), max_regions 1..32 default 8, discard_first default true (consume one frame first — unconverged AWB after camera-on; stateless: exactly two cam_frame_get calls per analyze) |

Means are integers 0..255 (the int-only value bridge era carried no
floats). RGB565 components replicated into 8 bits, means rounded to
nearest; luma = BT.601 integer approximation
`(77R + 150G + 29B) >> 8` — pure red is luma 76, so pass a lower
threshold when hunting red-only LEDs. The camera must already be on at
QVGA (caller's responsibility, same contract as gc2145's stream);
camera-off or larger framesizes surface as the host's `rt` exception.

Pipeline: luma → threshold → two-pass connected-component labelling
(4-connectivity, union-find with path compression, u16 label map in
linear memory) → per-label area → top-N selection → stats pass (bbox +
means). The u16 label space is overflow-proof at QVGA: labels are only
created by lit pixels whose left/up neighbours are dark (a
4-independent set, at most w·h/2 = 38,400 < 65,535) — the
checkerboard vector pins the worst case.

Behavior is byte-identical to 1.0.0; the only change from
[1.0.0](../1.0.0/doc.md) is the abi=2 import face (`cam_frame_get`
carries geometry with the frame; the retired `camera_*` names are
gone) — needs the abi=2 host table (`fw_compat: wasm >= 0.3.0`).

## Measured

- `wasm_bytes` — 7537 — deliverable .wasm (interp lane)
- `aot_bytes` — 12744 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 720896 — 11 pages: CCL working set ~627KB (frame 153600 + u16 label map 153600 + parent 76804 + area 153608 + selected map 76804 + heap 8192 + result 4096) + the wasm-ld 64KB stack
- `aot_text_bytes` — 11520 — surgery TEXT (PSRAM exec); board-verified info() trio
- `aot_pool_bytes` — 244 — xuansi.literal split pool (board-verified)
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve); the ABI 1 freeze pins initial==max on the W3 rebuild
- `coload_note` — co-loads with gc2145 + jpge + st7789 in the 2MB pool on the board (measured stable across both load orders + unload/reload cycles, 2026-11-09)
