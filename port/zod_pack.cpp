#include "zod_pack.h"

#include "zod_palette.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "zod_log.h"
#include "fineclock.h"
#include "amiga_startup.h"   /* zod_env: ZOD_PACKCHECK */

/* Warp-Messung (18.11.): Lesen gegen Aufbereiten im Archiv, Bericht am Ende
 * von Load_Graphics (zod_pack_zeit_report, unten beim Blockspeicher). */
static unsigned long zod_pk_io = 0, zod_pk_bau = 0;
static void pk_bloecke_frei(void);

#define ZPK_ENTRY_SIZE 24
/* Wo die Archive liegen. Auch levelnames.dat (die Levelbezeichnungen von
 * der CD) steht dort -- deshalb wird es gemerkt und herausgereicht. */
static char pack_dir[256] = "packs";

#define ZPK_FMT_INDEXED 1
#define ZPK_FMT_RGB565  2
#define ZPK_FMT_PCM8    3   /* rohes signed 8-Bit-Mono, Rate im Reservefeld */
#define ZPK_FMT_SHARED8 4   /* ein Byte je Pixel, Index in die GEMEINSAME Palette */
#define ZPK_FMT_PCM8_ST 5   /* wie 3, aber zwei Kanaele verschraenkt (Musik) */

/* Farbschlüssel der RGB565-Bilder: Magenta, wie im Packer gesetzt */
#define ZPK_COLORKEY565 0xF81F

namespace
{

struct Entry
{
	const char *name;      /* zeigt in die Namenstabelle des Archivs */
	unsigned short name_len;
	unsigned short w, h;
	unsigned char fmt;
	unsigned char trans;   /* Transparenzindex, 255 = keiner */
	unsigned short pal_colors;
	unsigned int data_off;
	unsigned int data_len;
	unsigned short extra;  /* Reservefeld; bei Klängen die Abtastrate */
	int pack;              /* Index des Archivs */
};

struct Pack
{
	FILE *fp;
	std::vector<char> names;
};

std::vector<Pack> packs;
std::vector<Entry> entries;   /* nach Namen sortiert, fuer die binaere Suche */

unsigned short rd_be16(const unsigned char *p)
{
	return (unsigned short)((p[0] << 8) | p[1]);
}

unsigned int rd_be32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

bool entry_less(const Entry &a, const Entry &b)
{
	int c = strncmp(a.name, b.name, a.name_len < b.name_len ? a.name_len : b.name_len);
	if(c) return c < 0;
	return a.name_len < b.name_len;
}

bool name_less(const Entry &a, const char *name)
{
	size_t n = strlen(name);
	int c = strncmp(a.name, name, a.name_len < n ? a.name_len : n);
	if(c) return c < 0;
	return a.name_len < n;
}

bool load_pack(const char *path)
{
	unsigned char head[16];
	FILE *fp = fopen(path, "rb");

	if(!fp) return false;

	if(fread(head, 1, sizeof(head), fp) != sizeof(head) || memcmp(head, "ZPK1", 4))
	{
		fclose(fp);
		return false;
	}

	unsigned int count = rd_be32(head + 4);
	unsigned int names_off = rd_be32(head + 8);
	unsigned int names_len = rd_be32(head + 12);

	std::vector<unsigned char> dir(count * ZPK_ENTRY_SIZE);
	if(count && fread(&dir[0], 1, dir.size(), fp) != dir.size())
	{
		fclose(fp);
		return false;
	}

	Pack pack;
	pack.fp = fp;
	/* Ein Nullbyte mehr: Die Namen liegen im Archiv OHNE Trenner hintereinander
	 * (nur ueber name_len abgegrenzt). Wer den Zeiger als C-Zeichenkette nimmt,
	 * liest sonst beim letzten Eintrag ueber die Belegung hinaus -- vom
	 * Pruefstand tests/shim so gemeldet. */
	pack.names.resize(names_len + 1);
	if(names_len)
	{
		fseek(fp, (long)names_off, SEEK_SET);
		if(fread(&pack.names[0], 1, names_len, fp) != names_len)
		{
			fclose(fp);
			return false;
		}
	}

	int pack_index = (int)packs.size();
	packs.push_back(pack);

	for(unsigned int i = 0; i < count; i++)
	{
		const unsigned char *e = &dir[i * ZPK_ENTRY_SIZE];
		Entry entry;

		unsigned int noff = rd_be32(e);

		entry.name       = noff < packs[pack_index].names.size() ? &packs[pack_index].names[noff] : "";
		entry.name_len   = rd_be16(e + 4);
		entry.w          = rd_be16(e + 6);
		entry.h          = rd_be16(e + 8);
		entry.fmt        = e[10];
		entry.trans      = e[11];
		entry.pal_colors = rd_be16(e + 12);
		entry.data_off   = rd_be32(e + 14);
		entry.data_len   = rd_be32(e + 18);
		entry.extra      = rd_be16(e + 22);
		entry.pack       = pack_index;

		entries.push_back(entry);
	}

	return true;
}

SDL_Surface *build_indexed(const Entry &e, const unsigned char *blob)
{
	const unsigned char *pal = blob;
	const unsigned char *pix = blob + e.pal_colors * 3;
	SDL_Surface *s = SDL_CreateRGBSurface(SDL_SWSURFACE, e.w, e.h, 8, 0, 0, 0, 0);

	if(!s) return 0;

	SDL_Color colors[256];
	for(int i = 0; i < e.pal_colors && i < 256; i++)
	{
		colors[i].r = pal[i * 3];
		colors[i].g = pal[i * 3 + 1];
		colors[i].b = pal[i * 3 + 2];
	}
	SDL_SetColors(s, colors, 0, e.pal_colors < 256 ? e.pal_colors : 256);

	if(SDL_MUSTLOCK(s)) SDL_LockSurface(s);
	for(int y = 0; y < e.h; y++)
		memcpy((unsigned char*)s->pixels + y * s->pitch, pix + (size_t)y * e.w, e.w);
	if(SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

	if(e.trans != 255) SDL_SetColorKey(s, SDL_SRCCOLORKEY, e.trans);

	return s;
}

/* Format 4: ein Byte je Pixel, Farben stehen in der gemeinsamen Palette
 * (port/zod_palette.cpp). Die Flaeche bleibt palettiert -- genau das ist der
 * Sinn des 8-Bit-Pfads: kein Aufblasen auf 16 oder 32 Bit mehr. */
SDL_Surface *build_shared8(const Entry &e, const unsigned char *blob)
{
	SDL_Surface *s = SDL_CreateRGBSurface(SDL_SWSURFACE, e.w, e.h, 8, 0, 0, 0, 0);

	if(!s) return 0;

#ifndef __amigaos__
	/* Host: SDL blittet 8 -> 32 ueber die Palette der Flaeche, jedes Bild
	 * braucht dort also seine eigene.
	 *
	 * Auf dem Amiga waere genau das Ballast: Die Zeichenflaeche ist selbst
	 * 8 Bit, ein Blit kopiert rohe Indizes, und die Farben stehen EINMAL im
	 * Schirm. Eine eigene Palette je Bild kostete dort 1 KB * 6941 Bilder =
	 * 7 MB, ohne ein einziges Pixel zu veraendern. Die Bank des Planeten
	 * wandert stattdessen beim Kartenladen in den Schirm
	 * (ZMap::RenderMap -> zod_palette_set_planet). */
	char name[64];
	size_t n = e.name_len < sizeof(name) - 1 ? e.name_len : sizeof(name) - 1;

	memcpy(name, e.name, n);
	name[n] = 0;

	const SDL_Color *colors = zod_palette_colors_for(name);

	SDL_SetColors(s, (SDL_Color*)(colors ? colors : zod_palette_colors()), 0, 256);
#endif

	if(SDL_MUSTLOCK(s)) SDL_LockSurface(s);
	for(int y = 0; y < e.h; y++)
		memcpy((unsigned char*)s->pixels + y * s->pitch, blob + (size_t)y * e.w, e.w);
	if(SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

	/* Platz 0 ist durchgehend der Schluessel */
	SDL_SetColorKey(s, SDL_SRCCOLORKEY, 0);

	return s;
}

SDL_Surface *build_rgb565(const Entry &e, const unsigned char *blob)
{
	SDL_Surface *s = SDL_CreateRGBSurface(SDL_SWSURFACE, e.w, e.h, 16,
	                                      0xF800, 0x07E0, 0x001F, 0);

	if(!s) return 0;

	if(SDL_MUSTLOCK(s)) SDL_LockSurface(s);
	for(int y = 0; y < e.h; y++)
	{
		const unsigned char *src = blob + (size_t)y * e.w * 2;
		unsigned short *dst = (unsigned short*)((unsigned char*)s->pixels + y * s->pitch);

		/* im Archiv big-endian abgelegt */
		for(int x = 0; x < e.w; x++)
			dst[x] = rd_be16(src + x * 2);
	}
	if(SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

	SDL_SetColorKey(s, SDL_SRCCOLORKEY, ZPK_COLORKEY565);

	return s;
}

} /* namespace */

int zod_pack_init(const char *dir)
{
	/* "zod_engine.zpk" traegt den Teil der Daten, der NICHT von der
	 * Original-CD stammt (HUD, Menues, Startbild, Echtfarb-Zeiger,
	 * Fabrikfenster und was der Extraktor sonst nicht liefern kann). Es
	 * wird mit der Engine ausgeliefert; die uebrigen Archive holt sich der
	 * Nutzer mit ZExtract von seiner CD. Erzeugt von
	 * tools/assets/pack_engine.py.
	 *
	 * "zod_music.zpk" ist freiwillig: rohes PCM der Musikstuecke, erzeugt von
	 * tools/assets/pack_music.py. Fehlt es, laeuft das Spiel stumm weiter --
	 * es wiegt 47 MB und soll niemanden zum Herunterladen zwingen. */
	static const char *names[] = {
		"zod_units.zpk", "zod_buildings.zpk", "zod_planets.zpk", "zod_fonts.zpk",
		"zod_cursors.zpk", "zod_other.zpk", "zod_teams.zpk", "zod_misc.zpk",
		"zod_sounds.zpk", "zod_engine.zpk", "zod_music.zpk",
		/* Die Vollbilder des Originals (Laden, Sieg, Niederlage). Sie
		 * tragen JE EIGENE Palette -- rund 250 Farben, die sich nicht
		 * in die gemeinsame bringen lassen. ZExtract holt sie aus
		 * z/main.pac der CD; fehlen sie, laeuft das Spiel mit dem
		 * bisherigen Ladebild. */
		"zod_screens.zpk", 0
	};

	if(!dir || !*dir) dir = "packs";

	/* Merken. Nicht nur die Archive liegen dort -- auch levelnames.dat
	 * (die Levelbezeichnungen von der CD). Wer ZOD_PACKS setzt, soll die
	 * ebenfalls von dort bekommen. */
	{
		size_t n = strlen(dir);

		if(n >= sizeof(pack_dir)) n = sizeof(pack_dir) - 1;

		memcpy(pack_dir, dir, n);
		pack_dir[n] = 0;
	}

	zod_pack_shutdown();

	/* Die gemeinsame Palette gehoert VOR die Archive: Format 4 braucht sie
	 * beim Anlegen jeder Flaeche. Fehlt sie, laeuft alles wie bisher. */
	zod_palette_load(dir);

	for(int i = 0; names[i]; i++)
	{
		std::string path = std::string(dir) + "/" + names[i];

		if(!load_pack(path.c_str()))
			ZLOG("Archiv nicht gelesen: %s\n", path.c_str());
	}

	std::sort(entries.begin(), entries.end(), entry_less);

	if(entries.size())
	{
		int sounds = 0;
		int shared = 0;

		for(size_t i = 0; i < entries.size(); i++)
		{
			if(entries[i].fmt == ZPK_FMT_PCM8
			|| entries[i].fmt == ZPK_FMT_PCM8_ST) sounds++;
			if(entries[i].fmt == ZPK_FMT_SHARED8) shared++;
		}

		ZLOG("Asset-Archive: %d Bilder und %d Klaenge aus %d Dateien, "
		     "davon %d mit gemeinsamer Palette\n",
		     (int)entries.size() - sounds, sounds, (int)packs.size(), shared);

		/* Palette und Archive muessen zusammenpassen -- sonst zeigen die
		 * Bildindizes in eine Palette, die gar nicht im Schirm steht.
		 *
		 * Auf dem Amiga sieht das nicht nach einem Fehler aus, sondern nach
		 * einem Grafikproblem: Karte und HUD wirken plausibel, Objekte und
		 * Text verschwinden im Untergrund. Genau so ist es am 18.09. auf der
		 * V2 aufgetreten, nachdem ein neuer Binaerstand auf ein Volume mit
		 * den ALTEN Archiven traf. Eine halbe Stunde Fehlersuche an der
		 * falschen Stelle -- deshalb steht es jetzt im Log. */
		const int bilder = (int)entries.size() - sounds;

		if(zod_palette_ready() && bilder > 0 && shared == 0)
			ZLOG("ACHTUNG: palette.zpl ist da, aber KEIN Bild nutzt die "
			     "gemeinsame Palette -- die Archive sind veraltet. "
			     "data/game/packs vollstaendig neu aufs Volume kopieren.\n");

		if(!zod_palette_ready() && shared > 0)
			ZLOG("ACHTUNG: Die Archive erwarten die gemeinsame Palette, aber "
			     "palette.zpl fehlt im selben Verzeichnis.\n");
	}

	return (int)entries.size();
}

void zod_pack_shutdown(void)
{
	for(size_t i = 0; i < packs.size(); i++)
		if(packs[i].fp) fclose(packs[i].fp);

	pk_bloecke_frei();

	packs.clear();
	entries.clear();
}

const char *zod_pack_dir(void)
{
	return pack_dir;
}

int zod_pack_available(void)
{
	return entries.size() ? 1 : 0;
}

static SDL_Surface *pack_load_opt(const char *name, int report_missing);
static SDL_Surface *pack_build(const Entry &e, const unsigned char *b);

/* Warp (18.11.2026): Blockspeicher fuer das Lesen aus den Archiven.
 *
 * Gemessen auf Amiga 2 (Warp 7060, Karte SD): beim Start gingen von 32 s
 * Ladezeit 25-27 s allein in fseek+fread -- je Bild ein Suchen und ein
 * kleines Lesen, rund 7,6 ms, bei ~3300 Bildern. Das Aufbereiten kostete
 * zusammen unter 1 s. Hier wird stattdessen in Bloecken zu 64 KB gelesen
 * und aus dem Speicher kopiert; die Bilder eines Objekts liegen im Archiv
 * hintereinander, ein Block bedient also viele Bilder.
 *
 * 16 Bloecke = 1 MB, am laengsten unbenutzter wird ersetzt. Grosse Eintraege
 * (ab 4 Bloecken: Vollbilder, Klaenge) gehen weiter direkt an die Datei.
 * ZOD_PACKCHECK 1 liest jeden Eintrag zusaetzlich auf dem alten Weg und
 * vergleicht die Bytes (Protokoll: "Archiv-Pruefung", muss 0 sein). */
#define PK_BLOCK  65536UL
#define PK_BLOCKS 16

struct PkBlock
{
	int pack;
	unsigned long nr;
	unsigned long len;
	unsigned long stempel;
	unsigned char *daten;
};

static PkBlock pk_block[PK_BLOCKS];
static unsigned long pk_uhr = 0;
static unsigned long pk_geprueft = 0, pk_falsch = 0, pk_gelesen = 0;

static int pack_read_direkt(int pk, unsigned long off, unsigned long len, unsigned char *dst)
{
	FILE *fp = packs[pk].fp;

	if(fseek(fp, (long)off, SEEK_SET)) return 0;

	return !len || fread(dst, 1, len, fp) == len;
}

static int pack_read(int pk, unsigned long off, unsigned long len, unsigned char *dst)
{
	if(len >= 4 * PK_BLOCK) return pack_read_direkt(pk, off, len, dst);

	while(len)
	{
		unsigned long nr = off / PK_BLOCK, in = off % PK_BLOCK, teil;
		PkBlock *b = 0, *alt = 0;
		int i;

		for(i = 0; i < PK_BLOCKS; i++)
		{
			PkBlock *k = &pk_block[i];

			if(k->daten && k->pack == pk && k->nr == nr && k->len) { b = k; break; }
			if(!alt || !k->daten || (alt->daten && k->stempel < alt->stempel)) alt = k;
		}

		if(!b)
		{
			b = alt;

			if(!b->daten && !(b->daten = (unsigned char *)malloc(PK_BLOCK)))
				return pack_read_direkt(pk, off, len, dst);

			b->pack = pk;
			b->nr = nr;
			b->len = 0;

			if(!fseek(packs[pk].fp, (long)(nr * PK_BLOCK), SEEK_SET))
				b->len = (unsigned long)fread(b->daten, 1, PK_BLOCK, packs[pk].fp);

			pk_gelesen++;
		}

		b->stempel = ++pk_uhr;

		if(in >= b->len) { b->len = 0; return 0; }   /* hinter dem Dateiende */

		teil = b->len - in;
		if(teil > len) teil = len;

		memcpy(dst, b->daten + in, teil);
		dst += teil;
		off += teil;
		len -= teil;
	}

	return 1;
}

static void pk_bloecke_frei(void)
{
	for(int i = 0; i < PK_BLOCKS; i++)
	{
		free(pk_block[i].daten);
		pk_block[i].daten = 0;
	}
}

void zod_pack_zeit_report(void)
{
	unsigned long ms = zod_fineclock_freq() / 1000;

	if(!ms) ms = 1;
	ZLOG("Archiv-Zeit: lesen %lu ms, aufbereiten %lu ms, %lu Bloecke zu 64 KB gelesen\n",
	     zod_pk_io / ms, zod_pk_bau / ms, pk_gelesen);

	if(pk_geprueft)
		ZLOG("Archiv-Pruefung: %lu Eintraege verglichen, %lu abweichend -- muss 0 sein\n",
		     pk_geprueft, pk_falsch);
}

/* Liest einen Eintrag; mit ZOD_PACKCHECK auch auf dem alten Weg zum Vergleich. */
static int pack_read_eintrag(const Entry &e, unsigned char *dst)
{
	static int pruefen = -1;

	if(pruefen < 0)
	{
		char wahl[8];

		pruefen = zod_env("ZOD_PACKCHECK", wahl, sizeof(wahl)) && wahl[0] != '0';
	}

	if(!pack_read(e.pack, e.data_off, e.data_len, dst)) return 0;

	if(pruefen && e.data_len)
	{
		std::vector<unsigned char> alt(e.data_len);

		pk_geprueft++;

		if(!pack_read_direkt(e.pack, e.data_off, e.data_len, &alt[0]) ||
		   memcmp(&alt[0], dst, e.data_len))
			pk_falsch++;
	}

	return 1;
}

SDL_Surface *zod_pack_load(const char *name)
{
	return pack_load_opt(name, 1);
}

SDL_Surface *zod_pack_load_quiet(const char *name)
{
	return pack_load_opt(name, 0);
}

static SDL_Surface *pack_load_opt(const char *name, int report_missing)
{
	if(!name || entries.empty()) return 0;

	std::vector<Entry>::iterator it =
		std::lower_bound(entries.begin(), entries.end(), name, name_less);

	size_t n = strlen(name);
	if(it == entries.end() || it->name_len != n || strncmp(it->name, name, n))
	{
		/* Fehlanzeige bei Bildern melden: sonst faellt der Aufrufer
		 * stillschweigend auf die Einzeldatei zurueck und eine Luecke im
		 * Archiv bliebe unbemerkt (z. B. ein zu scharfer Packfilter). */
		if(report_missing &&
		   n > 4 && (!strcmp(name + n - 4, ".png") || !strcmp(name + n - 4, ".bmp")))
			ZLOG("Archiv-Fehlanzeige (Einzeldatei): %s\n", name);

		return 0;
	}

	std::vector<unsigned char> blob(it->data_len ? it->data_len : 1);
	unsigned long zt0 = zod_fineclock_ticks();

	if(!pack_read_eintrag(*it, &blob[0]))
	{
		ZLOG("Archiv-Eintrag nicht lesbar: %s\n", name);
		return 0;
	}

	unsigned long zt1 = zod_fineclock_ticks();

	zod_pk_io += zt1 - zt0;

	SDL_Surface *erg = pack_build(*it, &blob[0]);

	zod_pk_bau += zod_fineclock_ticks() - zt1;

	return erg;
}

static SDL_Surface *pack_build(const Entry &e, const unsigned char *b)
{
	const Entry *it = &e;
	if(it->fmt == ZPK_FMT_INDEXED) return build_indexed(*it, b);
	if(it->fmt == ZPK_FMT_RGB565) return build_rgb565(*it, b);
	if(it->fmt == ZPK_FMT_SHARED8)
	{
		if(!zod_palette_ready())
		{
			ZLOG("Archiv: %s ist Format 4, aber es ist keine Palette geladen\n",
			     it->name);
			return 0;
		}

		{
			static unsigned long n = 0;

			if(++n % 250 == 0)
				ZLOG("Archiv: %lu Bilder geladen\n", n);
		}

		return build_shared8(*it, b);
	}

	return 0;
}

unsigned int zod_pack_image_names(const char ***names)
{
	static std::vector<std::string> store;
	static std::vector<const char*> ptrs;

	if(ptrs.empty())
		for(size_t i = 0; i < entries.size(); i++)
		{
			if(entries[i].fmt == ZPK_FMT_PCM8
			|| entries[i].fmt == ZPK_FMT_PCM8_ST) continue;

			store.push_back(std::string(entries[i].name, entries[i].name_len));
		}

	if(ptrs.size() != store.size())
		for(size_t i = 0; i < store.size(); i++) ptrs.push_back(store[i].c_str());

	if(names) *names = ptrs.empty() ? 0 : &ptrs[0];

	return (unsigned int)ptrs.size();
}

int zod_pack_load_sound(const char *name, unsigned char **data,
                        unsigned int *length, unsigned int *rate)
{
	if(!name || !data || entries.empty()) return 0;

	std::vector<Entry>::iterator it =
		std::lower_bound(entries.begin(), entries.end(), name, name_less);

	size_t n = strlen(name);
	if(it == entries.end() || it->name_len != n || strncmp(it->name, name, n))
		return 0;

	if(it->fmt != ZPK_FMT_PCM8) return 0;

	unsigned char *buf = (unsigned char*)malloc(it->data_len ? it->data_len : 1);
	if(!buf) return 0;

	if(!pack_read_eintrag(*it, buf))
	{
		ZLOG("Klang nicht lesbar: %s\n", name);
		free(buf);
		return 0;
	}

	*data = buf;
	if(length) *length = it->data_len;
	if(rate) *rate = it->extra ? it->extra : 11025;

	return 1;
}

