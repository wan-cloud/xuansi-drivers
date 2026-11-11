#!/usr/bin/env python3
"""gen.py — the xuansi ABI reference generator (B15 W2-T3, spec §4.14).

Single-source generation, no hand-written drift:

  src/xuansi_host.h  (the module-side mirror of the firmware's
        tools/wasm/xuansi_host.h — the SIGNATURE source: symbols,
        C prototypes, per-symbol comments)
        +
  sidecar.json  (the CURATED source: semantics, call convention,
        value domain, lifecycle, error faces, the eight Lua boundary
        conditions, bench.* signatures, the caps key table)
        ||
        gen.py
        ||
  abi-reference.json + abi-reference.md   (checked in at the repo root)

CI drift-lock: regenerating must reproduce the checked-in files
byte-for-byte (gen.py --check). The provenance block records the
sha256 of both inputs, so any header or sidecar edit visibly changes
the output and the lock catches it. No timestamps anywhere.

Usage:
  gen.py                write abi-reference.json + abi-reference.md
  gen.py --check        regenerate in memory and byte-compare (CI leg)
  gen.py --header PATH  read a different xuansi_host.h (e.g. verify the
                        firmware's own copy against the mirror locally)
"""
import hashlib
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
HEADER = ROOT / "src" / "xuansi_host.h"
SIDECAR = HERE / "sidecar.json"
OUT_JSON = ROOT / "abi-reference.json"
OUT_MD = ROOT / "abi-reference.md"

# --------------------------------------------------------------------------
# header parsing — the symbol face
# --------------------------------------------------------------------------

# Both declaration forms: the drivers mirror's XUANSI_IMPORT(...) macro and
# the firmware header's raw __attribute__((import_module, import_name)) —
# so --header can point gen.py at the firmware's own copy for a local
# parity check of the mirror sync.
_IMPORT = re.compile(
    r'(?:(?P<comment>/\*(?:(?!\*/)[\s\S])*\*/)\s*)?'
    r'(?:XUANSI_IMPORT\("xuansi",\s*"(?P<symbol>[a-z0-9_]+)"\)'
    r'|__attribute__\(\(import_module\("xuansi"\),\s*'
    r'import_name\("(?P<symbol2>[a-z0-9_]+)"\)\)\))\s*'
    r'extern\s+void\s+(?P<fn>[a-z0-9_]+)\((?P<args>[^)]*)\);',
)


def _wasm_param(p: str) -> str:
    p = p.strip()
    name = p.split()[-1].lstrip("*")
    if "*" in p:
        return f"out u32* {name}"
    if "int32_t" in p:
        return f"i32 {name}"
    return f"u32 {name}"


def parse_header(text: str):
    symbols = []
    for m in _IMPORT.finditer(text):
        raw = m.group("comment") or ""
        raw = re.sub(r"^\s*/\*+\s*", "", raw)
        raw = re.sub(r"\s*\*+/\s*$", "", raw)
        comment = " ".join(
            line.strip().lstrip("*").strip()
            for line in raw.splitlines()
        ).strip()
        # drop the section-divider comments (---- ...) from semantics
        comment = re.sub(r"^-+\s*", "", comment).strip()
        args = " ".join(m.group("args").split())
        params = [a.strip() for a in args.split(",") if a.strip()]
        proto = f"void {m.group('fn')}({', '.join(params)})"
        symbols.append({
            "symbol": m.group("symbol") or m.group("symbol2"),
            "prototype": proto,
            "wasm": "(" + ", ".join(_wasm_param(p) for p in params) + ") -> void",
            "semantics": comment,
        })
    if not symbols:
        raise SystemExit(f"{HEADER}: no XUANSI_IMPORT blocks found — parse drift")
    return symbols


def sha256_text(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()


# --------------------------------------------------------------------------
# assemble
# --------------------------------------------------------------------------

def build(header_text: str, sidecar: dict):
    symbols = parse_header(header_text)
    retired_names = {k: v for k, v in sidecar["retired_symbols"].items()
                     if not k.startswith("_")}
    active, retired = [], []
    for s in symbols:
        if s["symbol"] in retired_names:
            s = dict(s)
            s["note"] = retired_names.pop(s["symbol"])
            retired.append(s)
        else:
            active.append(s)
    if retired_names:
        raise SystemExit(f"sidecar.retired_symbols names not in header: "
                         f"{sorted(retired_names)}")
    ref = {
        "provenance": {
            "header": "src/xuansi_host.h",
            "header_sha256": sha256_text(header_text),
            "sidecar": "tools/gen-abi-reference/sidecar.json",
            "sidecar_sha256": sha256_text(json.dumps(
                sidecar, indent=2, ensure_ascii=False, sort_keys=True)),
            "generator": "tools/gen-abi-reference/gen.py",
            "note": "regenerate with tools/gen-abi-reference/gen.py; the "
                    "checked-in outputs are the single source the daemon "
                    "embeds (xuansi_abi_reference) — CI byte-compares",
        },
        "abi": sidecar["abi"],
        "sections": {
            "host_table": {
                "namespace": "xuansi",
                "import_rule": "a module imports host functions only from "
                               "the xuansi namespace; any import outside "
                               "this table fails to link (loader api refusal)",
                "error_channel": "every host function is void — results "
                                 "through out-parameters into linear memory; "
                                 "errors as wasm exceptions (prefix "
                                 "xuansi.<fn>), Lua face class=rt",
                "symbols": active,
                "retired": retired,
            },
            "call_convention": sidecar["call_convention"],
            "value_domain": sidecar["value_domain"],
            "lifecycle": {k: v for k, v in sidecar["lifecycle"].items()
                          if not k.startswith("_")},
            "error_faces": sidecar["error_faces"],
            "lua_boundaries": sidecar["lua_boundaries"],
            "bench_signatures": sidecar["bench_signatures"],
            "caps_keys": {
                "note": sidecar["caps_keys_note"],
                "keys": sidecar["caps_keys"],
            },
        },
    }
    return ref


# --------------------------------------------------------------------------
# markdown rendering
# --------------------------------------------------------------------------

def render_md(ref: dict) -> str:
    s = ref["sections"]
    L = []
    L.append("# xuansi ABI reference")
    L.append("")
    L.append("Generated single-source from `src/xuansi_host.h` (the symbol "
             "face — the module-side mirror of the firmware's "
             "`tools/wasm/xuansi_host.h`) plus the curated "
             "`tools/gen-abi-reference/sidecar.json`; regenerate with "
             "`tools/gen-abi-reference/gen.py` (CI drift-locks the output "
             "byte-for-byte). This is the reference the daemon's "
             "`xuansi_abi_reference` tool embeds.")
    L.append("")
    abi = ref["abi"]
    L.append(f"**ABI {abi['version']}** — {abi['note']}")
    L.append("")
    L.append(f"- load gate: {abi['load_gate_wave2']}")
    L.append(f"- memory freeze: {abi['memory_freeze']}")
    L.append("")

    L.append("## host_table")
    L.append("")
    L.append(f"Namespace `{s['host_table']['namespace']}` — "
             f"{s['host_table']['import_rule']}.")
    L.append("")
    L.append("| symbol | prototype | wasm view | semantics |")
    L.append("|--------|-----------|-----------|-----------|")
    for sym in s["host_table"]["symbols"]:
        sem = sym["semantics"].replace("|", "\\|")
        L.append(f"| `{sym['symbol']}` | `{sym['prototype']}` | "
                 f"`{sym['wasm']}` | {sem} |")
    L.append("")
    if s["host_table"]["retired"]:
        L.append("Retired (kept compilable for historical modules; a "
                 "binary importing them is refused at link time):")
        L.append("")
        for sym in s["host_table"]["retired"]:
            L.append(f"- `{sym['symbol']}` — {sym['note']}")
        L.append("")

    def kv_section(title, body, order=None):
        L.append(f"## {title}")
        L.append("")
        keys = order or [k for k in body if not k.startswith("_")]
        for k in keys:
            v = body[k]
            if isinstance(v, (list, dict)):
                L.append(f"- **{k}**: `{json.dumps(v, ensure_ascii=False)}`")
            else:
                L.append(f"- **{k}**: {v}")
        L.append("")

    kv_section("call_convention", s["call_convention"],
               ["module_shape", "args", "result", "out_params", "errors",
                "alloc_convention", "limits"])
    kv_section("value_domain", s["value_domain"],
               ["kinds", "float64", "integers", "non_finite", "mixed_keys"])

    L.append("## lifecycle")
    L.append("")
    L.append(s["lifecycle"].get("_note", ""))
    L.append("")
    L.append("| event | semantics |")
    L.append("|-------|-----------|")
    for k, v in s["lifecycle"].items():
        if k.startswith("_"):
            continue
        L.append(f"| `{k}` | {v} |")
    L.append("")

    kv_section("error_faces", s["error_faces"], ["classes", "api", "rt",
               "oom", "budget", "admission", "never_silent"])

    L.append("## lua_boundaries")
    L.append("")
    L.append("The eight boundary conditions that are learned by refusal "
             "unless written down (spec §4.14):")
    L.append("")
    L.append("| # | rule | detail |")
    L.append("|---|------|--------|")
    for b in s["lua_boundaries"]:
        L.append(f"| {b['id']} | {b['rule']} | {b['detail']} |")
    L.append("")

    bs = s["bench_signatures"]
    L.append("## bench_signatures")
    L.append("")
    for group in ("wasm", "clock", "meta"):
        for e in bs[group]:
            L.append(f"- `{e['sig']}` — {e['semantics']}")
    L.append("")
    L.append("Per-family op lists (the caps `primitives.<prim>.ops` face):")
    L.append("")
    fam = {k: v for k, v in bs["families"].items() if not k.startswith("_")}
    L.append("| family | ops |")
    L.append("|--------|-----|")
    for name, ops in fam.items():
        L.append(f"| `bench.{name}` | {', '.join(f'`{o}`' for o in ops)} |")
    L.append("")

    ck = s["caps_keys"]
    L.append("## caps_keys")
    L.append("")
    L.append(ck["note"])
    L.append("")
    L.append("| key | value | note |")
    L.append("|-----|-------|------|")
    for k in ck["keys"]:
        val = k["value"] if k["live"] is False else f"{k['value']} (live)"
        L.append(f"| `{k['key']}` | {val} | {k['note']} |")
    L.append("")
    return "\n".join(L) + "\n"


# --------------------------------------------------------------------------

def main() -> int:
    header_path = Path(sys.argv[sys.argv.index("--header") + 1]) \
        if "--header" in sys.argv else HEADER
    header_text = header_path.read_text()
    sidecar = json.loads(SIDECAR.read_text())
    ref = build(header_text, sidecar)
    jtxt = json.dumps(ref, indent=2, ensure_ascii=False) + "\n"
    mdtxt = render_md(ref)

    if "--check" in sys.argv:
        ok = True
        for path, want in ((OUT_JSON, jtxt), (OUT_MD, mdtxt)):
            got = path.read_text() if path.is_file() else None
            if got != want:
                print(f"DRIFT {path.relative_to(ROOT)}: regenerate with "
                      f"tools/gen-abi-reference/gen.py (header/sidecar edit "
                      f"without re-running the generator)")
                ok = False
            else:
                print(f"OK    {path.relative_to(ROOT)} byte-stable")
        print("gen-abi-reference:", "PASS" if ok else "DRIFT")
        return 0 if ok else 1

    OUT_JSON.write_text(jtxt)
    OUT_MD.write_text(mdtxt)
    print(f"wrote {OUT_JSON.relative_to(ROOT)} + {OUT_MD.relative_to(ROOT)} "
          f"({len(ref['sections']['host_table']['symbols'])} active symbols, "
          f"{len(ref['sections']['host_table']['retired'])} retired)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
