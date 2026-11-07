# xuansi-drivers

Public library of **WASM driver modules** for the xuansi bench (an
ESP32-S3 hardware probe driven by an agent over the
[xuansi-mcp](https://github.com/wan-cloud/xuansi-mcp) daemon). The
firmware hot-loads these modules onto the board's WAMR runtime, where
they drive GPIO/SPI/camera hardware through a small frozen host API.

Current contents: `hello@1.0.0` — the ABI's reference module (full
host-surface tour: echo/add marshalling, GPIO, timing, stream, SPI
loopback, camera frame).

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

Adding symbols or changing semantics means `abi = 2` — existing symbol
semantics never move.

## Delivery track: `.wasm` bytecode

The deliverable artifact is the **interpreter bytecode** (`.wasm`),
listed in `index.json` and deployed to `/drv/<name>.wasm` on the board.
`.aot` files are still built by CI (wamrc, hash-recorded) as
**auxiliary** artifacts — future-ready, but the current board stack
honestly refuses to *execute* them (an upstream AOT-in-PSRAM execution
defect on ESP32-S3; when it is fixed upstream, the `.aot` track turns
back on with zero module-side changes). The `.aot` is therefore never
listed as a deployable file in `index.json`.

## Repository layout

```
index.json                  # the module index (schema below; single
                            # source for "what is published")
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

`kind` is `"driver" | "sensor-profile" | "algorithm"`.

**Reserved: `sensor-profile`** (planned 0.3.0) — hot-loadable sensor
description modules (SCCB register sequences, timing parameters, frame
buffer geometry, pixel format) so a new DVP sensor becomes a data file
instead of a firmware rebuild. It is the first planned `abi = 2`
addition; the `kind` field is reserved now.

## Toolchain (pinned)

| piece | pin |
|---|---|
| wasi-sdk | **24.0** (LLVM 18.1.2) — [release tarball](https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24/wasi-sdk-24.0-x86_64-linux.tar.gz), sha256 `c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5` |
| wamrc | **WAMR-2.4.0** (tag `WAMR-2.4.0`, commit `47c7c85a…`) built from source against [espressif/llvm-project](https://github.com/espressif/llvm-project) `xtensa_release_18.1.2` (commit `344b928e…`) — prebuilt wamrc releases lack the xtensa backend |

Build flags are frozen in `build.sh` (`-O2 -fno-jump-tables -nostdlib`,
`--no-entry --initial-memory=262144 --max-memory=1048576
--global-base=1024`, `--strip-debug --export-dynamic`); exports are
declared with `export_name` attributes. Rebuilds are deterministic —
CI re-derives every hash and compares it against `index.json` and each
`meta.json` on every push.

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
