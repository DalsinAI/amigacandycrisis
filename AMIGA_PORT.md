Candy Crisis for AmigaOS 3.x
============================

Candy Crisis (John Stiles; source port by Iliyas Jorio) for AmigaOS 3.x on
a 68040 with FPU, on acsdl12, our SDL 1.2 for game ports (ACGame's screen,
AHI sound). Upstream: https://github.com/jorio/CandyCrisis at 1fcb507
(3.0.1, the last SDL 2 release; later commits move it to SDL 3).

Installing
----------

Unpack the archive anywhere (on AmiCradle: SYS:Utilities/OpenCompress x
candycrisis-...lha RAM:), then double-click Install-CandyCrisis beside the
CandyCrisis drawer: it asks where to put the game (Work:Games by default)
and keeps the prefs and high scores of an earlier install. The drawer's
Libs holds the acgame.library the game was built against, and the game
opens that copy first.

Running
-------

Double-click CandyCrisis, or run it from a Shell; it finds its
CandyCrisisResources drawer through PROGDIR:, so it runs from anywhere.
It opens a 640x480 screen in 256 colours (AGA or RTG).

Needs: 68040 + FPU, AmigaOS 3.x, AHI for sound, and about 12 MB of free
fast memory (the game keeps its pictures in 32-bit colour, below).

Keys: player 1 A/D move, S rotate, X drop; player 2 the cursor keys.
Esc pauses (End Game is in the pause menu, Quit on the title menu).
Keys can be changed in the Controls dialog. The mouse works the menus.

Prefs (keys, sound, music, high scores, best combo, widescreen) are saved
to PROGDIR:CandyCrisisPrefs.bin when the game quits.

What is different on the Amiga
------------------------------

* Graphics. The game draws in 32-bit colour, blending its blobs, shadows,
  glows and text in its own software blitter. That is kept as it is:
  amiga/sdlu_amiga.c gives it 32-bit surfaces of our own (acsdl12's are
  8-bit only), and only the rectangles of the frame that changed are
  turned to 256 colours and put on the screen, through one palette made
  for the whole game and a 2x2 ordered dither.
* The pictures (JPEG and PNG upstream) are 8-bit PNGs, made on a PC by
  amiga/tools/convert_gfx.py: grey masks and fonts exactly, pictures of
  256 colours or fewer exactly, the rest each with its own 256 colours.
  It also writes screen.pal, the screen's palette and colour tables.
* Fades are done with the screen's palette.
* Widescreen (the Video settings in the pause menu, on by default as
  upstream) shows the 640x360 picture between black bars on the 640x480
  screen. Fullscreen and Crisp upscaling do nothing here.
* Sound is mono: the two players' effects are not panned left and right.
  The effects' pitch changes (the chime climbing in a combo) are kept.
* Music: all of it, MODs and S3Ms, played by upstream's own player
  (IBXM) in acsdl12's music hook, at 22050 Hz.
* The mouse pointer is acsdl12's arrow (no hand pointer over buttons).

Building
--------

    make -f amiga/Makefile.amigaos STOVE=... NDK=... ACSDL=... ACSDLLIB=... \
         ACGAME=... ACGAMELIB=...

or, for the release archive with its installer, package/build-amiga.sh
(STOVE and OUT set; it fetches and builds acgame, acsdl12 and zlib at
pinned commits).

Output: build/amigaos/CandyCrisis/ (and its CandyCrisis.info beside it).
amiga/sdl2compat.h maps the SDL 2 calls the game makes onto SDL 1.2 and
our surfaces; src/SDLU.c, src/soundfx.c and src/music.c are replaced by
amiga/sdlu_amiga.c and amiga/sound_amiga.c. Other changes to upstream are
under `#ifdef __amigaos__` in src/main.c, src/gworld.c and src/prefs.c.

Byte order
----------

The game's own binary reads were checked for a big-endian machine:

* Pixels are 0x00RRGGBB words, read and written whole by blitter.c, so they
  mean the same on either byte order. Upstream's one byte-wise pixel write
  (the JPEG/PNG decode in gworld.c) is not used on the Amiga.
* IBXM reads MOD and S3M data byte by byte, building each value; WAVs are
  read by acsdl12, which swaps them.
* The prefs file is written and read in the machine's own order. A file
  from a PC has its lengths the other way round and is ignored as a whole;
  what is read is also checked (keys in range, names ended, best combo's
  blobs valid).

Licence
-------

GPL 2 (LICENSE). Artwork and music are copyright their owners, as
upstream says: original artwork by Kate Davis, Leanne Stiles, Arnauld de
la Grandiere, Bob Frasure and Ryan Bliss; music by Leanne Stiles,
Lizardking, Armadon/Explizit, Leviathan/Nemesis, Jester/Pygmy, Siren,
Sirrus, Scaven/FC, Spring, Timewalker, Jason/Silents, Chromatic Dragon and
Ng Pei Sin. IBXM (C) 2019 Martin Cameron, BSD 3-clause.

Amiga port: the AmiCradle Team.
