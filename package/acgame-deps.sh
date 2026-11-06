# Sourced by package/build-amiga.sh (sh, set -eu; STOVE, BUILD and OUT set).
# Fetches and builds what an ACGame game links, at pinned commits, into
# $BUILD/deps, and sets:
#   ACGAME, ACGAMELIB  DalsinAI/acgame at ACGAME_REF: its headers, and its
#                      build (libacgame.a, the stubs that open acgame.library,
#                      and acgame.library itself, which the game ships in
#                      Libs/); ACGAME_COMMIT is the commit
#   NDK                gfopensdk's OpenNDK headers (unless NDK is set)
#   ACSDL, ACSDLLIB    with ACSDL12=1: acsdl12 from DalsinAI/amicradle-guest
#                      at GUEST_REF, built without its own copy of ACGame
#                      (the game gets ACGame from acgame.library), on zlib
#                      1.3.1's inflate
# It copies acgame.library to $BUILD/acgame.library.
# MIT, Copyright (c) 2026 AmiCradle Team.

ACGAME_REF=${ACGAME_REF:-a3ea99f2a6d7376c8060e695e5d9c2878dbedc76}
GUEST_REF=${GUEST_REF:-d506dccd15d224792b8b4b61da1aac84140bf2a1}
DEPS="$BUILD/deps"
mkdir -p "$DEPS"

# A stove's compiler is in its prefix/.
if [ ! -x "$STOVE/bin/m68k-amigaos-gcc" ] && [ -x "$STOVE/prefix/bin/m68k-amigaos-gcc" ]; then
	STOVE="$STOVE/prefix"
fi

acg_fetch() {	# repo ref dir
	git init -q "$3"
	git -C "$3" fetch -q --depth 1 "https://github.com/$1" "$2"
	git -C "$3" checkout -q FETCH_HEAD
}

if [ -z "${NDK:-}" ]; then
	git clone -q --depth 1 https://github.com/DalsinAI/gfopensdk "$DEPS/gfopensdk"
	NDK="$DEPS/gfopensdk/OpenNDK/Include_H"
fi
if [ -z "${ACGAME:-}" ]; then
	acg_fetch DalsinAI/acgame "$ACGAME_REF" "$DEPS/acgame"
	ACGAME="$DEPS/acgame"
fi
ACGAMELIB="$DEPS/acgame-out"
CC="$STOVE/bin/m68k-amigaos-gcc" AR="$STOVE/bin/m68k-amigaos-ar" NM="$STOVE/bin/m68k-amigaos-nm" \
	OUT="$ACGAMELIB" sh "$ACGAME/build-amiga.sh" > "$OUT/acgame-output.txt" 2>&1 ||
	{ tail -n 60 "$OUT/acgame-output.txt"; exit 1; }
ACGAME_COMMIT=$(git -C "$ACGAME" rev-parse HEAD)
cp "$ACGAMELIB/acgame.library" "$BUILD/acgame.library"

if [ "${ACSDL12:-0}" = 1 ]; then
	if [ -z "${ACSDL:-}" ]; then
		git init -q "$DEPS/guest"
		git -C "$DEPS/guest" sparse-checkout set gameports/acsdl12
		git -C "$DEPS/guest" fetch -q --depth 1 --filter=blob:none https://github.com/DalsinAI/amicradle-guest "$GUEST_REF"
		git -C "$DEPS/guest" checkout -q FETCH_HEAD
		ACSDL="$DEPS/guest/gameports/acsdl12"
	fi
	if [ -z "${ZLIB:-}" ]; then
		acg_fetch madler/zlib 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf "$DEPS/zlib"	# v1.3.1
		ZLIB="$DEPS/zlib"
	fi
	ACSDLLIB="$DEPS/acsdl12-out"
	# ACG= leaves ACGame's own sources out of libacsdl12.a.
	make -C "$ACSDL" -f Makefile.amigaos STOVE="$STOVE" NDK="$NDK" ACGAME="$ACGAME" ZLIB="$ZLIB" \
		ACG= BUILD="$ACSDLLIB" -j4 > "$OUT/acsdl12-output.txt" 2>&1 ||
		{ tail -n 60 "$OUT/acsdl12-output.txt"; exit 1; }
fi
