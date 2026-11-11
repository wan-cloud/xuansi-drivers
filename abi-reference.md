# xuansi ABI reference

Generated single-source from `src/xuansi_host.h` (the symbol face — the module-side mirror of the firmware's `tools/wasm/xuansi_host.h`) plus the curated `tools/gen-abi-reference/sidecar.json`; regenerate with `tools/gen-abi-reference/gen.py` (CI drift-locks the output byte-for-byte). This is the reference the daemon's `xuansi_abi_reference` tool embeds.

**ABI 1** — ABI 1 is the 2026-11-11 recarve (B15 ruling): one contract axis — module ABI and bench api_version are the SAME 1. The old abi=1/abi=2 numbers are retired with fw 0.5.0 (wave-3); the recarved table is the unified host table (the i2c x4 + cam x5 families included, the retired camera_* names gone) plus the initial==max memory freeze. During wave-2 both {1,2} still load.

- load gate: {1,2} (fw < 0.5.0) — the wave-3 gate is {1} with the recarved semantics
- memory freeze: a module's linear memory must declare max == initial (the static-footprint freeze; the fw loader refuses initial != max and max-absent shapes with the bytes in the refusal)

## host_table

Namespace `xuansi` — a module imports host functions only from the xuansi namespace; any import outside this table fails to link (loader api refusal).

| symbol | prototype | wasm view | semantics |
|--------|-----------|-----------|-----------|
| `millis` | `void xuansi_millis(uint32_t *out)` | `(out u32* out) -> void` | Boot milliseconds written to *out (u32 — wraps at 49.7 days). |
| `delay_ms` | `void xuansi_delay_ms(uint32_t ms)` | `(i32 ms) -> void` | Millisecond delay (host busy-waits in <=50ms slices feeding the watchdog; cumulative cap of 15000ms per call — over the cap raises). |
| `gpio_set` | `void xuansi_gpio_set(uint32_t pin, uint32_t level)` | `(i32 pin, i32 level) -> void` | GPIO write (first touch auto-claims the pin as output; forbidden pins are refused). |
| `gpio_get` | `void xuansi_gpio_get(uint32_t pin, uint32_t *out)` | `(i32 pin, out u32* out) -> void` | GPIO read: level written to *out (first touch auto-claims as input). |
| `spi_config` | `void xuansi_spi_config(uint32_t *out, uint32_t slot, uint32_t sclk_pin, uint32_t sclk_hz, int32_t mosi, int32_t miso, uint32_t mode, uint32_t no_cs)` | `(out u32* out, i32 slot, i32 sclk_pin, i32 sclk_hz, i32 mosi, i32 miso, i32 mode, i32 no_cs) -> void` | SPI master bus self-configuration (slot=0\|1; mode 0..3; mosi/miso must be LEGAL pins 0..47 — the v1 engine's pins_valid wants that on all three bus pins, so the frozen doc's "mosi/miso=-1 = absent" convention is NOT actually accepted (rc=-3 "pin banned/invalid/ duplicate"; the on-board B10-T6 conflict vector hit it): pass a legal UNWIRED pin instead (st7789 uses 14); -1 semantics deferred to abi=2. no_cs!=0 -> CS-less, no_cs=0 is REFUSED on abi=1 — the CS bus is an abi=2 addition). Status 0 is written to *out (non-zero would have raised before returning). |
| `spi_xfer` | `void xuansi_spi_xfer(uint32_t *out, uint32_t slot, uint32_t wbuf_off, uint32_t wlen, uint32_t rbuf_off, uint32_t rcap)` | `(out u32* out, i32 slot, i32 wbuf_off, i32 wlen, i32 rbuf_off, i32 rcap) -> void` | SPI full duplex: w/r are linear-memory offsets; the transferred byte count is written to *out (rcap=0 = tx-only; wlen=0 = rx-only; both non-zero requires them equal). |
| `i2c_config` | `void xuansi_i2c_config(uint32_t *out, uint32_t slot, uint32_t sda, uint32_t scl, uint32_t hz)` | `(out u32* out, i32 slot, i32 sda, i32 scl, i32 hz) -> void` | I2C master self-configuration: slot=0\|1, sda/scl claimed with prim=i2c (owner="wasm", loader session), hz 10k..1M. Status 0 is written to *out (a failure raises before returning — the i2cslave fixture interlocks 10/11 still apply). |
| `i2c_write` | `void xuansi_i2c_write(uint32_t *out, uint32_t slot, uint32_t addr, uint32_t wbuf_off, uint32_t wlen)` | `(out u32* out, i32 slot, i32 addr, i32 wbuf_off, i32 wlen) -> void` | I2C pure write: 7-bit addr, a single wlen <= 4096B; the written byte count is written to *out. |
| `i2c_read` | `void xuansi_i2c_read(uint32_t *out, uint32_t slot, uint32_t addr, uint32_t rbuf_off, uint32_t rcap)` | `(out u32* out, i32 slot, i32 addr, i32 rbuf_off, i32 rcap) -> void` | I2C pure read: the read byte count is written to *out. |
| `i2c_writeread` | `void xuansi_i2c_writeread(uint32_t *out, uint32_t slot, uint32_t addr, uint32_t wbuf_off, uint32_t wlen, uint32_t rbuf_off, uint32_t rcap)` | `(out u32* out, i32 slot, i32 addr, i32 wbuf_off, i32 wlen, i32 rbuf_off, i32 rcap) -> void` | I2C combined transaction (repeated start): write(wbuf,wlen) -> restart -> read(rbuf,rcap) — the atomic register-addressed read form (same shape as SCCB_Read; the two-step fallback is module code). The read byte count is written to *out. |
| `cam_xclk` | `void xuansi_cam_xclk(uint32_t *out, uint32_t hz)` | `(out u32* out, i32 hz) -> void` | XCLK clock out on pin 15 (LCD_CAM direct drive; hz must divide 160MHz with a divider <= 255 — 20MHz is canonical). The GC2145 SCCB side needs a 1-2s warmup after the clock starts (the module handles it with delay_ms). Status 0 is written to *out. |
| `cam_start` | `void xuansi_cam_start(uint32_t *out, uint32_t w, uint32_t h, uint32_t pixfmt)` | `(out u32* out, i32 w, i32 h, i32 pixfmt) -> void` | Parallel capture stream start: claims the 11 DVP pins + LCD_CAM + GDMA + the PSRAM frame buffer (geometry decided by the module profile; if XCLK is not held it auto-starts the clock at the remembered hz). This wave's only pixfmt is 0=RGB565 (anything else raises). Below the conn floor = full rollback + raise. Status 0 is written to *out. |
| `cam_stop` | `void xuansi_cam_stop(uint32_t *out)` | `(out u32* out) -> void` | Full off: stop stream -> release the 11 DVP + XCLK pins (idempotent). Status 0 is written to *out. |
| `cam_frame_get` | `void xuansi_cam_frame_get(uint32_t *len_out, uint32_t buf_off, uint32_t cap, uint32_t *w_out, uint32_t *h_out)` | `(out u32* len_out, i32 buf_off, i32 cap, out u32* w_out, out u32* h_out) -> void` | Single frame copy: hold fb -> copy into buf (<=cap) -> return fb; the frame's byte count / width / height are written to the len_out, w_out and h_out outs (geometry rides the call). Frame over cap raises; serialized with cam_start/stop via the shared camera lock (an op in flight = busy, a 3s sliced wait then a retryable raise). |
| `cam_frame_info` | `void xuansi_cam_frame_info(uint32_t *w_out, uint32_t *h_out, uint32_t *pixfmt_out, uint32_t *on_out)` | `(out u32* w_out, out u32* h_out, out u32* pixfmt_out, out u32* on_out) -> void` | Read-only probe (does not touch the sensor): {w,h,pixfmt,on} written to the four outs (stream not started -> w=h=0, on=0; pixfmt 0 = RGB565). |
| `stream_send` | `void xuansi_stream_send(uint32_t *out, uint32_t buf_off, uint32_t len)` | `(out u32* out, i32 buf_off, i32 len) -> void` | Module-facing stream (host stamps the module name into the frame preamble); the accepted byte count is written to *out. |

Retired (kept compilable for historical modules; a binary importing them is refused at link time):

- `camera_frame_get` — renamed onto cam_frame_get at the camera migration (geometry now rides the frame call)
- `camera_info` — renamed onto cam_frame_info (reshaped to w/h/pixfmt/on)

## call_convention

- **module_shape**: wasm32 (wasi-sdk, no WASI imports); exports xuansi_abi() -> u32 (the loader calls it at instantiate and refuses anything the gate does not accept), xuansi_alloc(u32 n) -> u32 (linear-memory offset for the host-written args; 0 = refusal), and entry functions fn(ptr: u32, len: u32) -> u64
- **args**: the host serializes the caller's arguments as ONE msgpack array, calls xuansi_alloc, writes it, then calls fn(ptr, len)
- **result**: the entry returns a single packed u64 = (result_ptr << 32) | result_len pointing at the module's own msgpack result (C has no multi-value returns — the pair rides one i64); len=0 -> Lua nil
- **out_params**: every host function returns void — results are written through out-parameters into the module's linear memory (the host bounds-checks and writes directly; the module reads back as ordinary wasm memory). Buffer parameters are linear-memory offsets (u32). Module authors: land out-params in .bss/heap, not wasm stack slots (portable across the interp and AOT lanes)
- **errors**: the host error channel is the wasm exception (message prefix xuansi.<fn>) — surfaced at the Lua face as class=rt
- **alloc_convention**: the host calls xuansi_alloc on EVERY call-with-args; a bump-only allocator is a hidden per-call budget (~230 calls at small-args scale). Convention: allocations <= 128B reuse one fixed slot (safe — the fw host serializes all wasm calls); larger ones use the bump arena (see any module's ARGS_SLOT_SZ)
- **limits**: args/result msgpack <= 8192B each; container depth <= 8; module file <= 256KB; linear memory 256KB initial default (per-module pinnable), max == initial (the freeze)

## value_domain

- **kinds**: nil, boolean, integer (int64), string, array, map, float64
- **float64**: msgpack f64 (0xcb) both ways — eval/submit/job values and wasm call args/results. float32 (0xca) stays OUT of domain (landing it would silently lose precision)
- **integers**: always int64; exact for |v| <= 2^53 (binary64)
- **non_finite**: NaN and +/-Inf are honestly refused at the bridge — 'float value must be finite (JSON/msgpack carry no NaN/±Inf); rescale or send as {sign,exponent,mantissa} integers'
- **mixed_keys**: mixed/non-string map keys are out of domain (rt); depth > 8 is out of domain (rt)

## lifecycle



| event | semantics |
|-------|-----------|
| `load` | instantiates (after the abi gate + the initial==max memory gate + the admission pre-check: sum(loaded footprints) + candidate vs wasm_memory_budget_bytes — the refusal carries the full arithmetic); claims nothing (first host call claims) |
| `unload` | destroys the instance, releases NOTHING else — pin/bus claims survive (owner=wasm, loader session); a DVP camera stream also survives (board-level resource, not instance state) |
| `session_reset` | unloads every instance of the loader session AND releases its claims (incl. i2c pins and the DVP stream via the teardown chain) |
| `vm_reopen` | instances SURVIVE (board-level resource); claims released to a safe state and re-claimed on next host call; SPI slot configs are lost — the module must re-run spi_config |
| `script_death` | instances survive (their claims' owner is 'wasm', not the script name) |
| `reboot` | everything zeroed |
| `claims_discipline` | gpio/spi/i2c claims live for the module's whole loaded lifetime; cam pins (xclk + 11 DVP) are claimed owner=camera at cam_start/cam_xclk and released by cam_stop (idempotent) or the teardown chains |

## error_faces

- **classes**: api / rt / oom / compile / budget / timeout / protocol
- **api**: name-domain/file/magic/export/abi/mismatch/duplicate/instance-table/file-size/not-loaded/no-such-fn refusals at the loader face; messages carry the [api] prefix
- **rt**: marshalling domain (over cap, depth > 8, out-of-domain values), xuansi_alloc refusal, out-of-bounds result pointer, wasm traps (message passed through), and EVERY native host refusal (prefix xuansi.<fn>)
- **oom**: Lua VM heap cap exceeded (class=oom, refusal carries the effective cap; see lua_heap_max)
- **budget**: eval/script budget exceeded — the refusal reports the numbers ('cpu budget exceeded (budget %u ms; per-call budget_ms 1..30000 or budget.set)')
- **admission**: the load() pre-check refusal carries the arithmetic: loaded + candidate vs budget, with the unload-or-shrink fix in the message
- **never_silent**: no silent fallbacks anywhere in this contract: a refused .aot never degrades to .wasm, a refused ABI never loads an older lane, OOM never truncates silently

## lua_boundaries

The eight boundary conditions that are learned by refusal unless written down (spec §4.14):

| # | rule | detail |
|---|------|--------|
| 1 | budget is a wall clock, reset per tick, and blocking counts | the eval/script budget measures WALL time including time blocked inside a native call — a blocking call inside a tick eats the budget just like a spin loop. Per-call override: budget_ms 1..30000 on eval/submit; session budgets via budget.set (0..4000). The refusal reports the numbers. |
| 2 | return value domain (post-float) | values crossing the bridge (eval/submit/job value, wasm call args/results) are nil/bool/int64/string/array/map/float64; integers exact to |v|<=2^53; NaN/±Inf refused with the rescale hint; float32 never lands. |
| 3 | yield does not cross require | coroutine.yield cannot cross a require boundary (require is a C boundary). Resident-script pattern: /drv/*.lua modules do setup + timer registration and RETURN; the yield loop belongs to the launcher script. |
| 4 | args bumps are never returned | every wasm call-with-args consumes xuansi_alloc; the bump arena never gives memory back. Polling modules reuse a small-args slot (<= 128B) or exhaust the heap in ~230 calls. This is a module-author contract, invisible at the Lua face until it bites. |
| 5 | 16 timers, globally shared; queue overflow drops are console-only | the 16 timer slots are shared across ALL scripts; the per-owner event queue holds 16 entries and overflow DROPS (logged to console only — the Lua face never sees the loss). Design polling cadence against shared scarcity. |
| 6 | script lifecycle reaches the events face | script started/finished/killed events (source=script) reach the events face; killed carries the cause (budget/error/stop). A budget-killed script's verdict rides the event, not the eval reply. |
| 7 | synchronous ops stand silent; slices keep the box alive | a synchronous op is silent on its connection for its whole run; internal waits are sliced <= 100ms with watchdog feeding. Hard walls: the conn-task watchdog (5s) and budget.set (0..4000). Long work belongs on submit (async, job events carry the result). |
| 8 | unload does not release claims | bench.wasm.unload destroys the instance but no pin/bus claims (owner=wasm, loader session) and no DVP stream. Claims release on session_reset (which ALSO unloads the session's instances — the cleanup ritual is unload -> release pins -> session_reset). |

## bench_signatures

- `bench.wasm.load(name) -> true` — instantiate /drv/<name>.aot|.wasm (the .aot preferred when both exist); gates: abi, initial==max memory, admission arithmetic
- `bench.wasm.call(name, fn, args...) -> any` — the marshalling convention above (msgpack array in, packed u64 out)
- `bench.wasm.unload(name) -> true` — destroy the instance; claims survive (boundary #8)
- `bench.wasm.list() -> {names}` — loaded instances
- `bench.wasm.info(name) -> {abi, loaded_by, session, mem_bytes[, aot='psram', text_bytes, pool_bytes]}` — identity + the static footprint trio (.aot lane carries text/pool)
- `bench.wasm.xfer(src_name, src_off, dst_name, dst_off, len) -> n` — cross-instance memcpy with explicit ownership (no sharing, no escape); len=0 is a legal no-op; src==dst self-move uses memmove semantics; bounds checked against each instance's CURRENT extent
- `bench.wasm.plan(names) -> {rows=[{name, mem_bytes, text_bytes?, pool_bytes?}], loaded_bytes, total_bytes, budget_bytes, fits}` — admission dry-run — zero side effects; compare against budget before loading. total_bytes is the union of loaded and candidate footprints — an already-loaded candidate is counted once, so planning a loaded module projects zero growth
- `bench.millis() -> u32` — boot milliseconds (wraps at 49.7 days) — the Lua-face timing source (a hclock wasm module is no longer needed for this)
- `bench.timer_ms(period_ms, fn)` — resident-scripts-only periodic event source (since batch 5): period_ms 1..3600000, fn any function; the esp_timer fires periodically and each fire resumes the owner coroutine with fn as the yielded event value (local f = coroutine.yield()); budget counts per resume (event_ms); all of a script's timers are deleted when it dies (stop/teardown/budget/error/normal end); eval context is refused ('resident scripts only'); table full = 16 slots shared across ALL scripts (boundary #5), create failure is the esp_timer error
- `bench.caps() -> caps` — the caps table: fw/api_version/batch, primitives.<prim>.ops, pins.banned, limits (the numeric full table — see caps_keys), heap
- `bench.gc(mode [, pause [, stepmul]])` — the collectgarbage mirror (gc mode/knobs)
- `bench.api_version` — the bench API version constant (= 1, the same single axis as the module ABI post-recarve)

Per-family op lists (the caps `primitives.<prim>.ops` face):

| family | ops |
|--------|-----|
| `bench.gpio` | `mode`, `write`, `read`, `on` |
| `bench.uart` | `setup`, `write`, `read`, `on` |
| `bench.i2c` | `setup`, `write`, `read`, `wreg`, `rreg`, `scan` |
| `bench.spi` | `setup`, `xfer`, `write`, `read` |
| `bench.pwm` | `setup`, `duty`, `stop` |
| `bench.adc` | `setup`, `read`, `read_mv` |
| `bench.rmt` | `setup`, `tx`, `rx` |
| `bench.events` | `on`, `off`, `next` |

## caps_keys

18 keys in caps.limits (17 numeric + adc2_note), identical across the three exits (Lua bench.caps / msgpack caps op / console CAP: lines). Same keys, same values, same order — any drift is a bug.

| key | value | note |
|-----|-------|------|
| `resident_max` | 8 | resident Lua scripts cap |
| `eval_budget_ms` | live (live) | session eval budget (budget.set domain 0..4000) |
| `event_budget_ms` | live (live) | session event/callback budget (budget.set domain 0..4000) |
| `eval_budget_max_ms` | 30000 | per-call budget_ms cap (the budget_ms request key 1..30000) |
| `psram_cap_kb` | live (live) | the Lua VM allocator cap in KB (diag.cap adjustable; 0 = unlimited; boot default 4096) |
| `aot_psram_exec_budget_bytes` | live (live) | the SURGERY .aot text PSRAM exec window (drops while a surgery container is loaded, recovers on unload) |
| `timers_max` | 16 | timer slots shared across ALL scripts (Lua boundary #5) |
| `pending_jobs_max` | 4 | pending async jobs (full = busy refusal) |
| `event_subs_max` | 16 | event subscriptions per owner |
| `event_queue_depth` | 16 | per-owner event queue depth — overflow DROPS, console-only (Lua boundary #5) |
| `event_next_timeout_ms` | 5000 | events.next timeout ceiling |
| `name_max_bytes` | 31 | script/module name cap |
| `tag_max_bytes` | 31 | job tag cap |
| `lua_heap_max` | live (live) | the Lua heap cap in bytes (boot default 4194304; diag.cap 0..65536KB, 0 = unlimited) — the account the OOM refusal reports |
| `lua_heap` | live (live) | current Lua VM heap live bytes |
| `wasm_memory_budget_bytes` | 2097152 | the WAMR pool total account — the admission budget sum(footprints) is checked against (the account is live: load/unload maintain it; the pool itself is fixed at boot) |
| `wasm_inst_slots` | 16 | instance-table mapper slots (the memory account is the governing limit; a full table is the mapper's last-line api refusal) |
| `adc2_note` | live (live) | NOTE key (not a limit): the ADC2 x WiFi gate status string, one of two frozen strings |

