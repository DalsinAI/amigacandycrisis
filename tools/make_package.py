#!/usr/bin/env python3
"""The release archive: the game's drawer, with its GlowIcons, as an LhA archive.

    python3 tools/make_package.py --build BUILD --version VERSION --out OUT

package/package.json says what goes in the drawer, from where:

  "drawer"       the drawer's name; "drawer_icon" its icon in the repository
  "archive"      the archive's name, with {version}
  "members"      in order, each with "name" (its path in the drawer) and one of
                 "build" (a file BUILD holds), "repo" (a file in the
                 repository), "readme" (a template in the repository, its
                 @FIELDS@ filled) or "tar" (a ustar archive BUILD holds, all
                 of it unpacked under "name"; "" for the drawer itself)
  "check"        optional, on a "build" member: the receipt.json field that
                 must be its SHA-256
  "aminet"       the Aminet readme's head: [field, value] pairs
  "root"         files at the archive's root, beside the drawer, each with
                 "name" and "repo": the Amiga Installer script and its icon
                 (amicradle scripts/port_installer.py writes them from
                 package/installer.json). "installer": true on the script
                 checks its $VER names this version, so a script made for
                 another version is not shipped

BUILD holds what the port's build-amiga.sh made, with receipt.json. A
template's fields are @VERSION@ and @FIELD@ for any text field FIELD of
receipt.json (in capitals); one left unfilled stops the build, as does a
line over 78 characters.

OUT gets:
  ARCHIVE.lha          level-1 headers, -lh5-, as Amiga LhA writes them
                       (tools/lha_archive.py), the drawer and its icon, and
                       the "root" files beside them
  ARCHIVE.lha.sha256
  ARCHIVE.readme       the Aminet-style description
  DRAWER/ DRAWER.info  the drawer as it unpacks
  DRAWER.tar           the same as plain ustar, for a lab (lab_put copies
                       single files; untar unpacks it on the Amiga)
  package.json         every member's size and SHA-256

The members' dates are the source's (source_date_epoch in receipt.json), in
UTC, so the same inputs make the same bytes.
MIT, Copyright (c) 2026 AmiCradle Team.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import lha_archive  # noqa: E402

MAX_LINE = 78
EXECUTE = 0x40           # AmigaDOS "s" (script) bit, for a script started by IconX or Execute


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fill(template: Path, version: str, receipt: dict) -> bytes:
    text = template.read_text(encoding="utf-8")
    text = text.replace("@VERSION@", version)
    for k, v in receipt.items():
        if isinstance(v, (str, int)) and not isinstance(v, bool):
            text = text.replace(f"@{k.upper()}@", str(v))
    left = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if left:
        raise SystemExit(f"{template.name} has fields left unfilled: {left}")
    long = [i + 1 for i, line in enumerate(text.splitlines()) if len(line) > MAX_LINE]
    if long:
        raise SystemExit(f"{template.name}: lines over {MAX_LINE} characters: {long}")
    return text.encode("latin-1")              # MultiView and the Shell read Latin-1


def untarred(blob: bytes) -> list[tuple[str, bytes]]:
    out = []
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            name = m.name.lstrip("./")
            if name.startswith("/") or ".." in name.split("/") or ":" in name:
                raise SystemExit(f"a member outside the drawer: {m.name}")
            out.append((name, t.extractfile(m).read()))
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--spec", type=Path, default=ROOT / "package" / "package.json")
    args = ap.parse_args()

    spec = json.loads(args.spec.read_text())
    receipt = json.loads((args.build / "receipt.json").read_text())
    stamp = float(receipt.get("source_date_epoch") or os.environ.get("SOURCE_DATE_EPOCH") or 0)
    os.environ["TZ"] = "UTC"                    # LhA stamps are local time: make local UTC
    time.tzset()
    drawer = spec["drawer"]
    base = spec["archive"].format(version=args.version)

    files: list[tuple[str, bytes, int]] = []   # (path in the drawer, bytes, protection)
    readme = None
    for m in spec["members"]:
        name = m["name"]
        if "tar" in m:
            prefix = f"{name}/" if name else ""
            files += [(prefix + n, b, 0) for n, b in untarred((args.build / m["tar"]).read_bytes())]
            continue
        if "build" in m:
            blob = (args.build / m["build"]).read_bytes()
        elif "repo" in m:
            blob = (ROOT / m["repo"]).read_bytes()
        elif "readme" in m:
            blob = fill(ROOT / m["readme"], args.version, receipt)
            readme = readme or blob
        else:
            raise SystemExit(f"package.json: {name} has no source")
        if m.get("check") and sha256(blob) != receipt[m["check"]]:
            raise SystemExit(f"{name} is not the one receipt.json names ({m['check']})")
        files.append((name, blob, EXECUTE if m.get("script") else 0))
    paths = [p for p, _, _ in files]
    if len(set(p.lower() for p in paths)) != len(paths):
        raise SystemExit("two members of the drawer share a name (AmigaDOS names ignore case)")

    members = [lha_archive.Member(f"{drawer}/{p}", b, prot, "", stamp) for p, b, prot in files]
    members.append(lha_archive.Member(f"{drawer}.info", (ROOT / spec["drawer_icon"]).read_bytes(), 0, "", stamp))
    for m in spec.get("root", []):
        name, blob = m["name"], (ROOT / m["repo"]).read_bytes()
        if "/" in name or name.lower() in (drawer.lower(), f"{drawer}.info".lower()):
            raise SystemExit(f"package.json: root member {name} is not a file of its own at the root")
        if m.get("installer"):
            head = blob.split(b"\n", 1)[0].decode("latin-1")
            if not head.startswith(f"; $VER: {name} {args.version} ("):
                raise SystemExit(f"{name} was made for another version ({head!r}): run port_installer.py again")
        members.append(lha_archive.Member(name, blob, 0, "", stamp))
    listing = [{"path": m.path, "bytes": len(m.data), "sha256": sha256(m.data)} for m in members]

    args.out.mkdir(parents=True, exist_ok=True)
    lha = lha_archive.write(args.out / f"{base}.lha", members)
    back = lha_archive.read(lha.read_bytes())  # it reads back, member for member
    if [(m.path, m.data) for m in back] != [(m.path, m.data) for m in members]:
        raise SystemExit("the archive does not read back as written")
    digest = sha256(lha.read_bytes())
    (args.out / f"{base}.lha.sha256").write_text(f"{digest}  {lha.name}\n")
    width = max(len(k) for k, _ in spec["aminet"]) + 2
    head = "\n".join(f"{(k + ':').ljust(width)}{v.format(version=args.version)}" for k, v in spec["aminet"] if v)
    (args.out / f"{base}.readme").write_bytes((head + "\n\n").encode("latin-1") + (readme or b""))

    tree = args.out / drawer                   # the drawer as it unpacks, for a lab
    if tree.exists():
        shutil.rmtree(tree)
    with tarfile.open(args.out / f"{drawer}.tar", "w", format=tarfile.USTAR_FORMAT) as t:
        for m in members:
            target = args.out / m.path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(m.data)
            info = tarfile.TarInfo(m.path)
            info.size, info.mtime, info.mode = len(m.data), int(stamp), 0o644
            t.addfile(info, io.BytesIO(m.data))

    summary = {"archive": lha.name, "bytes": lha.stat().st_size, "sha256": digest, "version": args.version,
               "receipt": {k: v for k, v in receipt.items() if isinstance(v, str)}, "members": listing}
    (args.out / "package.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: v for k, v in summary.items() if k not in ("members", "receipt")}, indent=2))
    print(f"{len(members)} members")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
