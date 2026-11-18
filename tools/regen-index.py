#!/usr/bin/env python3
"""regen-index.py — regenerate index.json (schema v2, dist wave) from
the tree's modules/ plus the existing index's per-entry metadata.

Every binary artifact (.wasm/.aot/.lua) gets an ABSOLUTE release-asset
url under the version's tag (m-<name>-<ver>); the version's doc.md and
meta.json ride the same release (doc_url/doc_sha256/doc_bytes and
meta_url pins). Per-entry metadata (kind/abi/fw_compat/src_commit) and
the .aot target keys carry over from the existing index — the tree is
the artifact source, the old index is the metadata source, and a
version missing from the old index is an error (add it there first).

Idempotent: re-running against the same tree yields identical bytes.

Usage: regen-index.py [repo-root]   (default: cwd)
"""
import hashlib
import json
import sys
from pathlib import Path

RELEASES = "https://github.com/wan-cloud/xuansi-drivers/releases/download"
ARTIFACT_EXTS = (".wasm", ".aot", ".lua")


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    idx_path = root / "index.json"
    old = {"modules": []}
    if idx_path.exists():
        old = json.loads(idx_path.read_text())
    meta_by = {(m.get("name"), m.get("version")): m for m in old.get("modules", [])}

    mods = []
    for mdir in sorted((root / "modules").iterdir()):
        if not mdir.is_dir():
            continue
        for vdir in sorted(mdir.iterdir()):
            if not vdir.is_dir():
                continue
            name, ver = mdir.name, vdir.name
            prev = meta_by.get((name, ver))
            if prev is None:
                print(f"modules/{name}/{ver}: not in the existing index — "
                      f"add its metadata (kind/fw_compat/src_commit) there "
                      f"first", file=sys.stderr)
                return 1

            entry = {"name": name, "version": ver, "abi": prev.get("abi", 1)}
            if prev.get("kind"):
                entry["kind"] = prev["kind"]

            # binary artifacts: absolute release urls + pins
            prev_target = {}
            for pf in prev.get("files", []):
                base = (pf.get("path") or pf.get("url", "")).rsplit("/", 1)[-1]
                if pf.get("target"):
                    prev_target[base] = pf["target"]
            files = []
            for f in sorted(vdir.iterdir()):
                if f.suffix not in ARTIFACT_EXTS:
                    continue
                ent = {
                    "url": f"{RELEASES}/m-{name}-{ver}/{f.name}",
                    "sha256": sha256(f),
                    "bytes": f.stat().st_size,
                }
                if f.name in prev_target:
                    ent["target"] = prev_target[f.name]
                files.append(ent)
            entry["files"] = files

            doc = vdir / "doc.md"
            if doc.is_file():
                entry["doc_url"] = f"{RELEASES}/m-{name}-{ver}/doc.md"
                entry["doc_sha256"] = sha256(doc)
                entry["doc_bytes"] = doc.stat().st_size
            meta_json = vdir / f"{name}.meta.json"
            if meta_json.is_file():
                entry["meta_url"] = f"{RELEASES}/m-{name}-{ver}/meta.json"
            if prev.get("fw_compat"):
                entry["fw_compat"] = prev["fw_compat"]
            if prev.get("src_commit"):
                entry["src_commit"] = prev["src_commit"]
            mods.append(entry)

    out = {"index_ver": 2, "modules": mods}
    idx_path.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    print(f"regen: {len(mods)} module versions -> {idx_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
