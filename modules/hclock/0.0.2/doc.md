<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# hclock — the millis one-native module (the bench clock, smallest reference)

kind: driver
release: production
version: 0.0.2
abi: 1

The smallest useful module: one native (`xuansi.millis`), one op.
Born on-board (B14, 2026-11-11 — an agent's live composition, the
session that had to copy hello.c out of the then-private firmware tree
and hit the missing-stdint + 256B-heap template pitfalls); the
repo-native rebuild for the recarved ABI 1 keeps the same face and the
safe alloc convention. Since B15 W1-T3 the board exposes
`bench.millis()` directly — hclock remains as the one-native reference
and the cheapest load-path smoke.

```lua
bench.wasm.call("hclock", "now")   -- -> {ms}
```

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| now | [] | [ms] | boot milliseconds (u32, wraps at 49.7 days) via the millis out-param convention — fresh read per call, landed in a .bss scalar (cross-lane portable: never a wasm stack slot) |

## Measured

- `wasm_bytes` — 729 — interp lane (the on-board v0.0.2 predecessor was 704B: the repo-native safe-alloc convention costs ~25B)
- `aot_bytes` — 1284 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 262144 — the freeze default (args slot + 8KB bump arena + out buffer, ~26KB with the wasm stack)
- `aot_text_bytes` — 448 — surgery TEXT (PSRAM exec)
- `aot_pool_bytes` — 76 — xuansi.literal split pool
- `max_memory_bytes` — 262144 — the recarved ABI 1 freeze (initial == max)

## Notes

- Native used: `xuansi.millis` only. The module claims no hardware.
- Lineage: 0.0.1/0.0.2 (on-board era) lived in the daemon workspace +
  board #1 `/drv`; this is the first repo-native build. Version stays
  0.0.2 to keep the name the ecosystem knows — the on-board copies die
  in the wave-3 cleanup (old-shape memory sections, honestly refused).
