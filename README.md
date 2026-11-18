# Zod Engine for the Amiga with a Warp card

**Z** (Bitmap Brothers, 1996) on the Amiga: the **Zod Engine**, Nighsoft's open
source re-implementation of *Z*, in Cego's AmigaOS 3.2 port
([AMIGAbench/ZodEngine](https://github.com/AMIGAbench/ZodEngine)), tuned for
Warp cards.

**This build is made for the Amiga 500 / 1200 / 3000 / 4000 with a Warp
accelerator card: Warp 560, Warp 1240, Warp 1260, Warp 1260 Extreme or
Warp 7060.** It was ported and tested on an A1200 with a Warp 1260 Extreme
(68060, 100 MHz) and an A4000 with a Warp 7060 (68060). The picture is put
together in Fast RAM and only what changed goes to the card's graphics memory;
scrolling is done by the card's blitter. That fast graphics card is what makes
the frame rates below possible. Other Amigas with a 68040 or 68060 and a
Picasso96 card run the game too, but slower.

- **Original Z Story Mode**: the 35 original maps in order, all 63 films from
  the CD, the original loading and statistics screens, the music
- **ZodEngine Mode**: your own map list, teams and bots
- resolutions from 640x480 to 1280x720; on a Warp 1260 at 640x480 usually
  35-70 frames per second, large battles with about 200 robots about 15
- builds for the 68060 (`zod_060`) and the 68040 (`zod_040`, Warp 1240);
  ZodLaunch picks the one for the processor
- sound through AHI or Paula; the films have their own player (`jvplay`)
- from the start of the game to the first map about 9 seconds (Warp 7060),
  with the progress in percent on the loading screen
- the release number in the game menu ("Warp x.xx"), in ZodLaunch and in
  `Version zod_060`

## Play it

Download `Zod-<version>.zip` from the
[Releases](../../releases) page of this repository and unpack it on the Amiga:

    UnZip Zod-<version>.zip -d Work:Games

This makes the drawer `Work:Games/ZOD`. Then follow `doc/Install.txt` in it
(English, Polish, German) – in short:

1. **Game data from your CD** (needed): insert the CD of *Z* and type
   `CD Work:Games/ZOD` and `ZExtract CD0:`. ZExtract reads the graphics,
   sounds, loading screens and music notes from the CD (a few minutes).
2. **Films** (optional, about 330 MB): `Copy CD0:CUTS/#? Work:Games/ZOD/cuts/`
3. **Music instruments** (optional): the CD has only the notes; the game
   plays them with a bank of General MIDI samples. Get `ADoom_Ins.lha` from
   Aminet (game/shoot; made for ADoom, nothing of Doom is in it), unpack it
   and copy `MIDI_Instruments` to `Work:Games/ZOD/music/MIDIBANK`. Then tick
   "Music" in ZodLaunch.
4. Double-click **ZodLaunch** and press **Start**.

The texts for players are in the release and in this repository:
[English](docs/release/amiga/Zod_en.txt),
[Polski](docs/release/amiga/Zod.txt),
[Deutsch](docs/release/amiga/Zod_de.txt),
release history [EN](docs/release/amiga/Zod_release_en.txt) /
[PL](docs/release/amiga/Zod_release.txt) /
[DE](docs/release/amiga/Zod_release_de.txt).

**Requirements:** an Amiga with a Warp card (68060, or 68040 on the
Warp 1240), 32 MB Fast RAM or more, AmigaOS 3.2, Picasso96 (the Warp's RTG),
MUI 3.8 or newer for ZodLaunch, AHI or Paula for sound; about 25 MB on disk,
about 355 MB with the films – and your own original CD of *Z*.

**Settings:** everything is set in ZodLaunch (mode, resolution, frame rate,
films, sound and music output, program); extra options for the game go into
its "Arguments" field, e.g. `-L RAM:zod-log.txt` for a log with a bug report.

### The game data is not included

The graphics, sounds, films and music of *Z* belong to the Bitmap Brothers.
They are not in this repository and not in the release: they are read from
**your own CD** with ZExtract, on the Amiga. The instrument bank comes from
Aminet (see above); it is not included either.

The release ZIP carries the engine's own data (`packs/palette.zpl`,
`packs/zod_engine.zpk`, the maps in `Data/`, `assets/`), built from the
[Qt_ZodEngine](https://github.com/BallWin/Qt_ZodEngine) upstream like in
Cego's port. That data is kept out of this repository; the ZIP is published
under Releases.

### Bug reports

Please open an issue with the release number (game menu "Warp x.xx",
ZodLaunch title or `Version Work:Games/ZOD/zod_060`), your Amiga, card and
resolution, what you did and what happened, and the log (`-L RAM:zod-log.txt`
under "Arguments" in ZodLaunch).

## Building

Cross compiler: bebbo's m68k-amigaos-gcc (libnix, `-noixemul`).

    make PLATFORM=amiga CPU=68060            # build/amiga/zod_060
    make PLATFORM=amiga CPU=68040            # build/amiga/zod_040
    make PLATFORM=amiga CPU=68060 launcher   # ZodLaunch (MUI headers)
    make PLATFORM=amiga CPU=68060 jvplay     # the film player
    make PLATFORM=amiga CPU=68060 extract    # ZExtract
    make hosttest                            # host tests (Linux)

The release number is set in `port/zod_version.h`.
`tools/release/make_release.sh [ENGINE_DATA_DIR]` builds `releases/amiga/ZOD`
and `releases/amiga/Zod-<version>.zip` (not in git); it refuses a binary
without the current `$VER` and any data from the CD in the public tree.
`ENGINE_DATA_DIR` holds the engine data that is not in git (`packs/palette.zpl`,
`packs/zod_engine.zpk` from `tools/assets/pack_engine.py`, `Data/`, `assets/`,
the map lists, `default_settings.txt`).

Details for developers – test switches (`ZOD_*` compare modes that check every
fast path against the old one), the log, measuring:
[Zod_dev_en.txt](docs/release/amiga/Zod_dev_en.txt).

## Licence

GPLv3, see [LICENSE](LICENSE). The Zod Engine by Nighsoft is GPLv3, and Cego's
port and this build inherit it.

Third party code included here under its own terms:

* `upstream/ZodEgine_Libs/QZod_DnSeparate/SDL_rotozoom.*` — zlib licence
  (Andreas Schiffler)
* `port/third_party/stb_image.h` — public domain / MIT (Sean Barrett)

The game data of *Z* is **not** covered by that licence and is not
distributed.
