<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# trigpat — the T1 trigger-module reference (bit-pattern window matcher)

kind: trigger
release: production
version: 1.0.0
abi: 2

The first `trigger` module (LA wave D, ABI 2): the agent recipe starting
point for `la.sample_arm`/`la.edge_arm` with `trig.kind="module"`. The
engine calls `trigger_scan` at a bounded cadence; the module pulls one
run-length window via `la_window_get` and answers fire / no-fire. This
one matches a bit pattern over the sample domain: `width` consecutive
samples with `(sample & mask) == (value & mask)`, runs expanded
arithmetically (a record's value is constant over its run — the same
integer identity the fw's own `bits` trigger uses). Edge-domain windows
are not matched (returns 0): documented sample-domain only — copy the
source and grow your own matcher for the `{mask, dur_ns}` domain.

Determinism contract: `trigger_scan` is a pure function of (window
content, config state). No cross-call scan state, no clocks, no host
calls besides `la_window_get`. The same window bytes + config always
give the same decision (README "Trigger modules").

Recipe: push + load + configure + arm:

    driver_push trigpat            # or fs.write /drv/trigpat.aot
    eval: bench.wasm.load('trigpat')
    eval: bench.wasm.call('trigpat', 'config', 0x0F, 0x05, 4)
    la.sample_arm{rate_hz=1000000, ring_kb=256, trig={kind="module",
                   module="trigpat", pre_kb=4}}

The instance is pinned for the capture (`bench.wasm.unload` refuses
until `la.stop`/`la.disarm`); `pre_kb >= window depth` (512 records x
5B ≈ 2.6KB) keeps the matched pattern inside the captured window.

## Ops

| op | args | result | semantics |
| --- | --- | --- | --- |
| `xuansi_abi` | — | `u32` | declares ABI 2 (the trigger face gate) |
| `xuansi_alloc` | `u32 n` | `u32 off` | msgpack args slot (ABI 1 convention; 0 = refusal) |
| `config` | `[mask, value, width]` | `[ok]` | set the pattern (mask/value u8, width 1..4096); domain-reject keeps the previous config; defaults mask=0xFF value=0xA5 width=8 |
| `trigger_scan` | — | `i32` | 0 = no fire, 1 = fire — pull one window (`la_window_get`) and match; raw i32 callback convention, no marshalling |

## Measured

- `window_capacity` — 4096B buffer (fw window = 512 records x 5B + 16B header = 2576B; headroom documented, board round pending)
