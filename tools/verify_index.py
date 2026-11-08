#!/usr/bin/env python3
"""verify_index.py — compare freshly built module hashes against the
committed index.json (deliverable files[]) and per-version meta.json
(auxiliary .aot).

Usage: verify_index.py <built-root> <committed-root> <index.json>

<built-root> mirrors modules/<name>/<ver>/<file> from a scratch build;
<committed-root> is the repository's modules/ directory. Every file the
index lists must exist with an exact sha256/bytes match — rebuilt from
current sources when the scratch build produces that version, else (a
historical entry, pinned beside a newer current version) verified in
place against the committed artifact. Every .aot the meta.json records
as auxiliary is compared the same way (a mismatch is reported but only
fails the run when --strict is given, because wamrc output is only
reproducible across identical wamrc builds).
"""
import hashlib
import json
import sys
from pathlib import Path


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--strict"]
    strict = "--strict" in sys.argv[1:]
    if len(args) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    built, committed, index_path = (Path(a) for a in args)
    index = json.loads(index_path.read_text())
    fails = 0

    if index.get("index_ver") != 1:
        print(f"index_ver != 1: {index.get('index_ver')!r}", file=sys.stderr)
        return 1

    seen = set()
    for mod in index.get("modules", []):
        for f in mod.get("files", []):
            rel = f["path"]
            # index paths are repo-relative like modules/<n>/<v>/<file>
            got = built / Path(rel).relative_to("modules")
            if not got.is_file():
                # Historical entry: a newer version's sources are
                # current (e.g. ledwatch@1.0.0 pinned next to 2.0.0),
                # so the scratch rebuild cannot produce it. The
                # committed artifact IS that version's deliverable —
                # verify it in place against the index pin.
                pinned = committed / Path(rel).relative_to("modules")
                if not pinned.is_file():
                    print(f"MISSING build output and committed file for {rel}",
                          file=sys.stderr)
                    fails += 1
                    continue
                h = sha256(pinned)
                ok = h == f["sha256"] and pinned.stat().st_size == f["bytes"]
                print(f"{'OK  ' if ok else 'FAIL'} index {rel}: {h} "
                      f"({pinned.stat().st_size}B, historical, committed)")
                if not ok:
                    print(f"     index says sha256={f['sha256']} "
                          f"bytes={f['bytes']}", file=sys.stderr)
                    fails += 1
                continue
            h = sha256(got)
            ok = h == f["sha256"] and got.stat().st_size == f["bytes"]
            print(f"{'OK  ' if ok else 'FAIL'} index {rel}: {h} ({got.stat().st_size}B)")
            if not ok:
                print(f"     index says sha256={f['sha256']} bytes={f['bytes']}",
                      file=sys.stderr)
                fails += 1
            seen.add(rel)

        # auxiliary artifacts recorded in the module's meta.json
        ver = mod["version"]
        meta_path = committed / mod["name"] / ver / f"{mod['name']}.meta.json"
        if not meta_path.is_file():
            continue
        meta = json.loads(meta_path.read_text())
        for aux in meta.get("auxiliary", []):
            rel = aux["path"]
            got = built / Path(rel).relative_to("modules")
            if not got.is_file():
                print(f"WARN  no build output for auxiliary {rel}", file=sys.stderr)
                continue
            h = sha256(got)
            ok = h == aux["sha256"]
            print(f"{'OK  ' if ok else 'DIFF'} aux  {rel}: {h}")
            if not ok and strict:
                fails += 1

    print("verify:", "PASS" if fails == 0 else f"{fails} mismatch(es)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
