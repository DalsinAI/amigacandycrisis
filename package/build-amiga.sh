#!/bin/sh
# The release archive, for Cradle's stove_build (target package): the game
# built with amiga/Makefile.amigaos on acsdl12 and acgame.library (fetched and built at
# pinned commits by package/acgame-deps.sh), then tools/make_package.py
# makes candycrisis-VERSION-68040.lha with its .sha256 and .readme: the
# CandyCrisis drawer as the Makefile makes it (build/amigaos/CandyCrisis), with the
# acgame.library it was built against in Libs/, its icon, and beside them
# the Amiga Installer script Install-CandyCrisis and its icon
# (package/installer/, made by amicradle scripts/port_installer.py from
# package/installer.json). OUT also gets the drawer as it unpacks, as
# CandyCrisis.tar (for a lab), and package.json.
# VERSION is package/VERSION. Nothing is published from here.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT=${OUT:-"$ROOT/build-amiga-package"}
VERSION=$(cat "$HERE/VERSION")
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/candycrisis-package.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT INT TERM
cd "$ROOT"

ACSDL12=1
. "$HERE/acgame-deps.sh"

make -f amiga/Makefile.amigaos STOVE="$STOVE" NDK="$NDK" ACGAME="$ACGAME" ACGAMELIB="$ACGAMELIB" ACSDL="$ACSDL" ACSDLLIB="$ACSDLLIB" -j4 > "$OUT/build-output.txt" 2>&1 || { tail -n 60 "$OUT/build-output.txt"; exit 1; }

# The program, checked by its SHA-256, and the rest of the drawer as a tar.
cp "build/amigaos/CandyCrisis/CandyCrisis" "$BUILD/CandyCrisis"
python3 - "build/amigaos/CandyCrisis" "CandyCrisis" "$BUILD/drawer.tar" <<'PY'
import sys, tarfile
from pathlib import Path
drawer, exe, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as t:
    for p in sorted(drawer.rglob("*")):
        rel = p.relative_to(drawer).as_posix()
        if p.is_file() and rel != exe:
            info = tarfile.TarInfo(rel)
            info.size, info.mode = p.stat().st_size, 0o644
            with p.open("rb") as f:
                t.addfile(info, f)
PY

# What the ReadMe and the check name: the program's SHA-256, the commit, the
# compiler and the commit's date (the archive's dates, so the same inputs
# make the same bytes), and the acgame.library it ships.
python3 - "$BUILD" "$STOVE/bin/m68k-amigaos-gcc" "CandyCrisis" "$ACGAME_COMMIT" <<'PY'
import hashlib, json, subprocess, sys
from pathlib import Path
build, cc, exe, acgame = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
git = lambda *a: subprocess.run(["git", *a], capture_output=True, text=True).stdout.strip()
receipt = {
    "executable_sha256": hashlib.sha256((build / exe).read_bytes()).hexdigest(),
    "source_commit": git("rev-parse", "HEAD"),
    "compiler_release": subprocess.run([cc, "--version"], capture_output=True,
                                       text=True).stdout.splitlines()[0],
    "source_date_epoch": int(git("log", "-1", "--format=%ct") or 0),
    "acgame_commit": acgame,
    "acgame_library_sha256": hashlib.sha256((build / "acgame.library").read_bytes()).hexdigest(),
}
(build / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
PY
cp "$BUILD/receipt.json" "$OUT/receipt.json"
python3 "$ROOT/tools/make_package.py" --build "$BUILD" --version "$VERSION" --out "$OUT"
