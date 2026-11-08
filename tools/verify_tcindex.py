#!/usr/bin/env python3
"""verify_tcindex.py — structural validation of toolchain-index.json,
the pinned-toolchain index served from this repository and consumed by
the daemon's xuansi_toolchain_fetch tool.

Checks (any failure exits non-zero, each error reported with a
best-effort file:line location):

  * index_ver == 1
  * latest names an entry that exists in generations
  * the generation carries kind / wasi_sdk / fw_compat{wasm,aot}
    / platforms
  * platforms has exactly the five keys:
    linux-x64, linux-arm64, macos-x64, macos-arm64, win-x64
  * every platform's wasi_sdk entry: url starts with https://,
    sha256 is 64 lowercase hex chars, bytes is an integer > 0
  * wamrc is null or an entry passing the same three field checks

Usage: verify_tcindex.py [index.json]
       (default: toolchain-index.json at the repository root)

Line numbers are located by text search (best effort — the JSON
selector in the message is authoritative).
"""
import json
import re
import sys
from pathlib import Path

PLATFORMS = ["linux-x64", "linux-arm64", "macos-x64", "macos-arm64", "win-x64"]
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def is_int(v) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def find_line(lines, needles):
    """First 1-based line containing every needle, else the first one
    containing the leading needle, else None."""
    for i, ln in enumerate(lines, 1):
        if all(n in ln for n in needles):
            return i
    for i, ln in enumerate(lines, 1):
        if needles[0] in ln:
            return i
    return None


def main() -> int:
    if len(sys.argv) > 2:
        print(__doc__, file=sys.stderr)
        return 2
    default = Path(__file__).resolve().parent.parent / "toolchain-index.json"
    path = Path(sys.argv[1]) if len(sys.argv) == 2 else default

    try:
        text = path.read_text()
    except OSError as e:
        print(f"{path}: cannot read: {e}", file=sys.stderr)
        return 2
    try:
        idx = json.loads(text)
    except json.JSONDecodeError as e:
        print(f"{path}:{e.lineno}: invalid JSON: {e.msg}", file=sys.stderr)
        return 2
    lines = text.splitlines()
    errs = []

    def err(sel, msg, needles=()):
        line = find_line(lines, needles) if needles else None
        loc = f"{path}:{line}:" if line else f"{path}:"
        errs.append(f"{loc} {sel}: {msg}" if sel else f"{loc} {msg}")

    def check_entry(sel, parent_key, entry):
        """url/sha256/bytes field checks for a wasi_sdk or wamrc entry."""
        if not isinstance(entry, dict):
            err(sel, "entry must be an object", [f'"{parent_key}"'])
            return
        checks = [
            ("url", lambda v: isinstance(v, str) and v.startswith("https://"),
             "url must be an https:// string"),
            ("sha256", lambda v: isinstance(v, str) and SHA256_RE.match(v),
             "sha256 must be 64 lowercase hex chars"),
            ("bytes", lambda v: is_int(v) and v > 0,
             "bytes must be an integer > 0"),
        ]
        for field, ok, desc in checks:
            if field not in entry:
                err(f"{sel}.{field}", "missing field",
                    [f'"{parent_key}"'])
            elif not ok(entry[field]):
                err(f"{sel}.{field}", f"{desc}, got {entry[field]!r}",
                    [f'"{field}": {json.dumps(entry[field])}'])

    if not isinstance(idx, dict):
        err(None, "index must be a JSON object")
        print(f"verify-tcindex: FAIL ({len(errs)} error(s))", file=sys.stderr)
        return 1

    if idx.get("index_ver") != 1:
        err(None, f"index_ver != 1: {idx.get('index_ver')!r}",
            ['"index_ver"'])

    latest = idx.get("latest")
    gens = idx.get("generations")
    if not isinstance(gens, dict) or not gens:
        err(None, "generations must be a non-empty object", ['"generations"'])
        gens = {}
    if not isinstance(latest, str) or latest not in gens:
        err(None, f"latest {latest!r} not found in generations",
            ['"latest"'])
        gen = {}
    else:
        gen = gens[latest]

    if gen:
        gsel = f"generations.{latest}"
        if not isinstance(gen.get("kind"), str) or not gen.get("kind"):
            err(gsel, "kind must be a non-empty string", ['"kind"'])
        if not isinstance(gen.get("wasi_sdk"), str) or not gen.get("wasi_sdk"):
            err(gsel, "wasi_sdk must be a non-empty version string",
                ['"wasi_sdk"'])
        fwc = gen.get("fw_compat")
        if not isinstance(fwc, dict):
            err(gsel, "fw_compat must be an object", ['"fw_compat"'])
        else:
            for k in ("wasm", "aot"):
                if k not in fwc:
                    err(f"{gsel}.fw_compat", f"missing key {k!r}",
                        ['"fw_compat"'])

        plats = gen.get("platforms")
        if not isinstance(plats, dict):
            err(gsel, "platforms must be an object", ['"platforms"'])
            plats = {}
        for key in PLATFORMS:
            if key not in plats:
                err(f"{gsel}.platforms", f"missing platform {key!r}",
                    ['"platforms"'])
        for key in plats:
            if key not in PLATFORMS:
                err(f"{gsel}.platforms", f"unexpected platform {key!r}",
                    [f'"{key}"'])

        for key in PLATFORMS:
            if key not in plats:
                continue
            p = plats[key]
            psel = f"{gsel}.platforms.{key}"
            if not isinstance(p, dict):
                err(psel, "platform entry must be an object", [f'"{key}"'])
                continue
            if "wasi_sdk" not in p:
                err(psel, "missing key 'wasi_sdk'", [f'"{key}"'])
            else:
                check_entry(f"{psel}.wasi_sdk", key, p["wasi_sdk"])
            if "wamrc" not in p:
                err(psel, "missing key 'wamrc'", [f'"{key}"'])
            elif p["wamrc"] is not None:
                check_entry(f"{psel}.wamrc", key, p["wamrc"])

    for msg in errs:
        print(msg, file=sys.stderr)
    if errs:
        print(f"verify-tcindex: FAIL ({len(errs)} error(s))", file=sys.stderr)
        return 1
    print("verify-tcindex: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
