<!-- doc.md — the per-module, per-version doc face (B15 W2-T3). tools/
     extract_doc.py machine-reads the "Ops" table and the "Measured" list
     into <name>.meta.json's doc/release fields; CI regenerates and diffs
     (parity lock — edit here, never in the meta). -->

# hello — the ABI's reference module (full host-surface tour)

kind: driver
release: test-canary
version: 1.0.0
abi: 1

The ABI's first consumer and the template new modules copy: one entry
function per host-surface feature, exercised end to end — echo/add
marshalling, GPIO, timing, stream, SPI loopback, camera frame. The
marshalling convention (one msgpack array in at `xuansi_alloc`'s offset,
one packed `u64 = (result_ptr << 32) | result_len` out) ships here.

**test-canary** (not production): this artifact imports the retired
abi=1 camera names (`camera_frame_get`/`camera_info`) — on any abi=2
host table (fw ≥ 0.3.0) it is honestly refused at link time. That
negative case is exactly why it stays published: it is the loader's
retired-import refusal vector. Its `now`/`echo`/`add`/`spi_selftest`
faces remain the canonical quick smoke on abi=1-era firmware.

## Ops

| op | args | result | semantics |
|----|------|--------|-----------|
| echo | args... | args... | byte-level msgpack round-trip of the argument array |
| add | [a, b] | [a+b] | module-side msgpack decode+encode proof |
| gpio_echo | [out_pin?, in_pin?] | [hi, lo] | drive out (default 1), read in (default 2), drive low, read again |
| now | [] | [ms] | boot milliseconds via the out-param convention |
| now2x | [] | [ms, ms, ms] | three consecutive live snapshots (out-param landing proof) |
| timed_delay | [] | [dt_ms] | millis → delay_ms(120) → millis |
| delay_wrap | [] | raises | delay(100) then delay(0xFFFFFFF8): pins the host budget-cap refusal (no u32 wrap bypass) |
| stream_out | [] | [sent] | stream_send an 8-byte sample (host stamps ch=2 + module name) |
| spi_selftest | [] | [rx==tx] | self-configure SPI loopback (sclk=14, mosi=12, miso=13, 1MHz, mode 0, CS-less) + full-duplex 8B |
| spi_neg | [] | raises | spi_config(no_cs=0): a CS bus is refused at abi=1 (negative proof of the host exception face) |
| cam_frame | [] | [w, h, len] | camera_info probe + one QVGA RGB565 frame; the camera must already be on (caller's responsibility) |

Every module additionally exports the two ABI functions `xuansi_abi`
(returning 1 here) and `xuansi_alloc` (see the repo README's "Module
ABI" section; hello's `xuansi_alloc` is the args-slot-reuse template,
ABI §3.1).

## Measured

- `wasm_bytes` — 5714 — deliverable .wasm (index files[])
- `aot_bytes` — 9016 — toolchain-v3 surgery .aot
- `initial_memory_bytes` — 262144 — the ABI-freeze 4-page default (hello's budget class)
- `aot_text_bytes` — 6432 — surgery TEXT (PSRAM exec)
- `aot_pool_bytes` — 500 — xuansi.literal split pool
- `max_memory_bytes` — 1048576 — legacy lane (pre-recarve); the ABI 1 freeze pins initial==max on the W3 rebuild

## Notes

- `hello.c` was migrated from the firmware tree at the ABI-freeze
  batch; the fw copy is frozen there as build evidence, this repo is
  the canonical home.
- The native test (`src/hello/test_hello.c`) compiles against a stub
  host modeling every refusal channel — including the delay-wrap
  u32-overflow vector.
