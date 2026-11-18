#!/bin/sh
# The release of the Warp build (after the Amiga build), number from port/zod_version.h:
#   releases/amiga/ZOD.info          the game's drawer icon
#   releases/amiga/Zod-<v>.zip       the public archive: uploaded to GitHub Releases
#   releases/amiga/ZOD/              the same unpacked (Work:Games/ZOD on the Amiga)
# releases/ is NOT in git (.gitignore): the archive carries the engine's own data
# (zod_engine.zpk, maps), which stays out of the repository like in Cego's port.
# Contents:
#       zod_060, ZodLaunch(.info), ZExtract, jvplay, ZodLaunch.cfg, map/story lists,
#       default_settings.txt, Data/ (maps), assets/ (tile info),
#       packs/ (palette.zpl, zod_engine.zpk: the engine's own data, + ReadMe),
#       music/ and cuts/ (empty, ReadMe), doc/ (all texts EN/PL/DE, COPYING)
# NO data of "Z" goes in: the player reads it from the CD with ZExtract (doc/Install.txt).
# The script refuses when it finds any.
#
#   sh tools/release/make_release.sh [ENGINE_DATA_DIR] [--full CD_DATA_DIR CUTS_DIR]
#
# --full also makes private/Zod-<v>-full.zip (only on this PC, private/ is not in
# git): the public archive plus the data read from the CD (CD_DATA_DIR/packs:
# zod_*.zpk, levelnames.dat; CD_DATA_DIR/music: *.zmu, MIDIBANK) and the films
# (CUTS_DIR/*.JV) - unpacks and runs at once.
#
# ENGINE_DATA_DIR (default ../stage/Zod) holds the data that is not in git:
# packs/palette.zpl, packs/zod_engine.zpk (tools/assets/pack_engine.py), Data/,
# assets/, map_list.txt, story_list.txt, default_settings.txt.
# The archive unpacks on the Amiga with:  UnZip Zod-<v>.zip -d Work:Games
set -e
cd "$(dirname "$0")/../.."
V=$(sed -n 's/^#define ZOD_VERSION  *"\([^"]*\)".*/\1/p' port/zod_version.h)
[ -n "$V" ] || { echo "no ZOD_VERSION in port/zod_version.h" >&2; exit 1; }
E=../stage/Zod
FULL=
case "$1" in --full) ;; ?*) E=$1; shift ;; esac
if [ "$1" = --full ]; then FULL=$2; CUTS=$3
	[ -d "$FULL/packs" ] && [ -d "$FULL/music" ] && [ -d "$CUTS" ] || { echo "--full CD_DATA_DIR CUTS_DIR" >&2; exit 1; }
fi
D=docs/release/amiga
T=tools/release
R=releases/amiga
G=$R/ZOD
B=build/amiga

# the programs must carry this number
grep -a -q "\$VER: Zod $V " $B/zod_060 || { echo "$B/zod_060 is not $V: build first" >&2; exit 1; }
grep -a -q "\$VER: ZodLaunch $V " $B/ZodLaunch || { echo "$B/ZodLaunch is not $V: make launcher first" >&2; exit 1; }
for f in ZExtract jvplay; do [ -f $B/$f ] || { echo "$B/$f missing" >&2; exit 1; }; done
for f in packs/palette.zpl packs/zod_engine.zpk map_list.txt story_list.txt default_settings.txt; do
	[ -f "$E/$f" ] || { echo "$E/$f missing (engine data)" >&2; exit 1; }
done
[ -d "$E/Data" ] && [ -d "$E/assets" ] || { echo "$E/Data or $E/assets missing" >&2; exit 1; }

rm -rf "$G"
mkdir -p "$G/packs" "$G/music" "$G/cuts" "$G/doc"
cp $B/zod_060 $B/ZodLaunch $B/ZExtract $B/jvplay "$G/"
cp "$D/ZodLaunch.cfg" "$G/"
cp "$E/map_list.txt" "$E/story_list.txt" "$E/default_settings.txt" "$G/"
cp -r "$E/Data" "$E/assets" "$G/"
cp "$E/packs/palette.zpl" "$E/packs/zod_engine.zpk" "$G/packs/"

# texts: UTF-8 sources -> ISO-8859-1 for the Amiga, @V@ -> number
python $T/amiga_text.py "$D/packs_ReadMe.txt" "$G/packs/ReadMe.txt" "$V"
python $T/amiga_text.py "$D/music_ReadMe.txt" "$G/music/ReadMe.txt" "$V"
python $T/amiga_text.py "$D/cuts_ReadMe.txt" "$G/cuts/ReadMe.txt" "$V"
for f in Install Zod Zod_en Zod_de Zod_dev Zod_dev_en Zod_dev_de Zod_release Zod_release_en Zod_release_de; do
	python $T/amiga_text.py "$D/$f.txt" "$G/doc/$f.txt" "$V"
done
cp LICENSE "$G/doc/COPYING"

# icons: ZodLaunch (stack 32 KB, no Tool Types: all settings are in ZodLaunch), the game's drawer (the WARP logo; never
# a game picture - it looked like the program, user 18.11.2026), doc/ (a plain drawer)
# and the texts (opened by MultiView). Pictures: tools/release/icon (draw_icons.py).
# ZodLaunch: the user's picture "Z needs you!" (tools/release/icon/zod_icon.jpeg, not
# in git), scaled to 104x51 and reduced to 128 colours (icons take at most 256)
python -c "from PIL import Image; Image.open('$T/icon/zod_icon.jpeg').convert('RGB').resize((104, 51), Image.LANCZOS).quantize(128, dither=Image.FLOYDSTEINBERG).convert('RGB').save('$B/zod_icon.png')"
python $T/make_icon.py "$G/ZodLaunch.info" --frame $B/zod_icon.png --crop 0 0 104 51 \
	--stack 32768 --pos 20 12 >/dev/null
cp $T/icon/warp-drawer.info "$R/ZOD.info"	# the game's drawer: the WARP logo, as every port (PORTING_PLAN)
python $T/make_doc_icon.py "$G/doc.info" --frame $T/icon/drawer.png --crop 0 0 46 40 --transparent \
	--drawer --window 340 300 --pos 110 20 >/dev/null
icon() {	# file picture x y
	python $T/make_doc_icon.py "$G/doc/$1.info" --frame "$T/icon/$2.png" --crop 0 0 36 44 \
		--transparent --project SYS:Utilities/MultiView --pos "$3" "$4" >/dev/null
}
icon Install.txt doc_i 20 10
icon Zod.txt doc_pl 20 75;          icon Zod_en.txt doc_en 120 75;          icon Zod_de.txt doc_de 220 75
icon Zod_release.txt doc_pl 20 140; icon Zod_release_en.txt doc_en 120 140; icon Zod_release_de.txt doc_de 220 140
icon Zod_dev.txt doc_pl 20 205;     icon Zod_dev_en.txt doc_en 120 205;     icon Zod_dev_de.txt doc_de 220 205

# never game data of "Z" in the public tree
BAD=$(find "$G" \( -iname 'zod_units.zpk' -o -iname 'zod_buildings.zpk' -o -iname 'zod_planets.zpk' \
	-o -iname 'zod_fonts.zpk' -o -iname 'zod_cursors.zpk' -o -iname 'zod_other.zpk' \
	-o -iname 'zod_sounds.zpk' -o -iname 'zod_screens.zpk' -o -iname 'zod_music.zpk' \
	-o -iname 'levelnames.dat' -o -iname '*.zmu' -o -iname '*.jv' -o -iname 'MIDIBANK' \
	-o -iname '*.pac' \) -print)
[ -z "$BAD" ] || { echo "game data in the release tree, refusing:" >&2; echo "$BAD" >&2; exit 1; }

# the archive: older ones into private/old (kept)
mkdir -p private/old
for f in "$R"/Zod-*.zip; do
	[ -e "$f" ] && [ "$f" != "$R/Zod-$V.zip" ] && mv "$f" private/old/
done
python $T/make_zip.py "$R/Zod-$V.zip" "$R" ZOD ZOD.info
if [ -n "$FULL" ]; then
	for f in private/Zod-*-full.zip; do
		[ -e "$f" ] && [ "$f" != "private/Zod-$V-full.zip" ] && mv "$f" private/old/
	done
	python $T/make_zip.py "private/Zod-$V-full.zip" "$R" ZOD ZOD.info 		--add "$FULL/packs" ZOD/packs --add "$FULL/music" ZOD/music --add "$CUTS" ZOD/cuts
fi
