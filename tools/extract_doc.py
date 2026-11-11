#!/usr/bin/env python3
"""extract_doc.py — the doc.md -> meta.json embed pipeline (B15 W2-T3).

The per-module, per-version documentation lives at
modules/<name>/<ver>/doc.md (moved out of the top-level README in the
same wave). This tool is the publish pipeline's embed step and the CI
parity lock in one:

  * doc.md's "Ops" table and "Measured" list are extracted into the
    meta.json `doc` field ({ops:[{name,args,result,semantics}],
    measured:{...}}) — the daemon's xuansi_module_doc face and the
    index consumers read it from there.
  * doc.md's preamble `release:` line lands in meta.json's `release`
    field ("production" | "test-canary" — the drivers-index kind
    vocabulary; index.json's kind mirrors it verbatim).
  * meta.json's `footprint` field is parsed MECHANICALLY from the
    committed artifacts themselves (the .wasm memory section; the .aot
    INIT_DATA/TEXT/xuansi.literal sections) — a byte-faithful port of
    the fw loader's parsers (xs_wasm.c, B15 W2-T1), so the admission
    pre-check numbers can never drift from the bytes on the wire.

Usage:
  extract_doc.py [--root REPO] [--update] [--check]

  --update   rewrite the managed fields (release/doc/footprint) into
             every modules/<n>/<v>/<n>.meta.json, canonical formatting.
  --check    regenerate in memory and compare against the committed
             files; any drift (doc.md edited without re-embedding, an
             artifact rebuilt without refreshing meta) fails loudly.
             Exit 0 = parity. This is the CI leg.
  (default)  same as --check.

The managed fields are ONLY release/doc/footprint — every other meta
key is human-owned provenance and is carried through untouched, in its
original position. Managed keys are inserted at fixed canonical
positions (release right after "kind"; doc+footprint right after
"entries") so --update is idempotent and diffs stay reviewable.
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

MANAGED = ("release", "doc", "footprint")
RELEASES = ("production", "test-canary")

# --------------------------------------------------------------------------
# doc.md parsing
# --------------------------------------------------------------------------

META_KEYS = ("kind", "release", "version", "abi")
_META_LINE = re.compile(r"^([a-z]+): (\S+)$")
_OP_ROW = re.compile(r"^\|\s*([^|]+?)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|$")
_MEASURED = re.compile(r"^-\s+`([a-z0-9_]+)`\s+(?:—|-)\s+(.+?)$")


def parse_doc_md(text: str):
    """Parse a doc.md into (meta {...}, ops [...], measured {...}).

    The anchors are deliberately boring:
      * metadata = `key: value` lines immediately under the `# ` title
        (blank lines allowed; the first non-matching line ends the block);
      * ops = the first markdown table under a `## Ops` heading, four
        columns op/args/result/semantics (the header row names them);
      * measured = `- `key` — value...` list items under `## Measured`;
        the value is the first token, the rest is the note.
    """
    lines = text.splitlines()
    meta = {}
    i = 0
    # title
    while i < len(lines) and not lines[i].startswith("# "):
        i += 1
    i += 1
    while i < len(lines):
        s = lines[i].strip()
        if not s:
            i += 1
            continue
        m = _META_LINE.match(s)
        if not m or m.group(1) not in META_KEYS:
            break
        meta[m.group(1)] = m.group(2)
        i += 1
    # sections
    ops = []
    measured = {}
    section = None
    table_header_seen = False
    for s in lines:
        t = s.strip()
        if t.startswith("## "):
            section = t[3:].strip().lower()
            table_header_seen = False
            continue
        if section == "ops":
            # GFM table semantics: an escaped \| is part of the cell, not
            # a column separator — hide it before the split, restore after.
            t2 = t.replace("\\|", "\x00")
            m = _OP_ROW.match(t2)
            if not m:
                continue
            cells = tuple(c.strip().replace("\x00", "|") for c in m.groups())
            if not table_header_seen:
                want = ("op", "args", "result", "semantics")
                if tuple(c.lower() for c in cells[:4]) != want:
                    raise SystemExit(f"doc.md: '## Ops' table header must be "
                                     f"'| op | args | result | semantics |', got {cells[:4]}")
                table_header_seen = True
                continue
            if set(t) <= {"-", " ", "|"}:   # separator row
                continue
            name, args, result, semantics = cells
            ops.append({"name": name, "args": args, "result": result,
                        "semantics": semantics})
        elif section == "measured":
            m = _MEASURED.match(t)
            if not m:
                continue
            key, rest = m.groups()
            parts = re.split(r"\s+—\s+", rest, maxsplit=1)
            value = parts[0].strip()
            if re.fullmatch(r"-?\d+", value):
                measured[key] = int(value)
            else:
                measured[key] = value
    if not ops:
        raise SystemExit("doc.md: no '## Ops' table found")
    if meta.get("release") not in RELEASES:
        raise SystemExit(f"doc.md: release must be one of {RELEASES}, "
                         f"got {meta.get('release')!r}")
    return meta, ops, measured


# --------------------------------------------------------------------------
# artifact footprint parsing (ports of the fw loader's parsers — xs_wasm.c,
# B15 W2-T1: the wasm memory section walk and the aot 4-aligned segment
# walk. Numbers must agree with bench.wasm.info() on the board.)
# --------------------------------------------------------------------------

def _leb(buf: bytes, pos: int):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf) or shift > 35:
            raise ValueError("memory LEB overrun")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7


def wasm_footprint(blob: bytes):
    """(.wasm) walk the section table to the memory section (id 5):
    [count LEB]{[flags u8][min LEB][max LEB if flags&1]} — 64KB pages.
    max is None when the module declares no maximum (the fw ABI 1 gate
    refuses that shape; the footprint records the artifact's truth)."""
    pos = 8  # magic + version
    total_init = 0
    total_max = 0
    saw_mem = False
    max_absent = False
    while pos < len(blob):
        sid = blob[pos]
        pos += 1
        size, pos = _leb(blob, pos)
        if pos + size > len(blob):
            raise ValueError("wasm section overruns file")
        if sid == 5:
            saw_mem = True
            q = pos
            nmem, q = _leb(blob, q)
            for _ in range(nmem):
                flags = blob[q]
                q += 1
                init_pages, q = _leb(blob, q)
                total_init += init_pages * 65536
                if flags & 1:
                    max_pages, q = _leb(blob, q)
                    total_max += max_pages * 65536
                else:
                    max_absent = True
        pos += size
    if not saw_mem:
        return {"initial_bytes": 0, "max_bytes": None}  # no memory module
    return {"initial_bytes": total_init,
            "max_bytes": None if max_absent else total_max}


def aot_footprint(blob: bytes):
    """(.aot) the fw's 4-aligned segment walk: INIT_DATA (type 1) memory
    face {[u32 imp][u32 cnt]{flags,bpp,init_pages,max_pages}xN}, TEXT
    (type 2) size, CUSTOM (type 100) xuansi.literal pool size."""
    mem = text = pool = 0
    mem_seen = False
    p = 8
    while p < len(blob):
        p = (p + 3) & ~3
        if p + 8 > len(blob):
            break
        stype, size = struct.unpack_from("<II", blob, p)
        if size > len(blob) - p - 8:
            raise ValueError("aot section overruns file")
        body = blob[p + 8:p + 8 + size]
        if stype == 1:  # INIT_DATA
            if size < 16:
                raise ValueError("INIT_DATA too small")
            imp, cnt = struct.unpack_from("<II", body, 0)
            if imp > 16 or cnt > 16 or 8 + cnt * 16 > size:
                raise ValueError("aot memory count malformed")
            for i in range(cnt):
                _flags, bpp, ipg, _mpg = struct.unpack_from("<IIII", body, 8 + i * 16)
                mem += bpp * ipg
            mem_seen = True
        elif stype == 2:  # TEXT
            text = size
        elif stype == 100:  # CUSTOM sections — look for xuansi.literal
            if size < 4:
                p += 8 + size
                continue
            sub, = struct.unpack_from("<I", body, 0)
            if sub == 0:  # AOT_CUSTOM_SECTION_RAW
                if 4 + 2 > size:
                    p += 8 + size
                    continue
                nl = body[4] | (body[5] << 8)
                q = 6
                if q + nl > size:
                    p += 8 + size
                    continue
                name = body[q:q + nl]
                if name == b"xuansi.literal\x00":
                    content = (q + nl + 3) & ~3
                    if content > size:
                        content = size
                    pool = size - content
        p += 8 + size
    if not mem_seen:
        raise ValueError("aot has no INIT_DATA section")
    return {"initial_bytes": mem, "text_bytes": text, "pool_bytes": pool}


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def module_footprint(vdir: Path, name: str):
    """Build meta's `footprint` from the committed artifacts."""
    fp = {}
    wasm = vdir / f"{name}.wasm"
    if wasm.is_file():
        fp["wasm"] = wasm_footprint(wasm.read_bytes())
    aot_ns = {}
    for aot in sorted(vdir.glob(f"{name}.aot")):
        aot_ns["esp32s3"] = aot_footprint(aot.read_bytes())
    if aot_ns:
        fp["aot"] = aot_ns
    return fp


# --------------------------------------------------------------------------
# meta rewrite
# --------------------------------------------------------------------------

def managed_fields(vdir: Path, name: str):
    doc_path = vdir / "doc.md"
    meta, ops, measured = parse_doc_md(doc_path.read_text())
    fields = {
        "release": meta["release"],
        "doc": {"ops": ops, "measured": measured},
        "footprint": module_footprint(vdir, name),
    }
    # doc.md's preamble must agree with the meta identity fields it sits
    # beside — a version-dir mixup fails here, not downstream.
    ver = vdir.name
    if meta.get("version") != ver:
        raise SystemExit(f"{doc_path}: preamble version {meta.get('version')!r} "
                         f"!= directory version {ver!r}")
    return meta, fields


def insert_managed(meta: dict, fields: dict) -> dict:
    """Rebuild the meta dict with managed keys at canonical positions,
    every other key carried through untouched in its original order."""
    out = {}
    done = set()
    for k, v in meta.items():
        if k == "kind":
            out[k] = v
            if "release" not in done:
                out["release"] = fields["release"]
                done.add("release")
            continue
        if k == "entries":
            out[k] = v
            if "doc" not in done:
                out["doc"] = fields["doc"]
                out["footprint"] = fields["footprint"]
                done.update(("doc", "footprint"))
            continue
        if k in MANAGED:
            continue  # stale copy replaced by the canonical insert above
        out[k] = v
    for k in MANAGED:  # meta without kind/entries anchors: append
        if k not in done:
            out[k] = fields[k]
    return out


# consistency guard: doc.measured keys that duplicate the mechanical
# footprint/artifact numbers must agree with them (soft-locked: a stale
# measured number after an artifact rebuild fails the parity leg).
def cross_check(name: str, meta: dict, fields: dict, vdir: Path):
    m = fields["doc"]["measured"]
    fp = fields["footprint"]
    def want(key, got):
        if key in m and isinstance(m[key], int) and m[key] != got:
            raise SystemExit(f"{name}: doc.md measured.{key}={m[key]} "
                             f"!= artifact truth {got}")
    if "wasm" in fp:
        want("initial_memory_bytes", fp["wasm"]["initial_bytes"])
        if fp["wasm"]["max_bytes"] is not None:
            want("max_memory_bytes", fp["wasm"]["max_bytes"])
    if "aot" in fp:
        a = fp["aot"]["esp32s3"]
        want("aot_initial_memory_bytes", a["initial_bytes"])
        want("aot_text_bytes", a["text_bytes"])
        want("aot_pool_bytes", a["pool_bytes"])
    d = meta.get("deliverable")
    if isinstance(d, dict) and "bytes" in d:
        want("wasm_bytes", d["bytes"])
    aot = meta.get("aot") or {}
    ent = aot.get("esp32s3")
    if isinstance(ent, dict) and "bytes" in ent:
        want("aot_bytes", ent["bytes"])


def iter_versions(root: Path):
    for ndir in sorted((root / "modules").iterdir()):
        if not ndir.is_dir():
            continue
        for vdir in sorted(ndir.iterdir()):
            meta = vdir / f"{ndir.name}.meta.json"
            if vdir.is_dir() and meta.is_file() and (vdir / "doc.md").is_file():
                yield ndir.name, vdir, meta


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    mode = "--update" if "--update" in sys.argv else "--check"
    fails = 0
    for name, vdir, meta_path in iter_versions(root):
        meta = json.loads(meta_path.read_text())
        doc_meta, fields = managed_fields(vdir, name)
        cross_check(name, meta, fields, vdir)
        if doc_meta.get("kind") != meta.get("kind"):
            print(f"FAIL {name}@{vdir.name}: doc.md kind {doc_meta.get('kind')!r} "
                  f"!= meta kind {meta.get('kind')!r}")
            fails += 1
        rebuilt = insert_managed(meta, fields)
        canonical = json.dumps(rebuilt, indent=2, ensure_ascii=False) + "\n"
        current = meta_path.read_text()
        if mode == "--update":
            if canonical != current:
                meta_path.write_text(canonical)
                print(f"updated {meta_path.relative_to(root)}")
            continue
        # parity: content equality on the managed fields + canonical bytes
        if canonical != current:
            print(f"FAIL {meta_path.relative_to(root)}: not at parity — rerun "
                  f"tools/extract_doc.py --update (doc.md/meta/artifact drift)")
            fails += 1
        else:
            print(f"OK   {name}@{vdir.name}: doc/release/footprint at parity "
                  f"({len(fields['doc']['ops'])} ops)")
    if mode == "--check":
        print("extract_doc:", "PASS" if not fails else f"{fails} drift")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
