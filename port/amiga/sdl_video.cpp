/*
 * Eigene Flaechen und eigener Blitter (Ersatz fuer SDL 1.2 auf AmigaOS).
 *
 * Grundgedanke: Alles liegt in 8 Bit chunky auf EINER gemeinsamen Palette
 * (port/zod_palette.cpp, aus der Originalpalette des Spiels). Damit ist
 *   - eine Flaeche genau w*h Byte gross (statt w*h*4),
 *   - der Farbschluessel ein INDEX (Platz 0) statt eines Farbwerts, also
 *     kollisionsfrei,
 *   - der Blit eine Zeilenschleife mit Vergleich gegen 0,
 *   - die Teamfarbe eine Umsetztabelle von 256 Byte statt acht Bildkopien.
 *
 * 16-Bit-Flaechen gibt es weiterhin fuer die wenigen Echtfarbbilder
 * (Startbild, Portraits). Alles andere ist 8 Bit.
 *
 * Bewusst NICHT nachgebildet: Alphamischung je Pixel (der Bestand hat 8
 * Dateien mit Teiltransparenz, alle in Menues), RLE, Hardwareflaechen.
 */
#include <SDL/SDL.h>

#include <stdlib.h>
#include <string.h>

#include "zod_log.h"
#include "zod_palette.h"
#include "amiga_startup.h"
#include "ammx.h"
#include "fastcopy.h"
#include "blitkey.h"   /* Schluesselblit je Langwort (Warp F3) */
/* ACHTUNG: HIER STAND `#include <proto/dos.h>`, und zwar UNBEDINGT.
 * Auf dem Host gibt es den Header nicht -- damit liess sich `make shimtest`
 * seit dem 19.09. nicht mehr uebersetzen, ohne dass es jemandem auffiel.
 * Dieselbe Klasse wie beim frueheren `pack_test`: **Ein Pruefmittel, das nicht
 * baut, ist ein stiller Testausfall.**
 *
 * Die Schalter werden jetzt ueber `zod_env()` gelesen (amiga_startup.h, oben
 * bereits eingebunden). Das kapselt GetVar auf dem Amiga und getenv sonst --
 * und der Ersatz existierte bereits, er war hier nur nicht benutzt. */

/* ---- Was der Blitter wirklich bewegt (bisher unbelegt) ---------------
 *
 * "zeichnen" ist auf der V1200 79 % der Bildzeit, aber die Aufteilung auf
 * Skalierer und Blitter war nie gemessen. Der 8->8-Blit MIT Farbschluessel
 * ist der Kandidat fuer AMMX: linearer Bytestrom, laeuft je Sprite in jedem
 * Bild. Ohne Zahl waere jede Beschleunigung dort geraten.
 *
 * Nur Zaehler, kein Zeitnehmen -- current_time() je Blit waere bei
 * Hunderttausenden Aufrufen selbst der Messfehler. */
#ifdef ZOD_AMMX
/* port/amiga/ammx_blit.s -- acht Bildpunkte je Durchgang, ohne das Ziel zu
 * lesen (storeilm schreibt bedingt). Nur benutzt, wenn zod_ammx_available()
 * es freigibt: 68080 erkannt UND Selbsttest bestanden. */
extern "C" void zod_ammx_blit_row(unsigned char *dst, const unsigned char *src,
                                  unsigned long count, const unsigned char *key8);
/* Rechteckfassungen: die Zeilenschleife liegt IN der Routine. Frueher stand
 * sie hier, es gab also einen Aufruf JE ZEILE -- bei einem Truemmersprite von
 * 40 Zeilen 40 Spruenge, und beim Sterben des Hauptgebaeudes leben ueber 800
 * Effekte gleichzeitig. Ein Aufruf ist auf der V1200 rund 350 ns wert. */
extern "C" void zod_ammx_blit_rect(unsigned char *dst, const unsigned char *src,
                                   unsigned long w, unsigned long h,
                                   long dpitch, long spitch,
                                   const unsigned char *key8);
extern "C" void zod_ammx_blit_rect_k0(unsigned char *dst, const unsigned char *src,
                                      unsigned long w, unsigned long h,
                                      long dpitch, long spitch);

extern "C" void zod_ammx_blit_row_k0(unsigned char *dst, const unsigned char *src,
                                     unsigned long count);
#endif

/* ---- Schmutzspuren: was sich je Bild wirklich aendert --------------------
 *
 * Bisher kopierte SDL_Flip in jedem Bild die GANZE Flaeche in den Schirm --
 * 307 200 Byte, auf der V1200 gemessen 2451 us mit AMMX. Ein grosser Teil
 * davon aendert sich gar nicht: Die Anzeige (100 Punkte rechts, 36 unten,
 * zusammen 67 440 Byte = 22 %) wird nur bei gesetzter Schmutzmarke neu
 * gezeichnet, steht sonst unveraendert da.
 *
 * Hier wird deshalb je ZEILE mitgeschrieben, welcher x-Bereich seit dem
 * letzten Umschalten beschrieben wurde. Der Aufwand ist zwei Vergleiche je
 * Zeile eines Blits -- die Schleife laeuft ohnehin ueber diese Zeilen.
 *
 * Die Zeilenkoerning ist bewusst grob: Sie verlangt keine Rechteckverwaltung
 * und keine Vereinigung ueberlappender Bereiche, und sie trifft genau den
 * Fall, um den es geht (unberuehrte Randstreifen). */
/* Blockraster 16x16, passend zur Kachelgroesse der Karte.
 *
 * Die erste Fassung fuehrte EINEN x-Bereich je Zeile. Zwei weit
 * auseinanderliegende Einheiten in derselben Bildzeile ergaben damit einen
 * breiten Bereich samt allem dazwischen -- gemessen sparte das nur 20 % des
 * Kartenblits. Mit Bloecken zerfaellt dieselbe Zeile in getrennte Laeufe.
 *
 * 640x480 ergibt 40x30 = 1200 Bloecke, also 1200 Byte je Spur. Das Vermerken
 * kostet je Blit ein paar Schiebebefehle statt einer Schleife ueber alle
 * Zeilen.
 *
 * Zwei Spuren, weil zwei verschiedene Fragen zu beantworten sind:
 *   out_*  Was muss in den SCHIRM?  Alles -- auch die Wiederherstellung,
 *          denn ihre Bildpunkte haben sich geaendert.
 *   obj_*  Was ist NAECHSTES Bild zuzudecken?  Nur die Objekte.
 * Mit einer einzigen Spur haelt sich die Sache selbst am Leben; genau daran
 * ist die erste Fassung gescheitert.
 */
enum { ZOD_BLOCK_SHIFT = 4 };                 /* 16 Punkte  */
enum { ZOD_GRID_W = 128, ZOD_GRID_H = 96 };   /* bis 2048x1536 */

static SDL_Surface *dirty_target = 0;
/* DREI RASTER, ZWEI DAVON WERDEN JE BILD GETAUSCHT STATT KOPIERT.
 *
 * `zod_dirty_clear` kopierte bis zum 02.10. die Objektspur byteweise in die
 * Vorspur und nullte danach beide anderen -- 5 Befehle und 3 Speicherzugriffe
 * je Rasterblock. Das sind bei 640x480 (40x30 = 1200 Bloecke) rund 6000
 * Befehle je Bild, bei 1280x720 (80x45 = 3600) rund 18 000. Der Posten steckt
 * in keiner `zeichnen.*`-Klammer; er ist ein guter Teil der in CLAUDE.md
 * notierten "rund 430 us in umschalten, die keiner der drei Zaehler erfasst".
 *
 * Die Vorspur IST die Objektspur des letzten Bildes -- es gibt keinen Grund,
 * sie umzukopieren. Zwei Zeiger tauschen genuegt, und genullt wird mit
 * `memset` je Zeile statt byteweise.
 *
 * Die Zeiger behalten die Schreibweise `g[by][bx]`, damit sich an den
 * Benutzern nichts aendert. `out_grid` wird NICHT mitgetauscht: es hat keine
 * Vorgaengerfassung. */
static unsigned char grid_a[ZOD_GRID_H][ZOD_GRID_W];
static unsigned char grid_b[ZOD_GRID_H][ZOD_GRID_W];
static unsigned char grid_out[ZOD_GRID_H][ZOD_GRID_W];

static unsigned char (*out_grid)[ZOD_GRID_W]  = grid_out;
static unsigned char (*obj_grid)[ZOD_GRID_W]  = grid_a;
static unsigned char (*prev_grid)[ZOD_GRID_W] = grid_b;
static int grid_w = 0, grid_h = 0;
static int prev_gueltig = 0;
static int im_restore = 0;
static int dirty_an = 1;
static int sweep_an = 0;

/* 64 Bit, nicht 32: Bei 1280x720 laeuft ein 32-Bit-Zaehler nach rund 9600
 * Bildern ueber, und jede laengere Messung in dieser Aufloesung war damit
 * Unsinn. Belegt an drei Laeufen derselben Karte -- zwei uebergelaufene
 * ergeben um 2^32 berichtigt denselben Wert wie der dritte. Addieren kostet
 * auf m68k zwei Befehle; geteilt wird nur einmal beim Beenden. */
unsigned long long zod_dirty_bytes = 0;
unsigned long zod_dirty_frames = 0;
unsigned long zod_dirty_calls  = 0;

static void grid_alles(unsigned char (*g)[ZOD_GRID_W], unsigned char v)
{
	/* Je Blockzeile ein memset statt einer Byteschleife. libnix' memset ist
	 * echter Code (movem.l ueber 8 Register, 32 Byte je Durchgang) und kein
	 * CopyMem-Stummel -- nachgesehen, nicht angenommen; die memcpy-Falle
	 * gilt hier also nicht. */
	for(int by = 0; by < grid_h; by++)
		memset(g[by], v, (size_t)grid_w);
}

/* Wie viele SAUBERE Bloecke zwischen zwei schmutzigen uebersprungen werden,
 * statt den Lauf zu teilen. Vorgabe 4 Bloecke = 64 Punkte.
 *
 * WARUM UEBERHAUPT: Auf der V1200 gemessen -- das Blockraster senkte die
 * Ausgabe auf 30 % der Byte, aber `umschalten` nur um 7 %. Der Durchsatz fiel
 * von 104 auf 34 MB/s. Der Grund ist nicht das Kopieren, sondern der AUFRUF:
 * jeder Lauf ist ein Sprung durch den Funktionszeiger plus AMMX-Vorlauf, und
 * die Ausgabe setzt die Laeufe einer Blockzeile SECHZEHNMAL ab (einmal je
 * Bildzeile). Aus 480 Aufrufen je Bild wurden Tausende.
 *
 * Ueber eine Luecke hinwegzukopieren ist billiger als ein zweiter Aufruf,
 * sobald die Luecke kleiner ist als der Aufrufaufwand: 64 Byte kosten bei
 * 125 MB/s rund 0,5 us, ein Aufruf liegt in derselben Groessenordnung.
 * `SetEnv ZOD_LUECKE <bloecke>` stellt es um (0 = nie zusammenfassen).
 *
 * Vorgabe 4, und zwar aus einer MESSUNG auf der V1200 (ZOD_COPYBENCH, 19.09.):
 * ein Kopieraufruf kostet dort 350 ns, ein Byte 6,02 ns (166 MB/s). Ein Aufruf
 * ist damit 58 Byte wert, also 3,6 Bloecke.
 *
 * Hier stand vorher 12. Das kam aus einem Modell, das ich aus zwei Spiellaeufen
 * erschlossen hatte -- mit 200 Byte je Aufruf, dem 3,6-fachen des wahren Werts,
 * weil eine der beiden Aufrufzahlen aus dem EMULATOR uebernommen war. Dieselbe
 * Kurve mit den gemessenen Preisen bewertet: Tiefpunkt bei Luecke 2 bis 4
 * (1092 us), Luecke 12 bei 1112, Luecke 16 bei 1484 -- also 36 % schlechter,
 * waehrend das falsche Modell 16 als Bestwert nannte.
 *
 * Derselbe Wert steuert in SDL_Flip, wie viel Rest ein fast breiter Lauf
 * bekommt, um auf die ganze Breite aufgefuellt zu werden. Das ist dieselbe
 * Waehrung: Aufruf gegen Byte. */
static int luecke = 8;

extern "C" void zod_dirty_set_luecke(int bloecke)
{
	if(bloecke < 0) bloecke = 0;
	if(bloecke > ZOD_GRID_W) bloecke = ZOD_GRID_W;

	luecke = bloecke;
}

/* ---- ZOD_ROTOMEM: woher die Drehflaechen ihren Speicher nehmen ----------
 *
 * Eine Drehflaeche eines 16x16-Sprites ist bei Groesse 5 nur 80x80 = 6400
 * Byte und liegt damit UNTER ZOD_BIG_LIMIT -- sie kommt also aus libnix'
 * calloc. Gemessen auf der V1200 (20.09.) kostet das zugehoerige `free`
 * **131 us je Stueck**, und beim Sterben des Hauptgebaeudes sterben in EINEM
 * Bild bis zu 386 Flaechen. libnix' free laeuft die Blockliste linear ab.
 *
 * `SetEnv ZOD_ROTOMEM exec` schickt diese Flaechen stattdessen ueber
 * AllocVec/FreeVec, also an libnix vorbei direkt an exec.
 *
 * INZWISCHEN DIE VORGABE, auf der V1200 gemessen (20.09., A/B in derselben
 * Binaerdatei, beide Laeufe mit Hauptgebaeude-Explosion):
 *
 *                       libnix        AllocVec
 *   je Freigabe         131 us        72 us      (-45 %)
 *   Aufraeumbild      96915 us     65258 us      (-33 %)
 *   [OK] bench         63,3 fps      66,1 fps
 *
 * UND DIE SPEICHERBILANZ IST BESSER, nicht schlechter -- das war die Sorge:
 *
 *                       libnix        AllocVec
 *   frei nach Karte 2   45608 KB      46683 KB
 *   groesster Block     39845 KB      41241 KB
 *
 * Kein Widerspruch, sondern der bekannte Mechanismus: libnix gibt Speicher
 * NIE ans System zurueck, FreeVec schon. Genau daran lag das Schlieren ab
 * der 3./4. Karte.
 *
 * `SetEnv ZOD_ROTOMEM libnix` ist der Rueckweg, fuer jeden kuenftigen
 * A/B-Vergleich -- dasselbe Muster wie bei ZOD_COPY.
 *
 * Der Hinweis greift NUR dort, wo SDL_rotozoom ihn setzt. Ein blanker
 * Umstieg fuer ALLE kleinen Flaechen bleibt ausgeschlossen: die rund 14000
 * Flaechen vom Start wuerden exec's Liste dauerhaft verlaengern. */
static int zod_rotomem_exec  = 1;   /* Vorgabe: an; ZOD_ROTOMEM libnix zurueck */
static int zod_big_hint_tiefe = 0;  /* Schachtelungszaehler */

extern "C" void zod_surface_big_hint(int on)
{
	/* Beim ERSTEN Gebrauch lesen, nicht in einer Startfunktion.
	 *
	 * Erster Versuch war `zod_dirty_set_target` -- und die laeuft in
	 * `make shimtest` nie, der Schalter war dort also wirkungslos. Die Sonde
	 * hat es gezeigt (gleiche Zahlen mit und ohne), sonst haette der Nutzer
	 * einen Schalter gemessen, der nichts tut.
	 *
	 * Kein Problem mit der LockBitMap-Regel: diese Funktion ruft
	 * SDL_rotozoom, nie der Ausgabeweg. */
	static int gelesen = 0;

	if(!gelesen)
	{
		char wahl[16];

		gelesen = 1;

		if(zod_env("ZOD_ROTOMEM", wahl, sizeof(wahl)))
		{
			if(wahl[0] == 'l' || wahl[0] == 'L') zod_rotomem_exec = 0;
			if(wahl[0] == 'e' || wahl[0] == 'E') zod_rotomem_exec = 1;
		}
	}

	if(!zod_rotomem_exec) return;

	if(on) zod_big_hint_tiefe++;
	else if(zod_big_hint_tiefe > 0) zod_big_hint_tiefe--;
}

extern "C" int zod_dirty_get_luecke(void) { return luecke; }

static void rechteck_ungueltig(void);   /* weiter unten, Scrollen */
extern "C" { static void fen_einrichten(SDL_Surface *s); }   /* weiter unten, Fensterschub */

extern "C" void zod_dirty_set_target(SDL_Surface *s)
{
	char wahl[16];

	if(zod_env("ZOD_DIRTY", wahl, sizeof(wahl)))
		if(wahl[0] == 'a' || wahl[0] == 'A' || wahl[0] == '0')
			dirty_an = 0;

	/* Laeufe zusammenfassen: SetEnv ZOD_LUECKE <bloecke>. Kein atoi -- das
	 * liest unter libnix nichts Falsches, aber die Ziffern hier selbst zu
	 * nehmen ist kuerzer als der Beweis, dass es richtig ist. */
	if(zod_env("ZOD_LUECKE", wahl, sizeof(wahl)))
	{
		int v = 0, i = 0, ziffern = 0;

		while(wahl[i] >= '0' && wahl[i] <= '9')
		{
			v = v * 10 + (wahl[i] - '0');
			i++;
			ziffern++;
		}

		if(ziffern) zod_dirty_set_luecke(v);
	}

	if(zod_env("ZOD_SWEEP", wahl, sizeof(wahl)))
		if(wahl[0] == '1' || wahl[0] == 'j' || wahl[0] == 'J')
			sweep_an = 1;

	/* Der Schalter selbst wird beim ersten Gebrauch gelesen (siehe
	 * zod_surface_big_hint) -- hier nur die Protokollzeile, damit ein
	 * Mitschnitt zuzuordnen ist. */
	zod_surface_big_hint(0);

	ZLOG("Drehflaechen-Speicher: %s\n",
	     zod_rotomem_exec
	         ? "AllocVec (Vorgabe) -- SetEnv ZOD_ROTOMEM libnix zum Vergleichen"
	         : "libnix (ZOD_ROTOMEM libnix)");

	dirty_target = s;

	fen_einrichten(s);

	grid_w = s ? ((s->w + (1 << ZOD_BLOCK_SHIFT) - 1) >> ZOD_BLOCK_SHIFT) : 0;
	grid_h = s ? ((s->h + (1 << ZOD_BLOCK_SHIFT) - 1) >> ZOD_BLOCK_SHIFT) : 0;

	if(grid_w > ZOD_GRID_W) grid_w = ZOD_GRID_W;
	if(grid_h > ZOD_GRID_H) grid_h = ZOD_GRID_H;

	ZLOG("Ausgabe: %s, Raster %ldx%ld Bloecke zu %ld Punkten, Luecke %ld\n",
	     dirty_an ? "nur geaenderte Bereiche" : "ganzes Bild (ZOD_DIRTY=aus)",
	     (long)grid_w, (long)grid_h, (long)(1 << ZOD_BLOCK_SHIFT),
	     (long)luecke);

	/* Erstes Bild vollstaendig: was im Schirm steht, wissen wir nicht. */
	grid_alles(out_grid, 1);
	grid_alles(obj_grid, 1);
	grid_alles(prev_grid, 1);
	prev_gueltig = 0;
	rechteck_ungueltig();
}

extern "C" void zod_dirty_restore_begin(void) { im_restore = 1; }
extern "C" void zod_dirty_restore_end(void)   { im_restore = 0; }

extern "C" void zod_dirty_mark(void *dstv, int x, int y, int w, int h);

/* ---- Scrollen mit dem Blitter der Karte (Warp, 17.11.2026) --------------
 *
 * Bisher: jede Verschiebung des Ausschnitts = zod_dirty_force_full, das
 * ganze Bild geht in den Grafikspeicher (640x480: 6,7 ms mit MOVE16).
 *
 * Jetzt: der Kartenausschnitt R wird IM GRAFIKSPEICHER verschoben
 * (BltBitMap, Blitter der Warp, SDL_Flip), und kopiert werden nur Bloecke,
 * in denen der verschobene Speicherinhalt von der Zeichenflaeche abweichen
 * kann:
 *   1. die freigelegten Streifen am Rand von R,
 *   2. die Objektbloecke des LETZTEN Bildes (prev_grid), um die Verschiebung
 *      versetzt -- der Blitter hat die alten Einheiten mitgeschoben,
 *   3. alles, was in diesem Bild gezeichnet wird (wie immer),
 *   4. der alte Mauszeiger, versetzt (sdl_screen.cpp, zeiger_einsetzen).
 * Der volle Hintergrundblit dieses Bildes wird NICHT vermerkt
 * (zod_dirty_scrollblit_*): er erneuert nur, was der Blitter schon richtig
 * hingeschoben hat.
 *
 * Grundsatz: die Zeichenflaeche ist immer richtig. Ein Block zu viel kostet
 * nur Zeit, ein Block zu wenig waere ein Fehler -- deshalb wird grosszuegig
 * auf ganze Bloecke gerundet. Pruefung: ZOD_VERIFY (sdl_screen.cpp).
 * Abschalten: SetEnv ZOD_BLITSCROLL aus. */
static int scroll_erlaubt = -1;
static int scroll_pend = 0;
static int scroll_dx, scroll_dy, scroll_rx, scroll_ry, scroll_rw, scroll_rh;
static int im_scrollblit = 0;
unsigned long zod_scroll_blits = 0, zod_scroll_abgelehnt = 0;
/* Gruende: 1 aus, 2 Vorspur ungueltig, 3 noch eine offen, 4 Sprung zu gross, 5 Lage */
unsigned long zod_scroll_grund[6];

/* GENAUE Objektrechtecke je Bild (nicht nur Bloecke). Beim Scrollen wird ein
 * Block um eine Verschiebung, die kein Vielfaches von 16 ist, auf 2x2
 * Bloecke verschmiert -- jedes unbewegte Kartenobjekt kostete damit das
 * Vierfache. Ein genaues Rechteck faellt dagegen fuer ein unbewegtes Objekt
 * exakt auf sein neues Rechteck, das ohnehin vermerkt wird.
 * Laeuft die Liste ueber, gilt fuer dieses Bild der Blockweg. */
enum { ZOD_RECHTECKE = 4096 };
struct zod_rechteck { short x, y, w, h; };
static zod_rechteck rechteck_a[ZOD_RECHTECKE], rechteck_b[ZOD_RECHTECKE];
static zod_rechteck *rechteck_jetzt = rechteck_a, *rechteck_vor = rechteck_b;
static int rechteck_jetzt_n = 0, rechteck_vor_n = 0;
static int rechteck_jetzt_voll = 0, rechteck_vor_voll = 1;
unsigned long zod_scroll_genau = 0, zod_scroll_block = 0, zod_scroll_rechtecke = 0;
unsigned long zod_scroll_bl_innen = 0, zod_scroll_bl_aussen = 0;

/* Diagnose fuer SDL_Flip: Bloecke der Ausgabespur innerhalb/ausserhalb von R
 * in einem Scrollbild zaehlen */
extern "C" void zod_dirty_scroll_zaehlen(int rx, int ry, int rw, int rh)
{
	for(int by = 0; by < grid_h; by++)
		for(int bx = 0; bx < grid_w; bx++)
		{
			if(!out_grid[by][bx]) continue;

			const int x = bx << ZOD_BLOCK_SHIFT, y = by << ZOD_BLOCK_SHIFT;

			if(x >= rx && y >= ry && x < rx + rw && y < ry + rh) zod_scroll_bl_innen++;
			else zod_scroll_bl_aussen++;
		}
}

static inline void rechteck_merken(int x, int y, int w, int h)
{
	if(rechteck_jetzt_voll) return;

	if(rechteck_jetzt_n >= ZOD_RECHTECKE) { rechteck_jetzt_voll = 1; return; }

	zod_rechteck &r = rechteck_jetzt[rechteck_jetzt_n++];

	r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
}

static void rechteck_ungueltig(void)
{
	rechteck_jetzt_voll = 1;
	rechteck_vor_voll = 1;
}

/* in zod_dirty_clear: Liste dieses Bildes wird die Vorliste */
static void rechteck_tauschen(void)
{
	zod_rechteck *t = rechteck_vor;

	rechteck_vor = rechteck_jetzt;
	rechteck_jetzt = t;
	rechteck_vor_n = rechteck_jetzt_n;
	rechteck_vor_voll = rechteck_jetzt_voll;
	rechteck_jetzt_n = 0;
	rechteck_jetzt_voll = 0;
}

static void out_mark(int x, int y, int w, int h)
{
	if(w <= 0 || h <= 0 || !grid_w) return;

	if(x < 0) { w += x; x = 0; }
	if(y < 0) { h += y; y = 0; }
	if(w <= 0 || h <= 0) return;

	int bx0 = x >> ZOD_BLOCK_SHIFT;
	int by0 = y >> ZOD_BLOCK_SHIFT;
	int bx1 = (x + w - 1) >> ZOD_BLOCK_SHIFT;
	int by1 = (y + h - 1) >> ZOD_BLOCK_SHIFT;

	if(bx1 >= grid_w) bx1 = grid_w - 1;
	if(by1 >= grid_h) by1 = grid_h - 1;

	for(int by = by0; by <= by1; by++)
		for(int bx = bx0; bx <= bx1; bx++)
			out_grid[by][bx] = 1;
}

/* schirmfeste Rechtecke (Definition bei dirty_mark) */
struct zod_rechteck;
static zod_rechteck *fest_vor_liste(int *n);

/* Aus ZMap::DoRender, wenn sich NUR der Ausschnitt verschoben hat.
 * dx/dy: Verschiebung der Karte in Punkten (shift neu - shift alt), R in
 * Schirmkoordinaten. Rueckgabe 1 = uebernommen, 0 = voller Weg wie bisher. */
extern "C" int zod_dirty_scroll(int dx, int dy, int rx, int ry, int rw, int rh)
{
	if(scroll_erlaubt < 0)
	{
		char wahl[16];

		scroll_erlaubt = 1;

		if(zod_env("ZOD_BLITSCROLL", wahl, sizeof(wahl)))
			if(wahl[0] == 'a' || wahl[0] == 'A' || wahl[0] == '0')
				scroll_erlaubt = 0;

		ZLOG("Scrollen: %s\n", scroll_erlaubt
		     ? "Blitter im Grafikspeicher + Streifen (Vorgabe)"
		     : "ganzes Bild (ZOD_BLITSCROLL aus)");
	}

	{
		int grund = 0;

		if(!scroll_erlaubt || !dirty_an || !grid_w) grund = 1;
		else if(!prev_gueltig) grund = 2;
		else if(scroll_pend) grund = 3;
		else if(rw <= 0 || rh <= 0 || dx >= rw || -dx >= rw || dy >= rh || -dy >= rh) grund = 4;
		else if(rx < 0 || ry < 0 || rx + rw > dirty_target->w || ry + rh > dirty_target->h) grund = 5;

		if(grund)
		{
			zod_scroll_abgelehnt++;
			zod_scroll_grund[grund]++;

			if(zod_scroll_abgelehnt <= 6)
				ZLOG("Scrollen abgelehnt (%ld): d=%ld,%ld R=%ld,%ld %ldx%ld\n",
				     (long)grund, (long)dx, (long)dy, (long)rx, (long)ry,
				     (long)rw, (long)rh);
			return 0;
		}
	}

	/* 2. Objekte des letzten Bildes, soweit in R, um -d versetzt.
	 *    Genau ueber die Rechteckliste; ist sie uebergelaufen, ueber Bloecke. */
	const int block = 1 << ZOD_BLOCK_SHIFT;

	if(!rechteck_vor_voll)
	{
		zod_scroll_genau++;

		for(int i = 0; i < rechteck_vor_n; i++)
		{
			const zod_rechteck &r = rechteck_vor[i];

			/* 2 Punkte Rand: manches zeichnet knapp neben sein gemeldetes
			 * Rechteck (gemessen: 18 Punkte in 2 von 34 Bildern ohne Rand) */
			int x0 = r.x - 2, y0 = r.y - 2, x1 = r.x + r.w + 2, y1 = r.y + r.h + 2;

			if(x0 < rx) x0 = rx;
			if(y0 < ry) y0 = ry;
			if(x1 > rx + rw) x1 = rx + rw;
			if(y1 > ry + rh) y1 = ry + rh;

			if(x1 <= x0 || y1 <= y0) continue;

			out_mark(x0 - dx, y0 - dy, x1 - x0, y1 - y0);
		}
	}
	else
	{
	zod_scroll_block++;

	for(int by = 0; by < grid_h; by++)
		for(int bx = 0; bx < grid_w; bx++)
		{
			if(!prev_grid[by][bx]) continue;

			int x0 = bx << ZOD_BLOCK_SHIFT, y0 = by << ZOD_BLOCK_SHIFT;
			int x1 = x0 + block, y1 = y0 + block;

			if(x0 < rx) x0 = rx;
			if(y0 < ry) y0 = ry;
			if(x1 > rx + rw) x1 = rx + rw;
			if(y1 > ry + rh) y1 = ry + rh;

			if(x1 <= x0 || y1 <= y0) continue;

			out_mark(x0 - dx, y0 - dy, x1 - x0, y1 - y0);
		}
	}

	/* 3. schirmfeste Rechtecke des letzten Bildes: der Blit spart sie aus,
	 *    also muessen sie kopiert werden (verschwundene Meldung) */
	{
		int fn = 0;
		const zod_rechteck *fv = fest_vor_liste(&fn);

		for(int i = 0; i < fn; i++)
			out_mark(fv[i].x - 2, fv[i].y - 2, fv[i].w + 4, fv[i].h + 4);
	}

	/* 1. freigelegte Streifen */
	if(dx > 0) out_mark(rx + rw - dx, ry, dx, rh);
	else if(dx < 0) out_mark(rx, ry, -dx, rh);

	if(dy > 0) out_mark(rx, ry + rh - dy, rw, dy);
	else if(dy < 0) out_mark(rx, ry, rw, -dy);

	scroll_pend = 1;
	zod_scroll_rechtecke += (unsigned long)rechteck_vor_n;
	scroll_dx = dx; scroll_dy = dy;
	scroll_rx = rx; scroll_ry = ry; scroll_rw = rw; scroll_rh = rh;

	return 1;
}

/* Fuer SDL_Flip: liegt eine Verschiebung an? */
extern "C" int zod_dirty_scroll_get(int *dx, int *dy, int *rx, int *ry, int *rw, int *rh)
{
	if(!scroll_pend) return 0;

	*dx = scroll_dx; *dy = scroll_dy;
	*rx = scroll_rx; *ry = scroll_ry; *rw = scroll_rw; *rh = scroll_rh;

	return 1;
}

extern "C" void zod_dirty_scroll_done(void)
{
	if(scroll_pend) zod_scroll_blits++;

	scroll_pend = 0;
}

/* Fuer den Mauszeiger: ein Rechteck NUR in die Ausgabespur */
extern "C" void zod_dirty_out_mark(int x, int y, int w, int h)
{
	out_mark(x, y, w, h);
}

extern "C" void zod_dirty_scrollblit_begin(void) { im_scrollblit = 1; }
extern "C" void zod_dirty_scrollblit_end(void)   { im_scrollblit = 0; }

/* ---- Fensterschub: Zeichenflaeche verschieben statt neu fuellen ----------
 * (Warp F3, 17.11.2026)
 *
 * Beim Scrollen kopierte ZMap::DoRender in JEDEM Bild den ganzen
 * Kartenausschnitt aus der Kartenflaeche in die Zeichenflaeche (640x480:
 * ~240 KB, 9,6 ms auf Amiga 1 -- die groesste Einzelpost im Scrollbild).
 *
 * Jetzt liegt die Zeichenflaeche in einem groesseren Block, und beim Scrollen
 * wandert nur ihr Pixelzeiger um dy*pitch + dx. Dann steht im Ausschnitt R
 * schon das alte Bild, um d verschoben -- genau das, was der Blitter im
 * Grafikspeicher auch daraus macht. Aus der Kartenflaeche geholt wird nur:
 *   1. die freigelegten Streifen,
 *   2. was im LETZTEN Bild ueber den Hintergrund gezeichnet wurde (Objekte,
 *      Meldungen, Fenster -- die Rechteckliste bzw. die Blockspur), versetzt.
 * Alles AUSSERHALB von R (Anzeige rechts und unten) darf sich nicht bewegen:
 * es wird vom alten an den neuen Platz zurueckkopiert (Kopie mit Ueberlappung,
 * Reihenfolge nach der Richtung des Schubs).
 *
 * Laeuft der Zeiger aus dem Block, wird er in die Mitte gesetzt und R wie
 * bisher ganz gefuellt (Rueckgabe 0).
 *
 * Grundsatz wie beim Blitter-Scrollen: danach muss R in der Zeichenflaeche
 * GENAU der Kartenhintergrund sein. `SetEnv ZOD_FENSTER 2` prueft das in
 * jedem Schub Byte fuer Byte gegen die Kartenflaeche (und stellt bei einer
 * Abweichung den vollen Hintergrund her), `ZOD_FENSTER aus` = alter Weg. */
static int fen_modus = -1;              /* 0 aus, 1 an, 2 an + Pruefung */
static unsigned char *fen_block = 0, *fen_lo = 0, *fen_hi = 0, *fen_mitte = 0;
static SDL_Surface *fen_flaeche = 0;
unsigned long zod_fen_schuebe = 0, zod_fen_mitte = 0, zod_fen_rechtecke = 0;
unsigned long zod_fen_geprueft = 0, zod_fen_falsch = 0, zod_fen_falsch_punkte = 0;
unsigned long long zod_fen_bytes = 0, zod_fen_rand_bytes = 0;

static int fen_modus_lesen(void)
{
	if(fen_modus >= 0) return fen_modus;

	char wahl[16];

	/* VORGABE AUS (Amiga 1, 17.11.2026, plan 6r): R wird billiger (640x480
	 * 9,7 -> 7,4 ms), aber der verschobene Zeiger liegt nicht mehr auf
	 * derselben 16-Byte-Lage wie der Grafikspeicher -- die Bildausgabe
	 * verliert MOVE16 und wird um genau so viel teurer (4,2 -> 6,3 ms;
	 * 800x600 sogar 6,0 -> 9,6). Netto 0 bzw. -3 %. */
	fen_modus = 0;

	if(zod_env("ZOD_FENSTER", wahl, sizeof(wahl)))
	{
		if(wahl[0] == '1' || wahl[0] == 'e' || wahl[0] == 'E' ||
		   ((wahl[0] == 'a' || wahl[0] == 'A') && (wahl[1] == 'n' || wahl[1] == 'N')))
			fen_modus = 1;
		else if(wahl[0] == '2') fen_modus = 2;
	}

	ZLOG("Fensterschub: %s\n", fen_modus == 0 ? "aus (Vorgabe; ZOD_FENSTER an / 2 zum Vergleichen)"
	     : fen_modus == 2 ? "an, mit Pruefung gegen die Kartenflaeche (ZOD_FENSTER 2)"
	     : "an (ZOD_FENSTER an)");

	return fen_modus;
}

/* Kopie mit Ueberlappung, vorwaerts (Ziel vor der Quelle) bzw. rueckwaerts.
 * Langwoerter, das Ziel ausgerichtet; schiefe Lesezugriffe sind ab 020 erlaubt. */
static void fen_vor(unsigned char *d, const unsigned char *s, int n)
{
	while(n > 0 && ((unsigned long)d & 3)) { *d++ = *s++; n--; }

	unsigned long *dl = (unsigned long*)d;
	const unsigned long *sl = (const unsigned long*)s;

	while(n >= 16) { dl[0] = sl[0]; dl[1] = sl[1]; dl[2] = sl[2]; dl[3] = sl[3]; dl += 4; sl += 4; n -= 16; }
	while(n >= 4) { *dl++ = *sl++; n -= 4; }

	d = (unsigned char*)dl;
	s = (const unsigned char*)sl;

	while(n-- > 0) *d++ = *s++;
}

static void fen_rueck(unsigned char *d, const unsigned char *s, int n)
{
	d += n;
	s += n;

	while(n > 0 && ((unsigned long)d & 3)) { *--d = *--s; n--; }

	unsigned long *dl = (unsigned long*)d;
	const unsigned long *sl = (const unsigned long*)s;

	while(n >= 16) { dl[-1] = sl[-1]; dl[-2] = sl[-2]; dl[-3] = sl[-3]; dl[-4] = sl[-4]; dl -= 4; sl -= 4; n -= 16; }
	while(n >= 4) { *--dl = *--sl; n -= 4; }

	d = (unsigned char*)dl;
	s = (const unsigned char*)sl;

	while(n-- > 0) *--d = *--s;
}

/* Alles ausserhalb von R vom alten Zeiger `alt` an den neuen `neu`, in der
 * Reihenfolge, in der keine Quelle ueberschrieben wird, bevor sie gelesen
 * ist (neu > alt: von der hoechsten Adresse abwaerts). */
static void fen_rand_umsetzen(unsigned char *alt, unsigned char *neu, int pitch,
                              int w, int h, int rx, int ry, int rw, int rh)
{
	const int rueck = neu > alt;

	for(int i = 0; i < h; i++)
	{
		const int y = rueck ? (h - 1 - i) : i;
		const unsigned long z = (unsigned long)y * (unsigned long)pitch;
		int sa[2], sl[2], ns = 0;

		if(y < ry || y >= ry + rh) { sa[0] = 0; sl[0] = w; ns = 1; }
		else
		{
			if(rx > 0) { sa[ns] = 0; sl[ns] = rx; ns++; }
			if(rx + rw < w) { sa[ns] = rx + rw; sl[ns] = w - (rx + rw); ns++; }
		}

		for(int k = 0; k < ns; k++)
		{
			const int j = rueck ? (ns - 1 - k) : k;

			if(rueck) fen_rueck(neu + z + sa[j], alt + z + sa[j], sl[j]);
			else      fen_vor(neu + z + sa[j], alt + z + sa[j], sl[j]);

			zod_fen_rand_bytes += (unsigned long)sl[j];
		}
	}
}

/* Rechteck (Schirmkoordinaten, auf R geklemmt) aus der Kartenflaeche holen.
 * bx/by: Kartenpunkt, der auf (rx, ry) liegt. */
static inline void fen_holen(SDL_Surface *z, SDL_Surface *bg, int bx, int by,
                             int rx, int ry, int rw, int rh,
                             int x0, int y0, int x1, int y1)
{
	if(x0 < rx) x0 = rx;
	if(y0 < ry) y0 = ry;
	if(x1 > rx + rw) x1 = rx + rw;
	if(y1 > ry + rh) y1 = ry + rh;

	if(x1 <= x0 || y1 <= y0) return;

	zod_copy_rect((unsigned char*)z->pixels + (unsigned long)y0 * z->pitch + x0,
	              (const unsigned char*)bg->pixels
	                  + (unsigned long)(by + y0 - ry) * bg->pitch + (bx + x0 - rx),
	              x1 - x0, y1 - y0, (int)z->pitch, (int)bg->pitch);

	zod_fen_rechtecke++;
	zod_fen_bytes += (unsigned long)(x1 - x0) * (unsigned long)(y1 - y0);
}

/* Geaenderte Stellen der KARTENFLAECHE (Krater, Kacheln, Steine), in
 * Kartenpunkten, seit dem letzten Schub. Der volle Blit holte sie frueher
 * nebenbei; ohne diese Liste stuende im verschobenen Bild noch der alte
 * Boden (ZOD_FENSTER 2: 7 von 975 Schueben, orig07). */
enum { FEN_STEMPEL = 128 };
static short fen_st[FEN_STEMPEL][4];
static int fen_st_n = 0, fen_st_voll = 0;
unsigned long zod_fen_stempel = 0, zod_fen_stempel_voll = 0;

extern "C" void zod_fenster_stempel(int x, int y, int w, int h)
{
	if(fen_modus <= 0 || w <= 0 || h <= 0) return;

	zod_fen_stempel++;

	if(fen_st_n >= FEN_STEMPEL) { fen_st_voll = 1; return; }

	fen_st[fen_st_n][0] = (short)x; fen_st[fen_st_n][1] = (short)y;
	fen_st[fen_st_n][2] = (short)w; fen_st[fen_st_n][3] = (short)h;
	fen_st_n++;
}

/* Aus ZMap::DoRender im Scrollbild, NACH zod_dirty_scroll (das die Listen des
 * letzten Bildes noch unveraendert vorfindet). bg = Kartenflaeche, bx/by =
 * neuer Kartenversatz (Kartenpunkt auf rx/ry), d = Verschiebung.
 * Rueckgabe 1 = R ist fertiger Hintergrund, 0 = Aufrufer fuellt R selbst. */
extern "C" int zod_fenster_schub(SDL_Surface *bg, int bx, int by, int dx, int dy,
                                 int rx, int ry, int rw, int rh)
{
	SDL_Surface *z = dirty_target;

	if(fen_modus_lesen() == 0 || !fen_flaeche || z != fen_flaeche || !bg || !bg->pixels)
		return 0;
	if(bg->format->BytesPerPixel != 1 || bx < 0 || by < 0 ||
	   bx + rw > bg->w || by + rh > bg->h)
		return 0;

	unsigned char *alt = (unsigned char*)z->pixels;
	const long schub = (long)dy * (long)z->pitch + (long)dx;
	unsigned char *neu = alt + schub;
	const unsigned long bild = (unsigned long)z->pitch * (unsigned long)z->h;

	if(neu < fen_lo || neu + bild > fen_hi || fen_st_voll)
	{
		const int st_voll = fen_st_voll;

		fen_st_n = 0;
		fen_st_voll = 0;

		if(st_voll && neu >= fen_lo && neu + bild <= fen_hi)
		{
			/* zu viele Stempel: verschieben, aber R ganz fuellen lassen */
			fen_rand_umsetzen(alt, neu, z->pitch, z->w, z->h, rx, ry, rw, rh);
			z->pixels = neu;
			zod_fen_stempel_voll++;

			return 0;
		}

		/* Rand erschoepft: in die Mitte, Anzeige mitnehmen, R fuellt der
		 * Aufrufer wie bisher. */
		neu = fen_mitte;
		fen_rand_umsetzen(alt, neu, z->pitch, z->w, z->h, rx, ry, rw, rh);
		z->pixels = neu;
		zod_fen_mitte++;

		return 0;
	}

	fen_rand_umsetzen(alt, neu, z->pitch, z->w, z->h, rx, ry, rw, rh);
	z->pixels = neu;
	zod_fen_schuebe++;

	/* 1. freigelegte Streifen */
	if(dx > 0)      fen_holen(z, bg, bx, by, rx, ry, rw, rh, rx + rw - dx, ry, rx + rw, ry + rh);
	else if(dx < 0) fen_holen(z, bg, bx, by, rx, ry, rw, rh, rx, ry, rx - dx, ry + rh);

	if(dy > 0)      fen_holen(z, bg, bx, by, rx, ry, rw, rh, rx, ry + rh - dy, rx + rw, ry + rh);
	else if(dy < 0) fen_holen(z, bg, bx, by, rx, ry, rw, rh, rx, ry, rx + rw, ry - dy);

	/* 2. was im letzten Bild ueber den Hintergrund kam, an seinem neuen Platz
	 *    (dieselbe Menge, die zod_dirty_scroll zur Ausgabe vermerkt) */
	if(!rechteck_vor_voll)
	{
		for(int i = 0; i < rechteck_vor_n; i++)
		{
			const zod_rechteck &r = rechteck_vor[i];
			int x0 = r.x - 2, y0 = r.y - 2, x1 = r.x + r.w + 2, y1 = r.y + r.h + 2;

			if(x0 < rx) x0 = rx;
			if(y0 < ry) y0 = ry;
			if(x1 > rx + rw) x1 = rx + rw;
			if(y1 > ry + rh) y1 = ry + rh;

			if(x1 <= x0 || y1 <= y0) continue;

			fen_holen(z, bg, bx, by, rx, ry, rw, rh, x0 - dx, y0 - dy, x1 - dx, y1 - dy);
		}
	}
	else
	{
		const int block = 1 << ZOD_BLOCK_SHIFT;

		for(int gy = 0; gy < grid_h; gy++)
		{
			int gx = 0;

			while(gx < grid_w)
			{
				if(!prev_grid[gy][gx]) { gx++; continue; }

				const int a = gx;

				while(gx < grid_w && prev_grid[gy][gx]) gx++;

				int x0 = a << ZOD_BLOCK_SHIFT, y0 = gy << ZOD_BLOCK_SHIFT;
				int x1 = gx << ZOD_BLOCK_SHIFT, y1 = y0 + block;

				if(x0 < rx) x0 = rx;
				if(y0 < ry) y0 = ry;
				if(x1 > rx + rw) x1 = rx + rw;
				if(y1 > ry + rh) y1 = ry + rh;

				if(x1 <= x0 || y1 <= y0) continue;

				fen_holen(z, bg, bx, by, rx, ry, rw, rh, x0 - dx, y0 - dy, x1 - dx, y1 - dy);
			}
		}
	}

	/* 3. geaenderte Stellen der Kartenflaeche (Kartenpunkte -> Schirm) */
	for(int i = 0; i < fen_st_n; i++)
	{
		const int x0 = fen_st[i][0] - bx + rx, y0 = fen_st[i][1] - by + ry;

		fen_holen(z, bg, bx, by, rx, ry, rw, rh, x0, y0, x0 + fen_st[i][2], y0 + fen_st[i][3]);
	}

	fen_st_n = 0;

	/* Pruefung: R muss jetzt genau der Kartenhintergrund sein */
	if(fen_modus == 2)
	{
		unsigned long falsch = 0;

		for(int y = 0; y < rh; y++)
		{
			const unsigned char *a = (const unsigned char*)z->pixels
			                         + (unsigned long)(ry + y) * z->pitch + rx;
			const unsigned char *b = (const unsigned char*)bg->pixels
			                         + (unsigned long)(by + y) * bg->pitch + bx;

			if(memcmp(a, b, (size_t)rw))
				for(int x = 0; x < rw; x++)
					if(a[x] != b[x])
					{
						if(falsch < 4 && zod_fen_falsch < 8)
							ZLOG("Fensterschub FALSCH: Punkt %ld,%ld (d=%ld,%ld)\n",
							     (long)(rx + x), (long)(ry + y), (long)dx, (long)dy);
						falsch++;
					}
		}

		zod_fen_geprueft++;

		if(falsch)
		{
			zod_fen_falsch++;
			zod_fen_falsch_punkte += falsch;

			return 0;           /* Aufrufer stellt den vollen Hintergrund her */
		}
	}

	return 1;
}

extern "C" void zod_fensterschub_report(void)
{
	if(fen_modus <= 0) return;

	ZLOG("Fensterschub: %lu Schuebe, %lu mal Mitte; je Schub %lu Rechtecke, %lu Byte aus der "
	     "Karte, %lu Byte Anzeige umgesetzt\n",
	     zod_fen_schuebe, zod_fen_mitte,
	     zod_fen_schuebe ? zod_fen_rechtecke / zod_fen_schuebe : 0UL,
	     zod_fen_schuebe ? (unsigned long)(zod_fen_bytes / zod_fen_schuebe) : 0UL,
	     (zod_fen_schuebe + zod_fen_mitte)
	         ? (unsigned long)(zod_fen_rand_bytes / (zod_fen_schuebe + zod_fen_mitte)) : 0UL);

	ZLOG("Fensterschub: %lu Stempel in der Kartenflaeche, %lu mal zu viele (voller Hintergrund)\n",
	     zod_fen_stempel, zod_fen_stempel_voll);

	if(fen_modus == 2)
		ZLOG("Fensterschub Pruefung: %lu Schuebe geprueft, %lu falsch (%lu Punkte) -- muss 0 sein\n",
		     zod_fen_geprueft, zod_fen_falsch, zod_fen_falsch_punkte);
}

/* Warp F3: "seit dem Hintergrund beschrieben" in 8x8-Zellen.
 *
 * ZPlayer schaltet das nach zmap.DoRender ein (zod_overdraw_begin) und nach
 * dem Objektdurchlauf aus. Ein eingebackener Steinkopf darf nur dann
 * ausgelassen werden, wenn in seinem Bereich bis zu seiner Stelle in der
 * Zeichenreihenfolge nichts gezeichnet wurde. Gefuellt wird hier in
 * dirty_mark -- durch die laeuft jeder Blit und jedes FillRect auf die
 * Zeichenflaeche. Der Wiederherstellungsblit zaehlt nicht (er IST der
 * Hintergrund). */
#define OD_MAX_W (2048 / 8)
#define OD_MAX_H (2048 / 8)
static unsigned char od_grid[OD_MAX_H][OD_MAX_W];
static int od_an = 0, od_w = 0, od_h = 0;

extern "C" void zod_overdraw_begin(void)
{
	if(!dirty_target) { od_an = 0; return; }

	od_w = (dirty_target->w + 7) >> 3;
	od_h = (dirty_target->h + 7) >> 3;
	if(od_w > OD_MAX_W) od_w = OD_MAX_W;
	if(od_h > OD_MAX_H) od_h = OD_MAX_H;

	for(int y = 0; y < od_h; y++) memset(od_grid[y], 0, (size_t)od_w);

	od_an = 1;
}

extern "C" void zod_overdraw_end(void) { od_an = 0; }

/* 1, wenn in x..x+w, y..y+h (Schirmpunkte) seit begin etwas gezeichnet wurde.
 * Ausserhalb des Schirms gilt als "nichts". */
extern "C" int zod_overdraw_test(int x, int y, int w, int h)
{
	if(!od_an) return 1;

	int x0 = x < 0 ? 0 : x >> 3;
	int y0 = y < 0 ? 0 : y >> 3;
	int x1 = (x + w - 1) >> 3;
	int y1 = (y + h - 1) >> 3;

	if(x1 >= od_w) x1 = od_w - 1;
	if(y1 >= od_h) y1 = od_h - 1;

	for(int cy = y0; cy <= y1; cy++)
		for(int cx = x0; cx <= x1; cx++)
			if(od_grid[cy][cx]) return 1;

	return 0;
}

static inline void od_mark(int x, int y, int w, int h)
{
	int x0 = x < 0 ? 0 : x >> 3;
	int y0 = y < 0 ? 0 : y >> 3;
	int x1 = (x + w - 1) >> 3;
	int y1 = (y + h - 1) >> 3;

	if(x1 >= od_w) x1 = od_w - 1;
	if(y1 >= od_h) y1 = od_h - 1;

	for(int cy = y0; cy <= y1; cy++)
		for(int cx = x0; cx <= x1; cx++)
			od_grid[cy][cx] = 1;
}

/* Warp: SCHIRMFESTE Rechtecke (Pausenbild, Meldungen, Fenster, Zeiger,
 * Bildrate). Sie bewegen sich beim Scrollen NICHT mit der Karte. Der
 * Blitter verschiebt das Bild aber im SICHTBAREN Grafikspeicher -- bis die
 * CPU sie neu hineinkopiert, standen sie fuer einen Teil des Bildes am
 * falschen Platz (vom Nutzer als Flackern und "alter Platz" des
 * "Click to Resume" gesehen). Deshalb spart der Scrollblit diese Rechtecke
 * und ihre verschobenen Abbilder aus (zod_scroll_teile), die CPU kopiert sie
 * wie bisher. ZPlayer klammert das Zeichnen mit zod_fest_begin/end. */
enum { ZOD_FEST = 128 };
static zod_rechteck fest_a[ZOD_FEST], fest_b[ZOD_FEST];
static zod_rechteck *fest_jetzt = fest_a, *fest_vor = fest_b;
static int fest_jetzt_n = 0, fest_vor_n = 0, fest_jetzt_voll = 0, fest_vor_voll = 0;
static int fest_an = 0;
unsigned long zod_scroll_teile_bilder = 0, zod_scroll_teile_summe = 0, zod_scroll_teile_voll = 0;

static zod_rechteck *fest_vor_liste(int *n) { *n = fest_vor_n; return fest_vor; }

extern "C" void zod_fest_begin(void) { fest_an = 1; }
extern "C" void zod_fest_end(void)   { fest_an = 0; }

static inline void fest_merken(int x, int y, int w, int h)
{
	if(fest_jetzt_n >= ZOD_FEST) { fest_jetzt_voll = 1; return; }

	zod_rechteck &r = fest_jetzt[fest_jetzt_n++];

	r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
}

static void fest_tauschen(void)
{
	zod_rechteck *t = fest_vor;

	fest_vor = fest_jetzt;
	fest_jetzt = t;
	fest_vor_n = fest_jetzt_n;
	fest_vor_voll = fest_jetzt_voll;
	fest_jetzt_n = 0;
	fest_jetzt_voll = 0;
}

/* Zielflaeche des Scrollblits (zx,zy,bw,bh; Quelle = Ziel + dx,dy) ohne die
 * schirmfesten Rechtecke: F des letzten Bildes (steht im Grafikspeicher),
 * F - d (dorthin wuerde es verschoben) und F dieses Bildes (wird ohnehin
 * kopiert). Ergebnis als Baender nach y, je Band Stuecke nach x, und zwar
 * in der Reihenfolge, in der kein Stueck eine Quelle ueberschreibt, die ein
 * spaeteres noch liest (Quelle liegt in Richtung d: zuerst die Stuecke auf
 * der Seite, aus der gelesen wird). Rueckgabe: Zahl der Stuecke, 0 = nichts
 * auszusparen (ein Blit wie bisher), -1 = zu viele (ein Blit wie bisher). */
extern "C" int zod_scroll_teile(int zx, int zy, int bw, int bh, int dx, int dy,
                                short *teile, int max)
{
	enum { MAXEX = 3 * ZOD_FEST };
	static short ex[MAXEX][4];
	int nex = 0;

	if(fest_vor_voll || fest_jetzt_voll) { zod_scroll_teile_voll++; return -1; }

	const int zx1 = zx + bw, zy1 = zy + bh;

	for(int liste = 0; liste < 3; liste++)
	{
		const zod_rechteck *l = (liste == 2) ? fest_jetzt : fest_vor;
		const int n = (liste == 2) ? fest_jetzt_n : fest_vor_n;
		const int vx = (liste == 1) ? -dx : 0, vy = (liste == 1) ? -dy : 0;

		for(int i = 0; i < n && nex < MAXEX; i++)
		{
			int x0 = l[i].x - 2 + vx, y0 = l[i].y - 2 + vy;
			int x1 = l[i].x + l[i].w + 2 + vx, y1 = l[i].y + l[i].h + 2 + vy;

			if(x0 < zx) x0 = zx;
			if(y0 < zy) y0 = zy;
			if(x1 > zx1) x1 = zx1;
			if(y1 > zy1) y1 = zy1;
			if(x1 <= x0 || y1 <= y0) continue;

			ex[nex][0] = (short)x0; ex[nex][1] = (short)y0;
			ex[nex][2] = (short)x1; ex[nex][3] = (short)y1;
			nex++;
		}
	}

	if(!nex) return 0;

	/* Bandgrenzen */
	static short yc[2 * MAXEX + 2];
	int ny = 0;

	yc[ny++] = (short)zy;
	yc[ny++] = (short)zy1;
	for(int i = 0; i < nex; i++) { yc[ny++] = ex[i][1]; yc[ny++] = ex[i][3]; }

	for(int i = 1; i < ny; i++)   /* einfuegend sortieren, wenige Werte */
	{
		short v = yc[i];
		int j = i;

		while(j > 0 && yc[j - 1] > v) { yc[j] = yc[j - 1]; j--; }
		yc[j] = v;
	}

	int nt = 0;
	const int nb = ny - 1;

	for(int bi = 0; bi < nb; bi++)
	{
		const int b = (dy >= 0) ? bi : (nb - 1 - bi);
		const int ya = yc[b], yb = yc[b + 1];

		if(yb <= ya) continue;

		/* Ausschluesse dieses Bandes (decken es wegen der Grenzen ganz) */
		static short xa[MAXEX], xe[MAXEX];
		int nx = 0;

		for(int i = 0; i < nex; i++)
			if(ex[i][1] <= ya && ex[i][3] >= yb) { xa[nx] = ex[i][0]; xe[nx] = ex[i][2]; nx++; }

		for(int i = 1; i < nx; i++)
		{
			short a = xa[i], e = xe[i];
			int j = i;

			while(j > 0 && xa[j - 1] > a) { xa[j] = xa[j - 1]; xe[j] = xe[j - 1]; j--; }
			xa[j] = a; xe[j] = e;
		}

		/* freie Stuecke von links nach rechts sammeln */
		static short sa[MAXEX + 1], se[MAXEX + 1];
		int ns = 0, cur = zx;

		for(int i = 0; i < nx; i++)
		{
			if(xa[i] > cur) { sa[ns] = (short)cur; se[ns] = xa[i]; ns++; }
			if(xe[i] > cur) cur = xe[i];
		}
		if(cur < zx1) { sa[ns] = (short)cur; se[ns] = (short)zx1; ns++; }

		for(int k = 0; k < ns; k++)
		{
			const int s = (dx >= 0) ? k : (ns - 1 - k);

			if(nt >= max) { zod_scroll_teile_voll++; return -1; }

			teile[nt * 4 + 0] = sa[s];
			teile[nt * 4 + 1] = (short)ya;
			teile[nt * 4 + 2] = (short)(se[s] - sa[s]);
			teile[nt * 4 + 3] = (short)(yb - ya);
			nt++;
		}
	}

	zod_scroll_teile_bilder++;
	zod_scroll_teile_summe += (unsigned long)nt;

	return nt;
}

/* Einen beschriebenen Bereich vermerken. Nur fuer die Zeichenflaeche --
 * Blits in Karten- oder Zwischenflaechen gehen den Schirm nichts an. */
static inline void dirty_mark(SDL_Surface *dst, int x, int y, int w, int h)
{
	if(dst != dirty_target || w <= 0 || h <= 0) return;

	if(od_an && !im_restore) od_mark(x, y, w, h);
	if(fest_an && !im_restore) fest_merken(x, y, w, h);

	if(!grid_w) return;

	/* voller Hintergrundblit eines Scrollbildes: der Blitter hat das schon */
	if(im_scrollblit) return;

	/* genaues Rechteck fuer das naechste Scrollbild (nur Objekte) */
	if(!im_restore) rechteck_merken(x, y, w, h);

	int bx0 = x < 0 ? 0 : (x >> ZOD_BLOCK_SHIFT);
	int by0 = y < 0 ? 0 : (y >> ZOD_BLOCK_SHIFT);
	int bx1 = (x + w - 1) >> ZOD_BLOCK_SHIFT;
	int by1 = (y + h - 1) >> ZOD_BLOCK_SHIFT;

	if(bx1 >= grid_w) bx1 = grid_w - 1;
	if(by1 >= grid_h) by1 = grid_h - 1;

	for(int by = by0; by <= by1; by++)
		for(int bx = bx0; bx <= bx1; bx++)
		{
			out_grid[by][bx] = 1;

			/* Der Wiederherstellungsblit gehoert NICHT in die
			 * Objektspur -- sonst deckt er sich selbst wieder zu. */
			if(!im_restore) obj_grid[by][bx] = 1;
		}
}

extern "C" void zod_dirty_mark(void *dstv, int x, int y, int w, int h)
{
	dirty_mark((SDL_Surface*)dstv, x, y, w, h);
}

/* Laeufe zusammenhaengender Bloecke einer Blockzeile, in BILDPUNKTEN.
 * Rueckgabe: Anzahl der Laeufe.
 *
 * Kurze Luecken werden mitgenommen (siehe `luecke`). Ein Lauf deckt damit
 * immer MINDESTENS die schmutzigen Bloecke ab -- mehr zu kopieren oder mehr
 * Hintergrund wiederherzustellen ist in beiden Faellen folgenlos, weniger
 * waere ein Artefakt. */
static int grid_runs(const unsigned char g[ZOD_GRID_H][ZOD_GRID_W],
                     int by, short *x0, short *x1, int max)
{
	if(by < 0 || by >= grid_h || !dirty_target || max < 1) return 0;

	const int breite = dirty_target->w;
	int n = 0, bx = 0;

	while(bx < grid_w && n < max)
	{
		if(!g[by][bx]) { bx++; continue; }

		const int anfang = bx;
		int ende = bx;                  /* letzter schmutzige Block */

		while(bx < grid_w)
		{
			if(g[by][bx]) { ende = bx; bx++; continue; }

			/* Laenge der Luecke bestimmen. */
			int l = bx;

			while(l < grid_w && !g[by][l]) l++;

			/* Hinter der Luecke kommt nichts mehr, oder sie ist zu
			 * lang -- Lauf hier beenden. */
			if(l >= grid_w || (l - bx) > luecke) break;

			bx = l;
		}

		/* Letzter freier Platz, und rechts steht noch Schmutz: alles
		 * Restliche anhaengen. Sonst fiele es UNTER DEN TISCH, und das
		 * waere ein Artefakt -- bei 640 Punkten (40 Bloecke, hoechstens
		 * 20 Laeufe) kann das nicht auftreten, bei 2048 schon. */
		if(n == max - 1)
		{
			for(int r = grid_w - 1; r > ende; r--)
				if(g[by][r]) { ende = r; break; }
		}

		int a = anfang << ZOD_BLOCK_SHIFT;
		int b = ((ende + 1) << ZOD_BLOCK_SHIFT) - 1;

		if(b > breite - 1) b = breite - 1;

		if(b >= a) { x0[n] = (short)a; x1[n] = (short)b; n++; }
	}

	return n;
}

/* ---- Messmittel: die ganze Luecken-Kurve aus EINEM Lauf ------------------
 *
 * Getrennte Laeufe je Luecken-Wert waeren nicht vergleichbar -- jeder Lauf hat
 * andere Explosionen, andere Scrollstaende, andere Raster. Hier laufen alle
 * Kandidaten ueber DASSELBE Raster, Bild fuer Bild.
 *
 * Bewertet wird mit den auf der V1200 gemessenen Preisen (A/B 19.09.):
 * ein Kopieraufruf 1,48 us, ein Byte 7,34 ns. Das Modell sagt also voraus,
 * was `umschalten` bei jedem Wert kosten wuerde.
 *
 * Nur mit `SetEnv ZOD_SWEEP 1` -- sonst kostet es nichts. */
enum { SWEEP_N = 10 };
static const short sweep_l[SWEEP_N] = { 0, 2, 4, 6, 8, 12, 16, 24, 32, 999 };
static unsigned long sweep_aufrufe[SWEEP_N];
static unsigned long sweep_byte[SWEEP_N];

/* Aufrufe und Byte einer Blockzeile fuer eine gegebene Luecke -- dieselbe
 * Regel wie grid_runs, nur zaehlend statt eintragend. */
static void sweep_zeile(const unsigned char *z, int breite, int hoehe,
                        int L, unsigned long *aufrufe, unsigned long *byte)
{
	int bx = 0;

	while(bx < grid_w)
	{
		if(!z[bx]) { bx++; continue; }

		const int anfang = bx;
		int ende = bx;

		while(bx < grid_w)
		{
			if(z[bx]) { ende = bx; bx++; continue; }

			int l = bx;

			while(l < grid_w && !z[l]) l++;

			if(l >= grid_w || (l - bx) > L) break;

			bx = l;
		}

		int a = anfang << ZOD_BLOCK_SHIFT;
		int b = ((ende + 1) << ZOD_BLOCK_SHIFT) - 1;

		if(b > breite - 1) b = breite - 1;

		if(b >= a)
		{
			int n = b - a + 1;

			/* Genau wie SDL_Flip: fast breite Laeufe auffuellen, ganz
			 * breite in EINEM Aufruf ueber alle Zeilen. Der Zeilenabstand
			 * gilt hier als gleich der Breite -- bei 640 Punkten stimmt
			 * das, eine Auffuellung im Schirm zaehlt die Kurve nicht mit. */
			if((breite - n) <= L * (1 << ZOD_BLOCK_SHIFT)) n = breite;

			*aufrufe += (n == breite) ? 1UL : (unsigned long)hoehe;
			*byte    += (unsigned long)hoehe * (unsigned long)n;
		}
	}
}

extern "C" void zod_dirty_sweep(void)
{
	if(!sweep_an || !dirty_target) return;

	const int breite = dirty_target->w;
	const int block = 1 << ZOD_BLOCK_SHIFT;

	for(int by = 0; by < grid_h; by++)
	{
		/* Letzte Blockzeile kann angeschnitten sein. */
		int hoehe = dirty_target->h - (by << ZOD_BLOCK_SHIFT);

		if(hoehe > block) hoehe = block;
		if(hoehe <= 0) break;

		for(int i = 0; i < SWEEP_N; i++)
			sweep_zeile(out_grid[by], breite, hoehe, sweep_l[i],
			            &sweep_aufrufe[i], &sweep_byte[i]);
	}
}

extern "C" void zod_dirty_sweep_report(unsigned long frames)
{
	if(!sweep_an || !frames) return;

	ZLOG("Luecken-Kurve (Modell V1200: 1,48 us je Aufruf, 7,34 ns je Byte):\n");

	for(int i = 0; i < SWEEP_N; i++)
	{
		const unsigned long a = sweep_aufrufe[i] / frames;
		const unsigned long b = sweep_byte[i] / frames;
		/* us*100, damit ohne Fliesskomma gerechnet wird. */
		const unsigned long us = (a * 148 + b * 734 / 1000) / 100;

		ZLOG("  Luecke %3ld: %5ld Aufrufe, %6ld Byte -> Modell %4ld us\n",
		     (long)(sweep_l[i] == 999 ? -1 : sweep_l[i]),
		     (long)a, (long)b, (long)us);
	}
}

/* Hat diese Blockzeile ueberhaupt Schmutz?
 *
 * Fuer die AUSGABE ist das die einzige Frage, die sich lohnt -- nicht wo genau.
 * Gemessen auf der V1200 (19.09.): ein Kopieraufruf an einer VERSTREUTEN
 * Adresse kostet 1,45 us, einer im zusammenhaengenden Strom 0,35. Eine Luecke
 * von 64 Punkten zu ueberspringen spart 64 Byte = 0,39 us und kostet einen
 * zusaetzlichen verstreuten Aufruf = 1,45 us. Schmale Laeufe sind also ein
 * Verlustgeschaeft: 109 656 Byte in 963 Stuecken brauchten 2057 us, das ganze
 * Bild in 30 zusammenhaengenden Stuecken nur 1863.
 *
 * Deshalb kopiert die Ausgabe ganze Blockzeilen in voller Breite und fasst
 * senkrecht benachbarte zusammen. Was bleibt, ist die einzige Ersparnis, die
 * sich auf dieser Hardware bezahlt: Blockzeilen, die gar nichts enthalten,
 * werden ausgelassen -- und das sind bei stehender Karte die unteren mit der
 * Anzeige.
 *
 * Die Laufaufteilung (`zod_dirty_out_runs`) bleibt fuer den Kartenhintergrund
 * in Gebrauch. Dort ist das Ziel gewoehnlicher Speicher, kein Grafikspeicher,
 * und die Rechnung geht anders auf (dort gemessen: -1449 us). */
extern "C" int zod_dirty_out_zeile(int by)
{
	if(by < 0 || by >= grid_h) return 0;

	for(int bx = 0; bx < grid_w; bx++)
		if(out_grid[by][bx]) return 1;

	return 0;
}

extern "C" int zod_dirty_out_runs(int by, short *x0, short *x1, int max)
{
	return grid_runs(out_grid, by, x0, x1, max);
}

extern "C" int zod_dirty_prev_runs(int by, short *x0, short *x1, int max)
{
	if(!prev_gueltig) return 0;

	return grid_runs(prev_grid, by, x0, x1, max);
}

extern "C" int zod_dirty_block_shift(void) { return ZOD_BLOCK_SHIFT; }
extern "C" int zod_dirty_grid_h(void)      { return grid_h; }

extern "C" void zod_dirty_clear(void)
{
	/* Ausgeschaltet: alles bleibt schmutzig, es wird weiter voll kopiert. */
	if(!dirty_an) return;

	/* Gesichert wird die OBJEKTspur -- sie allein sagt, was im naechsten
	 * Bild zuzudecken ist. GETAUSCHT, nicht kopiert: die Vorspur des
	 * naechsten Bildes ist genau die Objektspur dieses Bildes. */
	{
		unsigned char (*tausch)[ZOD_GRID_W] = prev_grid;

		prev_grid = obj_grid;
		obj_grid  = tausch;
	}

	grid_alles(obj_grid, 0);
	grid_alles(out_grid, 0);

	rechteck_tauschen();
	fest_tauschen();

	prev_gueltig = 1;
}

extern "C" int zod_dirty_partial_ok(void)
{
	return dirty_an && prev_gueltig;
}

/* Beim Scrollen und beim Kartenwechsel stimmt nichts mehr: alles neu. */
extern "C" void zod_dirty_force_full(void)
{
	prev_gueltig = 0;
	grid_alles(out_grid, 1);
	grid_alles(obj_grid, 1);

	/* Die Objektspur gilt jetzt als "alles" -- die genaue Rechteckliste
	 * dieses Bildes waere zu klein (z. B. neu eingebackene Zonenmarker in
	 * der Kartenflaeche). Das naechste Scrollbild nimmt deshalb den
	 * Blockweg, und der vermerkt alles. Gefunden mit ZOD_VERIFY. */
	rechteck_jetzt_voll = 1;
}

extern "C" void zod_dirty_report(unsigned long long *bytes, unsigned long *frames)
{
	*bytes = zod_dirty_bytes;
	*frames = zod_dirty_frames;
}

extern "C" unsigned long zod_dirty_calls_get(void) { return zod_dirty_calls; }

/* Warp F3: Schluesselblit je Langwort. ZOD_BLITKEY 0 = alte Byteschleife,
 * ZOD_BLITCHECK 1 = beide rechnen und vergleichen (Zaehler im Bericht). */
static int zod_blitkey_modus = -1;
unsigned long zod_blitkey_lang = 0, zod_blitkey_geprueft = 0, zod_blitkey_falsch = 0;

static int zod_blitkey_an(void)
{
	if(zod_blitkey_modus < 0)
	{
		char wahl[16];

		zod_blitkey_modus = 1;
		if(zod_env("ZOD_BLITKEY", wahl, sizeof(wahl)) && wahl[0] == '0') zod_blitkey_modus = 0;
		if(zod_env("ZOD_BLITCHECK", wahl, sizeof(wahl)) && wahl[0] != '0') zod_blitkey_modus = 2;
		ZLOG("Schluesselblit: %s\n", zod_blitkey_modus == 0 ? "Byteschleife" :
		     zod_blitkey_modus == 1 ? "je Langwort" : "je Langwort, mit Pruefung");
	}

	return zod_blitkey_modus;
}

extern "C" void zod_blitkey_report(void)
{
	if(zod_blitkey_modus <= 0) return;

	ZLOG("Schluesselblit je Langwort: %lu Aufrufe", zod_blitkey_lang);
	if(zod_blitkey_modus == 2)
		ZLOG(", %lu geprueft, %lu abweichend (muss 0 sein)", zod_blitkey_geprueft, zod_blitkey_falsch);
	ZLOG("\n");
}

unsigned long zod_blit_key_calls   = 0;
unsigned long zod_blit_rows        = 0;
unsigned long long zod_blit_key_pixels  = 0;
unsigned long zod_blit_flat_calls  = 0;
unsigned long long zod_blit_flat_pixels = 0;

/* Laufende Summen fuer eine Klammer um einen einzelnen Abschnitt (zplayer.cpp
 * klammert damit den Effektabschnitt ein). */
extern "C" void zod_blit_now(unsigned long *calls, unsigned long *px,
                             unsigned long *rows)
{
	*calls = zod_blit_key_calls + zod_blit_flat_calls;
	/* Absichtlich auf 32 Bit abgeschnitten: die Aufrufer bilden DIFFERENZEN
	 * um einen Abschnitt, und die bleiben ueber den Ueberlauf hinweg richtig
	 * (dieselbe Ueberlegung wie in fineclock.h). */
	*px    = (unsigned long)(zod_blit_key_pixels + zod_blit_flat_pixels);
	*rows  = zod_blit_rows;
}

extern "C" void zod_blit_report(unsigned long *kc, unsigned long long *kp,
                                unsigned long *fc, unsigned long long *fp,
                                unsigned long *rows)
{
	*kc = zod_blit_key_calls;  *kp = zod_blit_key_pixels;
	*fc = zod_blit_flat_calls; *fp = zod_blit_flat_pixels;
	*rows = zod_blit_rows;
}

extern "C" {

/* ------------------------------------------------------------ Grundlagen */

static char sdl_error[256] = "";

static void set_error(const char *msg)
{
	strncpy(sdl_error, msg ? msg : "", sizeof(sdl_error) - 1);
	sdl_error[sizeof(sdl_error) - 1] = 0;
}

const char *SDL_GetError(void)
{
	return sdl_error;
}

int SDL_Init(Uint32 flags)
{
	(void)flags;
	return 0;
}

int SDL_InitSubSystem(Uint32 flags)
{
	(void)flags;
	return 0;
}

/* --------------------------------------------------------------- Palette */

/* EINE Palette fuer alle 8-Bit-Flaechen. Genau das spart gegenueber SDL die
 * 1024 Byte je Flaeche -- bei 6920 Bildern rund 7 MB. */
static SDL_Color shared_colors[256];
static SDL_Palette shared_palette = { 256, shared_colors };
static int shared_palette_set = 0;

/* Umrechnung 8 Bit -> Schirmformat; wird beim Oeffnen des Schirms gefuellt. */
static Uint16 pal_to16[256];
static int pal_to16_valid = 0;

/* Zwischenspeicher fuer SDL_MapRGB.
 *
 * Im 8-Bit-Pfad ist SDL_MapRGB eine LINEARE SUCHE: erst 256 Plaetze auf einen
 * genauen Treffer, dann 255 Plaetze auf den naechsten Nachbarn mit je drei
 * Multiplikationen. Und ZSDL_FillRect ruft es bei JEDER Fuellung
 * (zsdl_opengl.cpp: "SDL_FillRect(screen, dstrect, SDL_MapRGB(...))").
 *
 * Die Farben sind dabei Konstanten: ZObject::RenderHealth faerbt drei Balken
 * je Einheit (82,190,33 / 247,203,107 / Rahmen), und keine dieser Farben steht
 * genau in der Palette -- es laeuft also jedes Mal der volle Durchgang ueber
 * 511 Plaetze, dreimal je Einheit und Bild.
 *
 * Direkt abgebildeter Zwischenspeicher: ein Vergleich statt 511 Durchlaeufen.
 * Wird mit der Palette entwertet, sonst zeigte ein Planetenwechsel alte
 * Farben. */
enum { MAPRGB_CACHE = 64 };
static Uint32 maprgb_key[MAPRGB_CACHE];
static Uint8  maprgb_idx[MAPRGB_CACHE];
static int    maprgb_valid = 0;

static inline int maprgb_platz(Uint32 schluessel)
{
	/* Streuung ueber alle drei Kanaele; die benutzten Farben liegen sonst
	 * dicht beieinander und wuerden sich denselben Platz teilen. */
	return (int)(((schluessel >> 16) * 7u + (schluessel >> 8) * 13u + schluessel) & (MAPRGB_CACHE - 1));
}

void zod_sdl_set_shared_palette(const SDL_Color *colors)
{
	if(!colors) return;

	memcpy(shared_colors, colors, sizeof(shared_colors));
	shared_palette_set = 1;
	pal_to16_valid = 0;
	maprgb_valid = 0;
}

const SDL_Color *zod_sdl_shared_palette(void)
{
	return shared_colors;
}

void zod_sdl_build_16bit_table(int r_bits, int g_bits, int b_bits, int swapped)
{
	(void)r_bits; (void)g_bits; (void)b_bits;

	for(int i = 0; i < 256; i++)
	{
		Uint16 v = (Uint16)(((shared_colors[i].r & 0xF8) << 8) |
		                    ((shared_colors[i].g & 0xFC) << 3) |
		                     (shared_colors[i].b >> 3));

		if(swapped) v = (Uint16)((v >> 8) | (v << 8));

		pal_to16[i] = v;
	}

	pal_to16_valid = 1;
}

const Uint16 *zod_sdl_table16(void)
{
	return pal_to16_valid ? pal_to16 : 0;
}

/* ------------------------------------------------------- Flaechen anlegen */

struct SurfaceExtra
{
	SDL_PixelFormat format;
	SDL_Palette own_palette;  /* nur benutzt, wenn die Flaeche eine eigene Palette hat */
	SDL_Color *own_colors;
	int own_pixels;           /* 1 = wir haben die Pixel belegt */
	int big;                  /* 1 = ueber zod_big_alloc (AllocVec) belegt */
};

static SurfaceExtra *extra_of(SDL_Surface *s)
{
	/* Der Zusatz liegt unmittelbar hinter der Flaeche (eine Belegung). */
	return (SurfaceExtra*)((unsigned char*)s + sizeof(SDL_Surface));
}

/* Flaechen ab dieser Groesse am malloc vorbei belegen: libnix gibt Bereiche
 * erst zurueck, wenn sie ganz leer sind. */
#define ZOD_BIG_LIMIT (64UL * 1024UL)


static void fill_format(SDL_PixelFormat *f, int depth,
                        Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
	memset(f, 0, sizeof(*f));

	f->BitsPerPixel = (Uint8)depth;
	f->BytesPerPixel = (Uint8)((depth + 7) / 8);
	f->alpha = 255;
	f->colorkey = 0;

	if(depth == 8)
	{
		f->palette = &shared_palette;
		return;
	}

	f->Rmask = rmask ? rmask : 0xF800;
	f->Gmask = gmask ? gmask : 0x07E0;
	f->Bmask = bmask ? bmask : 0x001F;
	f->Amask = amask;

	if(depth == 16)
	{
		f->Rshift = 11; f->Gshift = 5; f->Bshift = 0;
		f->Rloss = 3; f->Gloss = 2; f->Bloss = 3; f->Aloss = 8;
	}
	else
	{
		f->Rshift = 16; f->Gshift = 8; f->Bshift = 0; f->Ashift = 24;
		f->Rloss = f->Gloss = f->Bloss = 0;
		f->Aloss = amask ? 0 : 8;
	}
}

/* ------------------------------------------------- POOL FUER DREHFLAECHEN
 *
 * Jede gerechnete Drehflaeche kostete ZWEI Belegungen und ZWEI Freigaben:
 * die Pixel ueber `zod_big_alloc` (AllocVec), den KOPF ueber libnix' calloc.
 * Auf der V1200 sind 72 us je Freigabe gemessen; bei 84 gerechneten Flaechen
 * in einem Explosionsbild sind das rund 6 ms nur fuers Freigeben, plus die
 * ungemessene Belegungsseite. CLAUDE.md benennt die Luecke selbst: "der
 * Flaechen-KOPF geht weiterhin durch libnix' calloc/free -- das erklaert,
 * warum die Freigabe von 131 auf 72 us fiel statt auf fast null."
 *
 * Also eine Freiliste. ZWEI ENTSCHEIDUNGEN, beide aus den Fallen dieses
 * Projekts:
 *
 *   1. NUR KOEPFE, nicht die Pixel. Die Pixelbloecke sind verschieden gross
 *      (ein Trummerteil bei Groesse 1,5 ist 576 Byte, bei 6 sind es 9216);
 *      eine groessengeklassete Freiliste haelt Speicher fest, und genau
 *      daran ist Karte 4 gescheitert ("Schlieren ab der 3./4. Karte", libnix
 *      gibt Speicher nie ans System zurueck). Der Kopf dagegen hat IMMER
 *      dieselbe Groesse -- eine einfach verkettete Liste, O(1), ohne jede
 *      Groessenfrage.
 *   2. GEDECKELT. Mehr als ZOD_KOPF_POOL Koepfe werden nicht gehalten;
 *      bei 184 Byte je Kopf sind 256 Stueck rund 47 KB.
 *
 * Geleert wird er in `zod_kopf_pool_leeren()`, das der Kartenwechsel ruft. */
#define ZOD_KOPF_POOL 256

static void *kopf_frei = 0;      /* einfach verkettet ueber das erste Langwort */
static int   kopf_frei_n = 0;
unsigned long zod_kopf_aus_pool = 0, zod_kopf_neu = 0, zod_kopf_zurueck = 0;

extern "C" void zod_kopf_pool_leeren(void)
{
	while(kopf_frei)
	{
		void *n = *(void**)kopf_frei;

		free(kopf_frei);
		kopf_frei = n;
	}

	kopf_frei_n = 0;
}

static SDL_Surface *alloc_surface(int w, int h, int depth)
{
	unsigned char *block;

	if(kopf_frei)
	{
		block = (unsigned char*)kopf_frei;
		kopf_frei = *(void**)kopf_frei;
		kopf_frei_n--;
		zod_kopf_aus_pool++;

		/* calloc hat genullt, also muss der Wiederverwender das auch. */
		memset(block, 0, sizeof(SDL_Surface) + sizeof(SurfaceExtra));
	}
	else
	{
		block = (unsigned char*)calloc(1, sizeof(SDL_Surface) + sizeof(SurfaceExtra));
		zod_kopf_neu++;
	}

	if(!block) return 0;

	SDL_Surface *s = (SDL_Surface*)block;
	SurfaceExtra *x = extra_of(s);

	s->format = &x->format;
	s->w = w;
	s->h = h;
	s->refcount = 1;
	s->clip_rect.x = 0;
	s->clip_rect.y = 0;
	s->clip_rect.w = (Uint16)w;
	s->clip_rect.h = (Uint16)h;
	(void)depth;

	return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
	if(w <= 0 || h <= 0)
	{
		set_error("SDL_CreateRGBSurface: Groesse <= 0");
		return 0;
	}

	/* NIEMALS eine andere Farbtiefe liefern als verlangt.
	 *
	 * Hier stand einmal "32-Bit-Anforderungen werden als 8 Bit angelegt" --
	 * gut gemeint (der Pfad soll ja 8 Bit sein), aber verheerend: SDL_rotozoom
	 * fordert ausdruecklich 32 Bit an, merkt sich is32bit = 1 und schreibt
	 * danach VIER Byte je Pixel in einen Ein-Byte-Puffer. Das viertelt keine
	 * Farbe, es ueberschreibt den vierfachen Bereich -- auf AmigaOS die
	 * Speicherliste, gemeldet als Guru 81000005 (AN_MemCorrupt) mitten im
	 * Laden der Grafiken.
	 *
	 * Belegt mit tests/shim unter AddressSanitizer:
	 *   ROTO src 2x16 pitch=4 bpp=1 | dst 1x4 pitch=4 bpp=1
	 *   READ of size 4 ... 0 bytes after 64-byte region
	 *
	 * Dass der Pfad 8 Bit bleibt, wird nicht hier erzwungen, sondern dort,
	 * wo es hingehoert: Die Archive liefern 8-Bit-Bilder (Format 4), und
	 * damit nimmt SDL_rotozoom von sich aus seinen 8-Bit-Zweig. */

	/* Eine 32-Bit-Flaeche ist im 8-Bit-Pfad fast immer ein Fehler.
	 *
	 * Sie laesst sich nicht auf den Schirm blitten, und der Aufrufer merkt
	 * davon nichts. Genau so sind am 18.09. Schrift, Krater und die
	 * Auswahlmarkierung verschwunden. SDL_rotozoom braucht 32 Bit intern
	 * weiterhin -- deshalb eine Meldung und kein Verbot. */
	if(depth == 32)
	{
		static int gemeldet = 0;

		if(gemeldet < 8)
		{
			gemeldet++;
			ZLOG("Achtung: 32-Bit-Flaeche %ldx%ld angelegt -- auf den 8-Bit-Schirm "
			     "laesst sie sich nicht zeichnen\n", (long)w, (long)h);
		}
	}

	SDL_Surface *s = alloc_surface(w, h, depth);

	if(!s) { set_error("SDL_CreateRGBSurface: kein Speicher"); return 0; }

	SurfaceExtra *x = extra_of(s);

	fill_format(s->format, depth, rmask, gmask, bmask, amask);

	const int bpp = s->format->BytesPerPixel;

	/* Zeilenlaenge auf vier Byte aufrunden, wie SDL es tut: fremder Code
	 * (SDL_rotozoom) rechnet mit dieser Ausrichtung. */
	const int pitch = ((w * bpp) + 3) & ~3;
	const unsigned long bytes = (unsigned long)pitch * h;

	s->pitch = (Uint16)pitch;
	s->flags = flags & (SDL_SRCALPHA | SDL_SRCCOLORKEY);

	if(bytes >= ZOD_BIG_LIMIT || zod_big_hint_tiefe > 0)
	{
		s->pixels = zod_big_alloc(bytes);
		x->big = 1;
	}
	else
	{
		s->pixels = calloc(1, bytes);
		x->big = 0;
	}

	if(!s->pixels)
	{
		free(s);
		set_error("SDL_CreateRGBSurface: kein Speicher fuer Pixel");
		return 0;
	}

	if(x->big) memset(s->pixels, 0, bytes);

	x->own_pixels = 1;

	return s;
}

/* Fensterschub: die Zeichenflaeche in einen Block mit Rand legen (nur 8 Bit,
 * Begruendung bei zod_fenster_schub). Die Mitte ist auf 16 Byte ausgerichtet,
 * damit die Bildausgabe in der Ruhe MOVE16 nehmen kann. */
static void fen_einrichten(SDL_Surface *s)
{
	if(!s || fen_flaeche || s->format->BytesPerPixel != 1) return;
	if(fen_modus_lesen() == 0) return;

	const unsigned long bild = (unsigned long)s->pitch * (unsigned long)s->h;
	/* Rand je Seite: zwei Bildhoehen -- bei ~13 Zeilen je Bild in eine
	 * Richtung rund 70 Bilder bis zur naechsten Mitte. */
	const unsigned long rand = 2UL * bild;
	const unsigned long gesamt = bild + 2UL * rand + 32UL;
	unsigned char *b = (unsigned char*)zod_big_alloc(gesamt);

	if(!b)
	{
		ZLOG("Fensterschub: kein Speicher fuer %lu KB -- aus\n", gesamt / 1024);
		fen_modus = 0;
		return;
	}

	memset(b, 0, gesamt);

	fen_block = b;
	fen_lo = (unsigned char*)(((unsigned long)b + 15UL) & ~15UL);
	fen_mitte = fen_lo + rand;
	fen_hi = fen_lo + bild + 2UL * rand;

	SurfaceExtra *x = extra_of(s);

	memcpy(fen_mitte, s->pixels, bild);

	if(x->own_pixels && s->pixels)
	{
		if(x->big) zod_big_free(s->pixels);
		else free(s->pixels);
	}

	x->own_pixels = 0;
	s->pixels = fen_mitte;
	fen_flaeche = s;

	ZLOG("Fensterschub: Zeichenflaeche in %lu KB (Rand je Seite %lu KB)\n",
	     gesamt / 1024, rand / 1024);
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
                                      Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
	SDL_Surface *s = alloc_surface(w, h, depth);

	if(!s) return 0;

	fill_format(s->format, depth, rmask, gmask, bmask, amask);

	s->pixels = pixels;
	s->pitch = (Uint16)pitch;
	s->flags = SDL_PREALLOC;

	extra_of(s)->own_pixels = 0;

	return s;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
	if(!surface) return;

	if(--surface->refcount > 0) return;

	SurfaceExtra *x = extra_of(surface);

	if(surface == fen_flaeche)
	{
		/* Pixel liegen im Fensterblock (fen_einrichten) */
		zod_big_free(fen_block);
		fen_block = fen_lo = fen_hi = fen_mitte = 0;
		fen_flaeche = 0;
		surface->pixels = 0;
	}

	if(x->own_pixels && surface->pixels)
	{
		if(x->big) zod_big_free(surface->pixels);
		else free(surface->pixels);
	}

	if(x->own_colors) free(x->own_colors);

	/* Den Kopf in die Freiliste statt an libnix zurueck -- siehe oben. */
	if(kopf_frei_n < ZOD_KOPF_POOL)
	{
		*(void**)surface = kopf_frei;
		kopf_frei = surface;
		kopf_frei_n++;
		zod_kopf_zurueck++;
		return;
	}

	free(surface);
}

int SDL_LockSurface(SDL_Surface *surface)
{
	(void)surface;
	return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
	(void)surface;
}

/* -------------------------------------------------------- Farben, Palette */

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors)
{
	if(!surface || !surface->format->palette || !colors) return 0;

	/* Stimmt die Palette mit der gemeinsamen ueberein, bleibt es bei der
	 * gemeinsamen -- das ist der Normalfall (Archivformat 4). */
	if(surface->format->palette == &shared_palette)
	{
		if(firstcolor == 0 && ncolors == 256 &&
		   !memcmp(colors, shared_colors, sizeof(shared_colors)))
			return 1;

		if(!shared_palette_set)
		{
			memcpy(shared_colors + firstcolor, colors, ncolors * sizeof(SDL_Color));
			shared_palette_set = 1;
			pal_to16_valid = 0;
			return 1;
		}

		/* Sonderfall: eigene Palette nur fuer diese Flaeche */
		/* Eigene Palette fuer genau diese Flaeche. Der Kopf liegt im
		 * Zusatzblock der Flaeche, damit beim Freigeben nichts uebrig
		 * bleibt und nichts ins Leere zeigt. */
		SurfaceExtra *x = extra_of(surface);

		x->own_colors = (SDL_Color*)malloc(256 * sizeof(SDL_Color));

		if(!x->own_colors) return 0;

		memcpy(x->own_colors, shared_colors, 256 * sizeof(SDL_Color));

		x->own_palette.ncolors = 256;
		x->own_palette.colors = x->own_colors;
		surface->format->palette = &x->own_palette;
	}

	SDL_Palette *p = surface->format->palette;

	if(firstcolor < 0 || firstcolor + ncolors > p->ncolors) return 0;

	memcpy(p->colors + firstcolor, colors, ncolors * sizeof(SDL_Color));

	return 1;
}

int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors,
                   int firstcolor, int ncolors)
{
	(void)flags;
	return SDL_SetColors(surface, colors, firstcolor, ncolors);
}


/* Ein Bit eines Parameters pruefen, ohne dass gcc daraus ein "btst" auf den
 * Speicher macht.
 *
 * Hintergrund, belegt am 18.09. durch Maschinencode UND Laufzeitmessung:
 * gcc 6.5.0b erzeugt fuer "flag & 0x1000" bei einem 32-Bit-PARAMETER
 *
 *     btst #-12,12(a5)
 *
 * "btst" auf einen Speicheroperanden prueft aber immer nur EIN Byte, und die
 * Bitnummer gilt modulo 8. Fuer Bit 12 muesste die Adresse um zwei Byte
 * weiterruecken (btst #4,14(a5)); gcc laesst sie stehen und prueft damit das
 * hoechstwertige Byte -- das ist bei 0x1000 immer 0. Die Bedingung war also
 * IMMER falsch. Beim Schreiben macht gcc es richtig (ori.w #4096,2(a0)),
 * nur beim Pruefen nicht.
 *
 * Gemessen auf dem Amiga: "SetColorKey: 0 gesetzt, 4547 geloescht" -- kein
 * einziges Bild bekam seinen Farbschluessel, und in 719009 Blits wurde Platz 0
 * mitgezeichnet. Auf dem Schirm: ein schwarzer Kasten um jedes Objekt.
 *
 * Reichweite (Binary abgesucht): -m68040 trifft es zweimal (SDL_SetColorKey
 * und SDL_SetAlpha), -m68060/68080 einmal (SDL_SetAlpha). -m68020/68030 sind
 * frei. "btst #N,dX" auf ein REGISTER ist harmlos -- dort gilt modulo 32.
 *
 * Das volatile ist der Punkt: Es zwingt den Wert durch den Stapel und damit in
 * ein Register, und der fehlerhafte Weg entfaellt. Ohne volatile hilft weder
 * eine lokale Kopie noch ein ausdruecklicher Vergleich noch eine eigene
 * Funktion -- alle vier Schreibweisen wurden geprueft und erzeugen denselben
 * kaputten Befehl. */
static int hat_flagge(Uint32 wert, Uint32 maske)
{
	volatile Uint32 v = wert;

	return (v & maske) != 0;
}

int SDL_SetColorKey(SDL_Surface *surface, Uint32 flag, Uint32 key)
{
	if(!surface) return -1;

	if(hat_flagge(flag, SDL_SRCCOLORKEY))
	{
		surface->flags |= SDL_SRCCOLORKEY;
		surface->format->colorkey = key;
	}
	else
		surface->flags &= ~SDL_SRCCOLORKEY;

	return 0;
}

int SDL_SetAlpha(SDL_Surface *surface, Uint32 flag, Uint8 alpha)
{
	if(!surface) return -1;

	if(hat_flagge(flag, SDL_SRCALPHA) && alpha != 255)
	{
		surface->flags |= SDL_SRCALPHA;
		surface->format->alpha = alpha;
	}
	else
	{
		surface->flags &= ~SDL_SRCALPHA;
		surface->format->alpha = 255;
	}

	return 0;
}

int SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect)
{
	if(!surface) return 0;

	if(!rect)
	{
		surface->clip_rect.x = 0;
		surface->clip_rect.y = 0;
		surface->clip_rect.w = (Uint16)surface->w;
		surface->clip_rect.h = (Uint16)surface->h;
		return 1;
	}

	surface->clip_rect = *rect;

	return 1;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b)
{
	if(!fmt) return 0;

	if(fmt->palette)
	{
		const SDL_Color *c = fmt->palette->colors;

		/* Zwischenspeicher nur fuer die GEMEINSAME Palette: Flaechen mit
		 * eigener Palette (zod_sdl_set_surface_palette) haben andere
		 * Zuordnungen, und der Schluessel aus r/g/b allein wuerde sie
		 * verwechseln. */
		const int gemeinsam = (c == shared_colors);
		const Uint32 schluessel = ((Uint32)r << 16) | ((Uint32)g << 8) | b;
		int platz = 0;

		if(gemeinsam)
		{
			if(!maprgb_valid)
			{
				for(int i = 0; i < MAPRGB_CACHE; i++) maprgb_key[i] = 0xFFFFFFFFu;

				maprgb_valid = 1;
			}

			platz = maprgb_platz(schluessel);

			if(maprgb_key[platz] == schluessel) return (Uint32)maprgb_idx[platz];
		}

		/* Genaue Treffer zaehlen zuerst -- Platz 0 EINGESCHLOSSEN.
		 *
		 * Hier stand einmal nur "naechster Platz, Platz 0 bleibt der
		 * Schluessel". Gut gemeint (ein echtes Schwarz im Bild soll nicht
		 * versehentlich durchsichtig werden), aber es macht die Umkehrung
		 * unmoeglich: SDL_rotozoom holt sich die Schluesselfarbe mit
		 * SDL_GetRGB(0), faerbt damit die Zielflaeche und erwartet, ueber
		 * SDL_MapRGB wieder die 0 zu bekommen. Ohne genauen Treffer kam der
		 * naechstdunkelste Platz zurueck -- der Rand gedrehter Bilder war
		 * danach schwarz statt durchsichtig (vom Nutzer auf der V2 gesehen).
		 *
		 * Ungefaehre Treffer meiden Platz 0 weiterhin: Dort landen nur
		 * Farben, die es in der Palette gar nicht gibt, und die sollen
		 * sichtbar bleiben. Der Packer vergibt die 0 ohnehin ausschliesslich
		 * an durchsichtige Pixel. */
		for(int i = 0; i < fmt->palette->ncolors; i++)
			if(c[i].r == r && c[i].g == g && c[i].b == b)
			{
				if(gemeinsam) { maprgb_key[platz] = schluessel; maprgb_idx[platz] = (Uint8)i; }

				return (Uint32)i;
			}

		int best = 1, best_d = 1 << 30;

		for(int i = 1; i < fmt->palette->ncolors; i++)
		{
			int dr = (int)c[i].r - r, dg = (int)c[i].g - g, db = (int)c[i].b - b;
			int d = dr * dr + dg * dg + db * db;

			if(d < best_d) { best = i; best_d = d; }
		}

		if(gemeinsam) { maprgb_key[platz] = schluessel; maprgb_idx[platz] = (Uint8)best; }

		return (Uint32)best;
	}

	return ((Uint32)(r >> fmt->Rloss) << fmt->Rshift) |
	       ((Uint32)(g >> fmt->Gloss) << fmt->Gshift) |
	       ((Uint32)(b >> fmt->Bloss) << fmt->Bshift);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	Uint32 v = SDL_MapRGB(fmt, r, g, b);

	if(fmt && fmt->Amask)
		v |= ((Uint32)(a >> fmt->Aloss) << fmt->Ashift) & fmt->Amask;

	return v;
}

void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *fmt, Uint8 *r, Uint8 *g, Uint8 *b)
{
	if(!fmt) return;

	if(fmt->palette)
	{
		unsigned i = pixel < (unsigned)fmt->palette->ncolors ? pixel : 0;

		if(r) *r = fmt->palette->colors[i].r;
		if(g) *g = fmt->palette->colors[i].g;
		if(b) *b = fmt->palette->colors[i].b;
		return;
	}

	if(r) *r = (Uint8)(((pixel & fmt->Rmask) >> fmt->Rshift) << fmt->Rloss);
	if(g) *g = (Uint8)(((pixel & fmt->Gmask) >> fmt->Gshift) << fmt->Gloss);
	if(b) *b = (Uint8)(((pixel & fmt->Bmask) >> fmt->Bshift) << fmt->Bloss);
}

void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat *fmt,
                 Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
	SDL_GetRGB(pixel, fmt, r, g, b);

	if(!a) return;

	if(fmt && fmt->palette)
	{
		/* Platz 0 ist durchsichtig, alles andere deckend */
		*a = pixel ? 255 : 0;
		return;
	}

	if(fmt && fmt->Amask)
		*a = (Uint8)(((pixel & fmt->Amask) >> fmt->Ashift) << fmt->Aloss);
	else
		*a = 255;
}

/* ------------------------------------------------------------- Zeichnen */

static int clip_blit(SDL_Surface *src, SDL_Rect *srcrect,
                     SDL_Surface *dst, SDL_Rect *dstrect,
                     int *sx, int *sy, int *dx, int *dy, int *w, int *h)
{
	int s_x = srcrect ? srcrect->x : 0;
	int s_y = srcrect ? srcrect->y : 0;
	int d_x = dstrect ? dstrect->x : 0;
	int d_y = dstrect ? dstrect->y : 0;
	int b_w, b_h;

	/* Ohne srcrect gilt die ganze Quellflaeche. MIT srcrect gilt, was
	 * drinsteht -- auch die Null.
	 *
	 * Hier stand frueher "srcrect->w ? srcrect->w : src->w", also Breite 0
	 * als "ganze Breite" gedeutet. Echtes SDL zeichnet bei Breite 0 NICHTS,
	 * und die Engine verlaesst sich darauf: ZSDL_Surface::GetMapBlitInfo
	 * rechnet fuer ein Sprite am rechten Kartenrand
	 * from_rect.w = view_w - to_rect.x -- und das ist genau 0, sobald die
	 * Einheit die Grenze zur Anzeige erreicht. Statt nichts zu zeichnen,
	 * malte der Blitter dann das VOLLE Sprite an diese Stelle, also
	 * vollstaendig in das HUD hinein. Vom Nutzer auf der V1200 gesehen:
	 * "Objekte zeichnen manchmal am rechten Rand in die HUD hinein". */
	if(srcrect)
	{
		b_w = srcrect->w;
		b_h = srcrect->h;
	}
	else
	{
		b_w = src->w;
		b_h = src->h;
	}

	/* Quelle beschneiden */
	if(s_x < 0) { b_w += s_x; d_x -= s_x; s_x = 0; }
	if(s_y < 0) { b_h += s_y; d_y -= s_y; s_y = 0; }
	if(s_x + b_w > src->w) b_w = src->w - s_x;
	if(s_y + b_h > src->h) b_h = src->h - s_y;

	/* Ziel beschneiden -- die Engine verlaesst sich darauf (HUD, Menues) */
	const int cx = dst->clip_rect.x;
	const int cy = dst->clip_rect.y;
	const int cw = dst->clip_rect.w;
	const int ch = dst->clip_rect.h;

	if(d_x < cx) { int d = cx - d_x; b_w -= d; s_x += d; d_x = cx; }
	if(d_y < cy) { int d = cy - d_y; b_h -= d; s_y += d; d_y = cy; }
	if(d_x + b_w > cx + cw) b_w = cx + cw - d_x;
	if(d_y + b_h > cy + ch) b_h = cy + ch - d_y;

	if(b_w <= 0 || b_h <= 0) return 0;

	*sx = s_x; *sy = s_y; *dx = d_x; *dy = d_y; *w = b_w; *h = b_h;

	return 1;
}

int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect)
{
	if(!src || !dst || !src->pixels || !dst->pixels) return -1;

	int sx, sy, dx, dy, w, h;

	if(!clip_blit(src, srcrect, dst, dstrect, &sx, &sy, &dx, &dy, &w, &h))
	{
		if(dstrect) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	dirty_mark(dst, dx, dy, w, h);

	/* SDL schreibt das Zielrechteck zurueck -- die Engine kennt das
	 * (zmini_map.cpp) und arbeitet mit einer Kopie. */
	if(dstrect)
	{
		dstrect->x = (Sint16)dx;
		dstrect->y = (Sint16)dy;
		dstrect->w = (Uint16)w;
		dstrect->h = (Uint16)h;
	}

	const int sbpp = src->format->BytesPerPixel;
	const int dbpp = dst->format->BytesPerPixel;
	const int key_on = (src->flags & SDL_SRCCOLORKEY) != 0;
	const Uint32 key = src->format->colorkey;

	/* Flaechenalpha als SCHWELLE statt als Mischung.
	 *
	 * Eine echte Mischung waere in 8 Bit teuer: Jeder Pixel braeuchte eine
	 * Suche in der Palette oder eine 64-KB-Mischtabelle. Der Bestand nutzt
	 * Flaechenalpha aber nur zum Ein- und Ausblenden.
	 *
	 * Der Anlass war das Startbild: `ZPlayer::DoSplash` blendete es ueber
	 * mehrere Sekunden aus, UND ZWAR UEBER das fertige HUD. Ohne jede
	 * Auswertung blieb es voll deckend, bis der Blendwert unter 5 fiel --
	 * auf der V2 als "das Ladebild scheint sekundenlang durch die Anzeige"
	 * gemeldet. Die Schwelle kostet einen Vergleich je Blit statt Arbeit je
	 * Pixel.
	 *
	 * DER ANLASS IST SEIT DEM 02.10. WEG (DoSplash entfallen), die Schwelle
	 * bleibt: `MakeAlphable`/`SetAlpha` hat der Statistikbildschirm-Code
	 * nicht, aber die Engine ruft es anderswo weiter (z. B. News-Text), und
	 * ein stillschweigend volldeckender Blit waere dort dasselbe Problem. */
	if((src->flags & SDL_SRCALPHA) && src->format->alpha < 128)
	{
		if(dstrect) { dstrect->w = 0; dstrect->h = 0; }

		return 0;
	}

	unsigned char *sp = (unsigned char*)src->pixels + (size_t)sy * src->pitch + (size_t)sx * sbpp;
	unsigned char *dp = (unsigned char*)dst->pixels + (size_t)dy * dst->pitch + (size_t)dx * dbpp;

	/* 8 -> 8: der Normalfall */
	if(sbpp == 1 && dbpp == 1)
	{
		/* Die ZEILENzahl ist die Groesse, an der der Aufrufaufwand haengt --
		 * die Bildpunktzahl sagt darueber nichts. Ohne sie waere "ein Aufruf
		 * je Zeile kostet X" wieder eine Hochrechnung aus einer Mengenangabe,
		 * und genau daran ist in diesem Projekt schon ein Kostenmodell
		 * gescheitert. */
		zod_blit_rows += (unsigned long)h;

		if(key_on) { zod_blit_key_calls++; zod_blit_key_pixels += (unsigned long)w * h; }
		else       { zod_blit_flat_calls++; zod_blit_flat_pixels += (unsigned long)w * h; }

#ifdef ZOD_AMMX
		/* Acht Ausrichtungen sind geprueft, die Achterbloecke laufen also
		 * auch bei ungerader Zeilenbreite und ungeradem Versatz. Unter acht
		 * Bildpunkten lohnt der Aufruf nicht -- dann bleibt es beim C-Weg. */
		if(key_on && w >= 8 && zod_ammx_available())
		{
			/* Schluessel 0 ist der Normalfall (die gemeinsame Palette
			 * vergibt Platz 0 nur an durchsichtige Punkte). Dafuer gibt
			 * es STOREM3 im Bytemodus: zwei Befehle je acht Bildpunkte
			 * statt drei, und ohne Schluesselregister. */
			if(key == 0)
			{
				zod_ammx_blit_rect_k0(dp, sp, (unsigned long)w,
				                      (unsigned long)h,
				                      (long)dst->pitch, (long)src->pitch);
			}
			else
			{
				/* DAS FELD WIRD NUR HIER GEBAUT, und das ist der Punkt.
				 *
				 * Es stand bis zum 02.10. VOR der `key == 0`-Abfrage. Alle
				 * Aufrufer dieses Blitters setzen aber Schluessel 0
				 * (zplayer_events.cpp:757, zplayer.cpp:1301, zteam.cpp:492
				 * erbt 0, SDL_rotozoom erbt den des Quellbildes) -- der
				 * k0-Weg wurde also IMMER genommen und `key8` war IMMER
				 * unbenutzt. 29 Befehle je Blit fuer nichts, rund 1100 je
				 * Bild.
				 *
				 * Einmal je Blit, nicht je Zeile: "load" mit Speicherquelle
				 * ist eindeutig 64 Bit, ein Aufbau aus einem Datenregister
				 * waere es nicht (die Referenz beschreibt nur die
				 * Wiederholung von Sofortwerten). */
				unsigned char key8[8];

				for(int i = 0; i < 8; i++) key8[i] = (unsigned char)key;

				zod_ammx_blit_rect(dp, sp, (unsigned long)w,
				                   (unsigned long)h,
				                   (long)dst->pitch, (long)src->pitch, key8);
			}

			return 0;
		}
#endif

		/* DER DECKENDE WEG: EIN Aufruf fuer das ganze Rechteck.
		 *
		 * Bis zum 02.10. stand hier ein `zod_copy_row` JE ZEILE -- ein
		 * indirekter Aufruf ueber eine Uebersetzungseinheitsgrenze, und der
		 * ist auf der V1200 mit 1140 ns gemessen, so viel wie 131 kopierte
		 * Byte. Bei 750 flachen Zeilen je Bild (640x480) bzw. rund 4800
		 * (1280x720) ist das der groesste Einzelposten der Fixkosten.
		 *
		 * NICHT memcpy -- das ist hier ein Aufruf von exec/CopyMem, und
		 * CopyMem faellt bei ungleicher Paritaet von Quelle und Ziel auf
		 * BYTEWEISES Kopieren zurueck. Genau das tritt bei ungeradem
		 * Scrollstand ein, also etwa in jedem zweiten Bild. */
		if(!key_on)
		{
			/* Die Zaehler stehen schon weiter oben -- hier NICHT noch
			 * einmal, sonst zaehlt jeder deckende Blit doppelt. */
			zod_rect_calls++;
			zod_rect_rows += (unsigned long)h;

			if(w == dst->pitch && w == src->pitch) zod_rect_fast++;

			zod_copy_rect(dp, sp, w, h, (int)dst->pitch, (int)src->pitch);

			return 0;
		}

		if(key == 0 && zod_blitkey_an())
		{
			zod_blitkey_lang++;

			if(zod_blitkey_modus == 2)
			{
				/* alte Schleife auf eine Kopie des Zielrechtecks, neue auf das
				 * Ziel, dann vergleichen */
				unsigned char *tmp = (unsigned char*)malloc((size_t)w * h);

				if(tmp)
				{
					for(int y = 0; y < h; y++) memcpy(tmp + (size_t)y * w, dp + (size_t)y * dst->pitch, w);

					zod_blit_key_bytes(tmp, sp, w, h, w, (int)src->pitch, 0);
					zod_blit_key0_long(dp, sp, w, h, (int)dst->pitch, (int)src->pitch);

					zod_blitkey_geprueft++;
					for(int y = 0; y < h; y++)
						if(memcmp(tmp + (size_t)y * w, dp + (size_t)y * dst->pitch, w))
						{
							if(zod_blitkey_falsch < 8)
								ZLOG("Schluesselblit: Abweichung w=%d h=%d Zeile %d\n", w, h, y);
							zod_blitkey_falsch++;
							break;
						}

					free(tmp);
					return 0;
				}
			}

			zod_blit_key0_long(dp, sp, w, h, (int)dst->pitch, (int)src->pitch);

			return 0;
		}

		zod_blit_key_bytes(dp, sp, w, h, (int)dst->pitch, (int)src->pitch, (unsigned char)key);

		return 0;
	}

	/* 8 -> 16: ueber die Umrechnungstabelle der gemeinsamen Palette */
	if(sbpp == 1 && dbpp == 2)
	{
		const Uint16 *tab = zod_sdl_table16();

		if(!tab) return -1;

		for(int y = 0; y < h; y++)
		{
			const unsigned char *s = sp;
			Uint16 *d = (Uint16*)dp;

			for(int x = 0; x < w; x++)
			{
				const unsigned char v = s[x];

				if(!key_on || v != (unsigned char)key) d[x] = tab[v];
			}

			sp += src->pitch;
			dp += dst->pitch;
		}

		return 0;
	}

	/* 16 -> 16 */
	if(sbpp == 2 && dbpp == 2)
	{
		for(int y = 0; y < h; y++)
		{
			const Uint16 *s = (const Uint16*)sp;
			Uint16 *d = (Uint16*)dp;

			if(!key_on)
				memcpy(d, s, (size_t)w * 2);
			else
				for(int x = 0; x < w; x++)
					if(s[x] != (Uint16)key) d[x] = s[x];

			sp += src->pitch;
			dp += dst->pitch;
		}

		return 0;
	}

	/* LAUT scheitern, nicht still.
	 *
	 * Diese Zeile hat am 18.09. dreimal Zeit gekostet: Ein nicht vorgesehenes
	 * Formatpaar kehrte einfach mit -1 zurueck, und der Aufrufer wertet den
	 * Rueckgabewert nicht aus. Auf dem Schirm fehlte dann einfach etwas --
	 * Gebirge, Schrift, Fenster -- ohne eine einzige Meldung. Genau diese
	 * Klasse von stillem Ausfall ist in diesem Port schon dreimal
	 * aufgetreten.
	 *
	 * Je Paarung nur einmal, damit es kein Bild je Bild wird. */
	{
		static unsigned char gemeldet[5][5] = { { 0 } };

		const int si = (sbpp >= 0 && sbpp < 5) ? sbpp : 0;
		const int di = (dbpp >= 0 && dbpp < 5) ? dbpp : 0;

		if(gemeldet[si][di] < 4)
		{
			gemeldet[si][di]++;

			ZLOG("Blit: %ld -> %ld Bit ist nicht vorgesehen -- hier wird NICHTS "
			     "gezeichnet (Quelle %ldx%ld, Ziel %ldx%ld, Schluessel %ld)\n",
			     (long)(sbpp * 8), (long)(dbpp * 8), (long)src->w, (long)src->h,
			     (long)dst->w, (long)dst->h, (long)key_on);
		}
	}

	set_error("Blit: Formatpaarung nicht vorgesehen");

	return -1;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)
{
	if(!dst || !dst->pixels) return -1;

	int x0 = dstrect ? dstrect->x : dst->clip_rect.x;
	int y0 = dstrect ? dstrect->y : dst->clip_rect.y;
	int w  = dstrect ? (int)dstrect->w : (int)dst->clip_rect.w;
	int h  = dstrect ? (int)dstrect->h : (int)dst->clip_rect.h;

	const int cx = dst->clip_rect.x;
	const int cy = dst->clip_rect.y;

	if(x0 < cx) { w -= cx - x0; x0 = cx; }
	if(y0 < cy) { h -= cy - y0; y0 = cy; }
	if(x0 + w > cx + dst->clip_rect.w) w = cx + dst->clip_rect.w - x0;
	if(y0 + h > cy + dst->clip_rect.h) h = cy + dst->clip_rect.h - y0;

	if(w <= 0 || h <= 0) return 0;

	dirty_mark(dst, x0, y0, w, h);

	const int bpp = dst->format->BytesPerPixel;
	unsigned char *dp = (unsigned char*)dst->pixels + (size_t)y0 * dst->pitch + (size_t)x0 * bpp;

	for(int y = 0; y < h; y++)
	{
		if(bpp == 1)
			memset(dp, (unsigned char)color, w);
		else
		{
			Uint16 *d = (Uint16*)dp;

			for(int x = 0; x < w; x++) d[x] = (Uint16)color;
		}

		dp += dst->pitch;
	}

	return 0;
}

/* ------------------------------------------------------------ Umwandlung */

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags)
{
	if(!src || !fmt) return 0;

	SDL_Surface *out = SDL_CreateRGBSurface(flags, src->w, src->h,
	                                        fmt->BitsPerPixel,
	                                        fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);

	if(!out) return 0;

	if(src->format->BytesPerPixel == out->format->BytesPerPixel)
	{
		for(int y = 0; y < src->h; y++)
			memcpy((unsigned char*)out->pixels + (size_t)y * out->pitch,
			       (unsigned char*)src->pixels + (size_t)y * src->pitch,
			       (size_t)src->w * src->format->BytesPerPixel);
	}
	else
	{
		SDL_Rect r;

		r.x = 0; r.y = 0; r.w = (Uint16)src->w; r.h = (Uint16)src->h;

		const Uint32 keep = src->flags;

		((SDL_Surface*)src)->flags &= ~SDL_SRCCOLORKEY;
		SDL_UpperBlit(src, &r, out, 0);
		((SDL_Surface*)src)->flags = keep;
	}

	if(src->flags & SDL_SRCCOLORKEY)
		SDL_SetColorKey(out, SDL_SRCCOLORKEY, src->format->colorkey);

	if(src->flags & SDL_SRCALPHA)
		SDL_SetAlpha(out, SDL_SRCALPHA, src->format->alpha);

	return out;
}

SDL_Surface *SDL_DisplayFormat(SDL_Surface *surface)
{
	/* Im 8-Bit-Pfad ist jede Flaeche bereits im Schirmformat. */
	if(!surface) return 0;

	return SDL_ConvertSurface(surface, surface->format, surface->flags);
}

SDL_Surface *SDL_DisplayFormatAlpha(SDL_Surface *surface)
{
	return SDL_DisplayFormat(surface);
}

} /* extern "C" */

/* --- Mikromessung der Blitwege (Warp F3, ZOD_BLITBENCH 1) ----------------
 *
 * Je Fall N Aufrufe in einer Schleife, EINE Uhrablesung davor und danach --
 * eine Ablesung kostet selbst um 10 us, so viel wie ein Kachelblit, und ist
 * deshalb als Fall A mitgemessen. Ziel ist die Zeichenflaeche (dirty_target),
 * wie im Spiel; die beschmierten Bloecke sind als schmutzig vermerkt und
 * werden im naechsten Bild neu gezeichnet. Einmal je Lauf. */
#include "fineclock.h"

static unsigned long zod_mb_us(unsigned long ticks, unsigned long n)
{
	const unsigned long f = zod_fineclock_freq();

	if(!f || !n) return 0;

	/* ns je Aufruf */
	return (unsigned long)(((double)ticks * 1.0e9 / (double)f) / (double)n);
}

extern "C" void zod_blit_microbench(void)
{
	SDL_Surface *ziel = dirty_target;

	if(!ziel || !ziel->pixels || ziel->format->BytesPerPixel != 1)
	{
		ZLOG("Blitmessung: keine 8-Bit-Zeichenflaeche\n");
		return;
	}

	const unsigned long N = 20000;
	SDL_Surface *k16  = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	SDL_Surface *k32  = SDL_CreateRGBSurface(0, 32, 32, 8, 0, 0, 0, 0);
	SDL_Surface *kk16 = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	SDL_Surface *neben = SDL_CreateRGBSurface(0, ziel->w, ziel->h, 8, 0, 0, 0, 0);

	if(!k16 || !k32 || !kk16 || !neben) { ZLOG("Blitmessung: kein Speicher\n"); return; }

	for(int i = 0; i < 16 * 16; i++) ((unsigned char*)k16->pixels)[i] = (unsigned char)(1 + (i % 200));
	for(int i = 0; i < 32 * 32; i++) ((unsigned char*)k32->pixels)[i] = (unsigned char)(1 + (i % 200));
	for(int i = 0; i < 16 * 16; i++) ((unsigned char*)kk16->pixels)[i] = (unsigned char)(((i * 7) % 3) ? 1 + (i % 200) : 0);
	SDL_SetColorKey(kk16, SDL_SRCCOLORKEY, 0);

	SDL_Rect r;
	unsigned long t0, t1;
	volatile unsigned long senke = 0;

#define ZOD_MB_POS(i) do { r.x = (Sint16)(64 + ((i) * 16) % 448); r.y = (Sint16)(48 + (((i) / 28) * 16) % 320); r.w = 0; r.h = 0; } while(0)

	/* A: Uhr */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) senke += zod_fineclock_ticks();
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung A ReadEClock: %lu ns je Aufruf\n", zod_mb_us(t1 - t0, N));

	/* B: leere Schleife mit Positionsrechnung */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); senke += r.x; }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung B leere Schleife: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* C: SDL_UpperBlit 16x16 deckend auf die Zeichenflaeche */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); SDL_UpperBlit(k16, 0, ziel, &r); }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung C UpperBlit 16x16 deckend, Zeichenflaeche: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* D: dasselbe auf eine Nebenflaeche (ohne Schmutzvermerk) */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); SDL_UpperBlit(k16, 0, neben, &r); }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung D UpperBlit 16x16 deckend, Nebenflaeche: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* E: nur die Kopie */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++)
	{
		ZOD_MB_POS(i);
		zod_copy_rect((unsigned char*)ziel->pixels + r.y * ziel->pitch + r.x,
		              (const unsigned char*)k16->pixels, 16, 16, (int)ziel->pitch, 16);
	}
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung E nur zod_copy_rect 16x16: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* F: nur der Schmutzvermerk */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); dirty_mark(ziel, r.x, r.y, 16, 16); }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung F nur dirty_mark 16x16: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* G: nur das Beschneiden */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++)
	{
		int sx, sy, dx, dy, w, h;

		ZOD_MB_POS(i);
		senke += clip_blit(k16, 0, ziel, &r, &sx, &sy, &dx, &dy, &w, &h);
	}
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung G nur clip_blit: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* H: 32x32 deckend */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); SDL_UpperBlit(k32, 0, ziel, &r); }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung H UpperBlit 32x32 deckend: %lu ns\n", zod_mb_us(t1 - t0, N));

	/* I: 16x16 mit Schluessel */
	t0 = zod_fineclock_ticks();
	for(unsigned long i = 0; i < N; i++) { ZOD_MB_POS(i); SDL_UpperBlit(kk16, 0, ziel, &r); }
	t1 = zod_fineclock_ticks();
	ZLOG("Blitmessung I UpperBlit 16x16 mit Schluessel: %lu ns\n", zod_mb_us(t1 - t0, N));

#undef ZOD_MB_POS

	(void)senke;

	SDL_FreeSurface(k16);
	SDL_FreeSurface(k32);
	SDL_FreeSurface(kk16);
	SDL_FreeSurface(neben);
}
