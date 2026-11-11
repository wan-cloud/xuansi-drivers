<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# ledwatch@1.0.0 — camera frame → LED on/off regions (the superseded abi=1 build)

kind: algorithm
release: production
version: 1.0.0
abi: 1

Historical entry (superseded by [ledwatch@2.0.0](../2.0.0/doc.md) at
the camera migration): the same analyze() face on the abi=1 import
table — it imports the retired `camera_frame_get`/`camera_info` names,
so any abi=2 host (fw ≥ 0.3.0) honestly refuses it at link time. Kept
published as the rename-era negative vector; behavior on abi=1-era
firmware is byte-identical to 2.0.0's.

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| analyze | [cfg?] | {regions=[{x,y,w,h,area,luma_mean,r_mean,g_mean,b_mean}], frame={w,h}} | one QVGA frame → lit regions sorted by area desc, capped at max_regions; cfg keys all optional, unknown keys ignored: threshold 0..255 (default 128, lit = luma >= threshold), min_area (default 16), max_regions 1..32 (default 8), discard_first (default true — consume one frame first for AWB convergence; stateless: exactly two camera_frame_get calls per analyze) |

Semantics detail (shared with 2.0.0): means are integers 0..255 (the
frozen int-only value bridge carries no floats); RGB565 components
replicated into 8 bits, means rounded to nearest; luma = BT.601
integer approximation `(77R + 150G + 29B) >> 8` (pure red is luma 76 —
pass a lower threshold when hunting red-only LEDs). The camera must
already be on at QVGA (caller's responsibility); camera-off or larger
framesizes surface as the host's `rt` exception. Pipeline: luma →
threshold → two-pass connected-component labelling (4-connectivity,
union-find with path compression, u16 label map) → per-label area →
top-N selection → stats pass (bbox + means). The u16 label space is
overflow-proof at QVGA (labels only created by lit pixels whose
left/up neighbours are dark — at most w·h/2 = 38,400 < 65,535; the
checkerboard vector pins the worst case).

## Measured

- `wasm_bytes` — 7547 — deliverable .wasm (interp lane)
- `initial_memory_bytes` — 720896 — 11 pages: the CCL working set (~627KB) + the wasm stack
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve)
- `coload_note` — one ledwatch + one hello fit the 1MB abi=1-era WAMR pool; two ledwatch instances did not — the abi=2 wave widened the pool to 2MB

## Notes

- The auxiliary `.aot` recorded in this version's meta is the old
  stock-wamrc product (provenance only, never indexed — not the
  toolchain-v3 surgery lane).
