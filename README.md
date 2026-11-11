# xuansi-drivers

Public library of **WASM driver modules** for the xuansi bench (an
ESP32-S3 hardware probe driven by an agent over the
[xuansi-mcp](https://github.com/wan-cloud/xuansi-mcp) daemon). The
firmware hot-loads these modules onto the board's WAMR runtime, where
they drive GPIO/SPI/camera hardware through a small frozen host API.

## Module index

Per-module documentation lives **beside each artifact** at
`modules/<name>/<ver>/doc.md` (ops table, measured numbers, wiring —
the same file the daemon's `xuansi_module_doc` face reads through the
meta). This table is the one-glance index:

| module | version | release | kind | one-liner |
|--------|---------|---------|------|-----------|
| [hello](modules/hello/1.0.0/doc.md) | 1.0.0 | test-canary | driver | the ABI's reference module — full host-surface tour; imports the retired camera names on purpose (the loader's retired-import refusal vector) |
| [ledwatch](modules/ledwatch/2.0.0/doc.md) | 2.0.0 | production | algorithm | QVGA camera frames → LED on/off regions (connected components), consumed by the closed-loop tasks |
| [ledwatch](modules/ledwatch/1.0.0/doc.md) | 1.0.0 | production | algorithm | the superseded abi=1 build (retired camera imports — link-refused on fw ≥ 0.3.0) |
| [st7789](modules/st7789/1.0.0/doc.md) | 1.0.0 | production | driver | ST7789V 240x240 TFT panel over CS-less SPI — the first `driver` module |
| [gc2145](modules/gc2145/1.0.0/doc.md) | 1.0.0 | production | sensor-profile | the GC2145 bring-up sequence as module data + code — the first abi=2 module |
| [jpge](modules/jpge/1.0.0/doc.md) | 1.0.0 | production | codec | camera frames → JPEG or raw RGB565 out the stream — the first `codec` module |

`release` is the index's `kind` axis (`production` | `test-canary` —
what the daemon surfaces verbatim at fetch/push time); the module
kind (`driver` / `sensor-profile` / `algorithm` / `codec`) is the
descriptive vocabulary carried by each meta.json and doc.md.

## Module ABI (abi = 1 — the 2026-11-11 recarve)

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
- imports host functions only from the `xuansi` namespace (16 active
  symbols — see `src/xuansi_host.h`, the per-symbol template; the
  signature source is the firmware's `tools/wasm/xuansi_host.h`,
  mirrored here). Every host function is `void`: results are written
  through out-parameters into the module's linear memory; errors
  surface as wasm exceptions.

**The B15 recarve (ruled 2026-11-11)**: the ABI number is **1
again** — a clean recarve. The old abi=1 and abi=2 are retired with
fw 0.5.0 (wave-3; during wave-2 the load gate still accepts {1,2}).
The recarved 1 is the **unified host table** (the abi=2 i2c×4 + cam×5
families included; the retired `camera_frame_get`/`camera_info` names
gone) plus the **initial == max linear-memory freeze** (a static
footprint for the memory-admission budget — the fw loader refuses
`initial != max` and max-absent shapes with the bytes in the
refusal). One contract axis from here on: module ABI and bench
`api_version` are the same 1.

Limits (frozen): args/result msgpack ≤ 8192B, container depth ≤ 8,
module file ≤ 256KB, module memory = one fixed allocation
(initial == max; 256KB the default, per-module pinnable up to 1MB).
The value domain carries float64 (binary64; integers exact to
|v| ≤ 2^53; NaN/±Inf honestly refused).

The full machine-readable reference — host table, call convention,
value domain, lifecycle, error faces, the eight Lua boundary
conditions, the bench.* signature table, the caps key table — is
generated single-source into [`abi-reference.json`](abi-reference.json)
(+ [`abi-reference.md`](abi-reference.md), human rendering) by
[`tools/gen-abi-reference/`](tools/gen-abi-reference/); the daemon
embeds the JSON for its `xuansi_abi_reference` tool.

Historical abi=2 note (kept for the record; the rename wave's
one-time exemption): `i2c_config`/`i2c_write`/`i2c_read`/
`i2c_writeread` and `cam_xclk`/`cam_start`/`cam_stop`/
`cam_frame_get`/`cam_frame_info` replaced the retired
`camera_frame_get`/`camera_info`; an abi=1 binary importing the
retired names is refused at link time.

## Delivery track: dual artifacts (`.wasm` + `.aot`)

Every module ships **two** deliverable artifacts since P3-T12
(spec §8-4 dual-product ruling):

- **`.wasm`** — the interpreter bytecode, architecture-neutral, loads
  on any fw with a wasm loader (>=0.2.0). The universal fallback.
- **`.aot`** — the compiled xtensa/esp32s3 container, per-target
  (meta `aot` namespace: `"aot": {"esp32s3": {...}}` — future targets
  add keys, no schema migration). Built by the **toolchain-v3 surgery
  wamrc** (below): its containers carry the split literal pool
  (`xuansi.literal` CUSTOM section, `literal_size == 0`), which fw
  **0.3.0+** classifies SURGERY and places in **PSRAM exec** — the
  10x jpge-class speedup with no internal-SRAM budget gate. Older fw
  honestly refuses the shape (the pool section predates RELOCATION by
  construction — an old-runtime honest refusal, not a crash).

Both are pinned in `index.json` `files[]` (the daemon's
`xuansi_driver_fetch` lands both and verifies both hashes) and in each
module's `meta.json` (`deliverable` + `aot.esp32s3`). CI rebuilds both
from the pinned toolchain and hard-compares — the single-source lock:
the release-asset wamrc rebuilding the same `.wasm` must reproduce the
committed `.aot` bit-for-bit. The loader prefers the `.aot` when both
are on the board (kind-aware since P3-T11: SURGERY → PSRAM exec).
The STOCK `.aot` lane (toolchain-v2 → fw-internal SRAM, within the
`aot_exec_budget_bytes` gate) is fw 0.3.x-only — retired in fw 0.4.0
(the internal-SRAM exec lane is gone): a stock `.aot` is honestly
refused at load with the reason and the fix in the error — never
silently swapped for the `.wasm`.

Two lane notes carried from the toolchain surgery tasks (T9/T10):

- **~32KB per-module pool ceiling** — the literal pool lives in the
  data heap as one `xuansi.literal` section; the LLVM-side `addmi`
  expansion bounds the addressable pool span at roughly 32KB, so a
  module whose literal mass grows past that is the practical size
  ceiling of the surgery lane (none of the modules approaches it;
  the compiler fails loudly past the encodable span, never silently).
- **default-ON pairing** — the surgery codegen is the xtensa DEFAULT
  on the toolchain-v3 wamrc, and it is paired by construction:
  v3 wamrc ↔ fork LLVM (`xuansi/a3-surgery`) ↔ WAMR fork
  (`xuansi/base-2.4.0`) ↔ fw >=0.3.0 (the kind-aware loader). Using
  any stock element against that chain (an old wamrc, an old fw)
  either honestly refuses or falls back to the narrower stock lane
  (fw 0.3.x only — fw 0.4.0 retired it and refuses instead) — there
  is no silent mixing. `--disable-xuansi-surgery` on the fork wamrc
  rebuilds the stock lane explicitly.

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
  one-call still trips) — which is exactly why the interpreter lane
  pins QVGA (note below).
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
(see the gc2145 doc.md).

**VGA is the measured recipe shape** (P3 AOT landed, measured
2026-11-11): on the surgery-`.aot` lane — what the paired push loads —
the in-module VGA encode measured **1229ms median at q80 / 1179ms at
q60** (n=3 each, same-scene board run against the canonical
toolchain-v3 `jpge.aot`), comfortably inside the recipe's 4s budget.
The recipe block above therefore pins `framesize:"VGA"` at its
published value. The interpreter lane (`.wasm` — older firmware, or an
explicit single-artifact push) remains honestly QVGA-class: interp
encodes measured ~3.1s at QVGA q60/q80 on the same scene and same run
(~10× the AOT lane per cell), and a single interp VGA encode still
trips the 5s VM-task watchdog — re-confirmed on that same run
(no-reply + reboot; the board self-heals). On the `.wasm` lane pass
`framesize:"QVGA"`; on the default paired push, VGA is the shape to
use.

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
co-residency under a VGA scene is unmeasured (three-module co-residency
under a VGA scene measured green on the 2026-11-11 P3 run); the
interpreter lane pins QVGA (note above), which is the shape the
four-module runs were measured at. Since the B15 wave the account is
explicit: every meta carries the module's static footprint
(`footprint`: initial/max from the `.wasm` memory section; text/pool
from the `.aot` container) and the fw load gate pre-checks
Σ(footprints) against `wasm_memory_budget_bytes` with the arithmetic
in the refusal (`bench.wasm.plan` is the dry-run).

**No camera tool on the MCP face — assemble state from the modules**:
the dedicated camera status/capture/off tools are gone; camera
knowledge now lives entirely in the modules. Whatever an agent (or a
HIL check) wants to know about camera state, it assembles from the
two existing paths: `bench.wasm.call("gc2145", "info")` — the host
camera-state passthrough plus the module's last framesize — and the
`diag.pins` bench op, the pin-claim table (which owner holds which
pin).

## Repository layout

```
index.json                  # the module index (schema below; single
                            # source for "what is published")
abi-reference.json/.md      # the GENERATED ABI reference (host table,
                            # conventions, boundaries, bench/caps tables)
                            # — tools/gen-abi-reference/, CI drift-locked
toolchain-index.json        # the pinned-toolchain index (see
                            # "Toolchain"); served raw to the daemon
modules/<name>/<ver>/       # built artifacts + docs, committed:
                            #   <name>.wasm           deliverable (in index files[])
                            #   <name>.aot            auxiliary (hash in meta only)
                            #   <name>.meta.json      entry table + provenance
                            #                       + doc/release/footprint fields
                            #   doc.md                the module's doc face
                            #                       (ops/measured/wiring — the
                            #                       extractor embeds it into meta)
src/xuansi_host.h           # shared ABI declarations (module-side mirror
                            # of the fw signature source)
src/<name>/<name>.c         # module source (canonical home)
src/<name>/test_<name>.c    # native unit test + stub host
src/<name>/VERSION          # e.g. "1.0.0"
build.sh                    # wasi-sdk + wamrc build (all modules or by name;
                            # --next = the recarved-ABI-1 lane -> staging/)
staging/                    # git-ignored: recarved-ABI-1 products awaiting
                            # the wave-3 republish (NOT in the index)
tools/extract_doc.py        # doc.md -> meta doc/release/footprint embed +
                            # artifact footprint parser (CI parity)
tools/gen-abi-reference/    # the ABI reference generator + curated sidecar
tools/verify_index.py       # rebuild-vs-index hash comparison
tools/verify_tcindex.py     # toolchain-index.json structure check
.github/workflows/ci.yml    # CI: build / verify / native / generated
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
      "kind": "test-canary",
      "version": "1.0.0",
      "abi": 1,
      "files": [
        { "path": "modules/hello/1.0.0/hello.wasm",
          "sha256": "<hex>", "bytes": 5714 }
      ],
      "src_commit": "<git commit of src/hello/hello.c>"
    }
  ]
}
```

`kind` is the **release class** — `"production" | "test-canary"` (the
canary vocabulary the daemon passes verbatim through fetch/manifest/
push replies). The descriptive module kind (`"driver" |
"sensor-profile" | "algorithm" | "codec"`) lives in each meta.json's
`kind` and in the doc.md header. A `test-canary` entry is a published
module whose FACE is deliberately a test vector (hello@1.0.0 keeps the
retired camera imports — it exists to be refused). `fw_compat` entries
(the fw floor a module's imports need — `{"wasm": ">=0.3.0"}` for the
abi=2 host table) ride the index and meta as before.

### meta.json (per version, beside the artifacts)

The human-owned halves: identity (`name`/`kind`/`version`/`abi`),
provenance (`src_commit`, `imports`, `entries`, `linear_memory`,
`hardware`, `toolchain`, `deliverable`, `aot`, `auxiliary`,
`fw_compat`). Three fields are pipeline-managed by
`tools/extract_doc.py` (CI parity-locked against `doc.md` + the
artifacts — edit the doc, never the meta):

- `release` — the release class (mirrors the index `kind`)
- `doc` — `{ops:[{name,args,result,semantics}], measured:{...}}`
  extracted from doc.md's Ops table + Measured list (the
  `xuansi_module_doc` face reads this)
- `footprint` — the admission numbers parsed from the artifacts
  themselves: `.wasm` memory section `{initial_bytes, max_bytes}` and
  `.aot` `{initial_bytes, text_bytes, pool_bytes}` (the same walk the
  fw loader does — numbers can't drift from the bytes)

## Toolchain (pinned, two generations)

| piece | pin |
|---|---|
| wasi-sdk | **24.0** (LLVM 18.1.2) — [release tarball](https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz), sha256 `c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5` |
| wamrc (**toolchain-v3**, the default/latest) | the **surgery wamrc**: WAMR fork [wan-cloud/wasm-micro-runtime](https://github.com/wan-cloud/wasm-micro-runtime) `xuansi/base-2.4.0` (commit `17036fa0…`) built against the LLVM fork [wan-cloud/llvm-project](https://github.com/wan-cloud/llvm-project) `xuansi/a3-surgery` (commit `cf84bd31…`) — kind `xuansi-surgery-wamrc`; its `.aot` is the PSRAM-exec deliverable lane |
| wamrc (toolchain-v2, kept) | **WAMR-2.4.0** (tag `WAMR-2.4.0`, commit `47c7c85a…`) built from source against [espressif/llvm-project](https://github.com/espressif/llvm-project) `xtensa_release_18.1.2` (commit `344b928e…`) — the stock wamrc, kept for the wasm track; its `.aot` lane is retired (`fw_compat.aot ">=0.3.0 <0.4.0"` — the fw-internal-SRAM lane loads on fw 0.3.x only; fw 0.4.0+ refuses, rebuild via toolchain-v3 or push the `.wasm`) |

The machine-readable pin is [`toolchain-index.json`](toolchain-index.json)
(structure-checked by `tools/verify_tcindex.py`) — the pinned source the
daemon's `xuansi_toolchain_fetch` tool fetches, served raw from this
repository's `main`; `latest` is **toolchain-v3** (the surgery
generation). The wamrc leg-matrix per generation: v3 carries
linux-x64 + both mac legs (built in-workflow by `toolchain-v3.yml`,
which also attaches the llvm-dev package of the same lineage — the
release IS the pin chain); platforms without a built wamrc carry
`null` there (linux-arm64 and win-x64 — wasi-sdk is pinned regardless,
and the wamrc entries land as those legs are built). win-x64 wamrc:
best-effort leg failed (MSVC probe); tracked P2.

Build flags are frozen in `build.sh` (`-O2 -fno-jump-tables -nostdlib`,
`--no-entry --initial-memory=<per module> --max-memory=1048576
--global-base=1024`, `--strip-debug --export-dynamic`; hello pins the
ABI-freeze 256KB initial, ledwatch pins 11 pages — see
`init_mem_for()`); exports are declared with `export_name` attributes.
Rebuilds are deterministic — CI re-derives every hash and compares it
against `index.json` and each `meta.json` on every push. **The
`--max-memory=1048576` half is the LEGACY lane** (pre-recarve
artifacts): the recarved ABI 1 freezes `max == initial`, and
`./build.sh --next` builds that lane into `staging/` for the wave-3
republish — the committed artifacts keep the legacy flag set until
that flip, byte-for-byte.

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

`.aot` lanes (dual since P3-T12): the generations' `fw_compat.aot`
domains diverge at fw 0.4.0 — v3 `">=0.3.0"`, v2 `">=0.3.0 <0.4.0"`
(retired). **toolchain-v3** (the default) is the surgery wamrc —
`xuansi_wasm_build`'s `aot: true` knob compiles the freshly-built
`.wasm` one step further with the cache's v3 wamrc (this repo's frozen
recipe: `--target=xtensa --cpu=esp32s3 --size-level=3`) and stores
`drivers/<name>@<version>.aot` (manifest kind `wasm` — the
cache-paired build): the split-literal-pool container fw 0.3.0+ places
in **PSRAM exec**. **toolchain-v2**'s stock wamrc compiled the
narrower fw-INTERNAL SRAM lane (within the board's advertised
`aot_exec_budget_bytes` ceiling; still reachable through the
`wamrc_path` dev override, manifest kind `wasm-dev`) — fw 0.3.x only:
fw 0.4.0+ honestly refuses a stock `.aot` at load (rebuild via
toolchain-v3 or push the `.wasm` single artifact), never silently
swapped for the `.wasm`. v2 itself stays current for the wasm track
(`fw_compat.wasm ">=0.2.0"` unchanged).

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
URL=$(curl -fsSL "$IDX" | python3 -c "import json,sys; print(json.load(sys.stdin)['generations']['toolchain-v3']['platforms']['$PLAT']['wasi_sdk']['url'])")
SHA=$(curl -fsSL "$IDX" | python3 -c "import json,sys; print(json.load(sys.stdin)['generations']['toolchain-v3']['platforms']['$PLAT']['wasi_sdk']['sha256'])")
curl -fL -o sdk.tar.gz "$URL"
echo "$SHA  sdk.tar.gz" | sha256sum -c -        # must say OK (two spaces; macOS: shasum -a 256 -c -)
CACHE="${XUANSI_WASM_CACHE:-$HOME/.cache/xuansi-wasm}"
mkdir -p "$CACHE" && tar -xzf sdk.tar.gz -C "$CACHE"
```

PowerShell (windows):

```powershell
$plat = 'win-x64'   # your platform key
$a = (Invoke-RestMethod https://raw.githubusercontent.com/wan-cloud/xuansi-drivers/main/toolchain-index.json).generations.'toolchain-v3'.platforms.$plat.wasi_sdk
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
./build.sh              # build all published modules -> modules/<name>/<ver>/
./build.sh hello        # one module
./build.sh --check      # rebuild into a scratch dir, compare hashes
./build.sh --next       # the recarved-ABI-1 lane (initial==max) -> staging/
                        #   (st7789 migration + hclock; NOT published — wave-3)
python3 tools/extract_doc.py --check      # doc.md <-> meta doc/release/footprint parity
python3 tools/gen-abi-reference/gen.py --check   # abi-reference drift-lock
cc -DXUANSI_NATIVE_TEST -Wall -Wextra -I src -I src/hello \
   src/hello/test_hello.c -o /tmp/test_hello && /tmp/test_hello
```

The native test compiles the module with the host toolchain against a
stub host (`XUANSI_NATIVE_TEST` seam in the module sources and
`xuansi_host.h`; the wasm32 build is unchanged — the committed
`.wasm` is byte-identical to the pre-seam build).

## CI

Four legs on every push and PR (`.github/workflows/ci.yml`):

1. **build** — install the pinned toolchain (cached), `./build.sh`,
   upload the artifacts;
2. **verify** — rebuild from scratch and compare every hash against
   `index.json` (hard gate on deliverable `.wasm`) and each
   `meta.json` (auxiliary `.aot`);
3. **native** — compile and run every `src/*/test_*.c` with the host
   gcc against its stub host;
4. **generated** — the two parity locks: `tools/extract_doc.py
   --check` (doc.md ↔ meta doc/release/footprint ↔ artifact bytes)
   and `tools/gen-abi-reference/gen.py --check` (abi-reference
   byte-stable against `src/xuansi_host.h` + the sidecar).

The first run after a cache miss builds wamrc's LLVM from source
(~1h); cached afterward.

## License

Apache-2.0 (see `LICENSE`).
