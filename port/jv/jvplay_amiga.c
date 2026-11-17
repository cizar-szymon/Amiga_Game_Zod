/*
 * jvplay -- Filme der Z-CD (Bitmap Brothers JV) auf AmigaOS mit RTG.
 *
 *   jvplay DATEIEN/M RTG/S AGA/S AHI/S PAULA/S NOSOUND/S SCREEN/K DOUBLE/S
 *          NOSER/S MODES/S INFO/S
 *
 * Aufrufer ist das Spiel (port/amiga/video_play.cpp):
 *   jvplay "cuts/e_logo.jv" ... RTG AHI SCREEN=640x480 DOUBLE NOSER
 * Eine Folge von Filmen auf EINEM Schirm. ESC ueberspringt den laufenden
 * Film, Strg-C beendet die ganze Folge.
 *
 * Aufbau:
 *   - die ganze Datei liegt im Speicher (der laengste Film der CD, das Intro,
 *     hat 26 MB); dekodiert wird mit port/jv/jvdec.c in einen 8-Bit-Puffer
 *     im Fast RAM, der von Bild zu Bild stehen bleibt (Blockart 0);
 *   - gezeigt wird ueber LockBitMapTagList: je Quellzeile eine verdoppelte
 *     Zeile in einen Zeilenpuffer, der zweimal in den Grafikspeicher geht;
 *   - der Ton ist ein eigener Strom: Bloecke zu 16 KB, immer vier unterwegs
 *     (AHI ueber ahi.device mit ahir_Link, Paula ueber audio.device, linker
 *     und rechter Kanal), nachgefuellt in der Hauptschleife;
 *   - Takt: Bild i erscheint bei t0 + i * Bilddauer (E-Clock). Liegt das
 *     Programm zurueck, wird das Zeigen ausgelassen, das Dekodieren nicht
 *     (die Bilder bauen aufeinander auf). Nach dem letzten Bild laeuft der
 *     Ton aus (das Intro hat danach noch 5,9 s Ausblendung).
 *
 * AGA kann der Player nicht; der Aufrufer nimmt RTG.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <devices/timer.h>
#include <devices/audio.h>
#include <devices/ahi.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/cybergraphics.h>
#include <clib/alib_protos.h>   /* BeginIO (amiga.lib) */

#include <string.h>

#include "jvdec.h"

static const char ver[] = "$VER: jvplay 1.0 (17.11.2026) Zod Engine Warp";

struct IntuitionBase *IntuitionBase = 0;
struct GfxBase *GfxBase = 0;
/* selbst oeffnen: libstubs traegt den falschen Bibliotheksnamen */
struct Library *CyberGfxBase = 0;
struct Device *TimerBase = 0;

static struct MsgPort *timer_port = 0;
static struct timerequest *timer_req = 0;

/* ------------------------------------------------------------ Zeit */

static ULONG eclock_freq = 0;

static ULONG jetzt_ms(void)
{
	struct EClockVal ev;

	ReadEClock(&ev);

	/* 64 Bit (hi:lo) / freq*1000 ohne 64-Bit-Division: ueber double */
	const double t = ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo) * 1000.0 / (double)eclock_freq;

	return (ULONG)t;
}

/* ------------------------------------------------------------ Ton */

#define TON_SLOTS  4   /* hoechstens */

/* AHI: ahir_Link traegt nur DOPPELpufferung (einer spielt, einer wartet);
 * mit einer Kette aus vier blieb AHI nach zwei Bloecken stehen (Amiga 1,
 * 17.11.2026). Deshalb dort zwei groessere Bloecke, bei Paula vier. */
static int   ton_slots = TON_SLOTS;
static ULONG ton_block = 16384;

enum { TON_AUS = 0, TON_AHI, TON_PAULA };

static int ton_weg = TON_AUS;
static struct MsgPort *ton_port = 0;

/* AHI */
static struct AHIRequest *ahi_basis = 0;
static struct AHIRequest *ahi_req[TON_SLOTS];
static struct AHIRequest *ahi_letzte = 0;

/* Paula */
static struct IOAudio *pau_basis = 0;
static struct IOAudio *pau_req[TON_SLOTS][2];
static UBYTE pau_kanaele[2] = { 0, 0 };   /* Kanalmasken links, rechts */
static UWORD pau_periode = 0;

static BYTE *ton_puffer[TON_SLOTS];
static int   ton_unterwegs[TON_SLOTS];
static ULONG ton_rate = 0;
static ULONG ton_gesendet = 0, ton_fertig = 0, ton_bytes = 0;

static int ton_auf(int weg, ULONG rate)
{
	ton_rate = rate;
	ton_port = CreateMsgPort();
	if(!ton_port) return 0;

	if(weg == TON_AHI)
	{
		ahi_basis = (struct AHIRequest*)CreateIORequest(ton_port, sizeof(struct AHIRequest));
		if(!ahi_basis) return 0;

		ahi_basis->ahir_Version = 4;
		if(OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT, (struct IORequest*)ahi_basis, 0))
		{
			DeleteIORequest((struct IORequest*)ahi_basis);
			ahi_basis = 0;
			return 0;
		}

		ton_slots = 2;
		ton_block = 32768;

		for(int i = 0; i < ton_slots; i++)
		{
			ahi_req[i] = (struct AHIRequest*)AllocVec(sizeof(struct AHIRequest), MEMF_PUBLIC);
			ton_puffer[i] = (BYTE*)AllocVec(ton_block, MEMF_PUBLIC);
			if(!ahi_req[i] || !ton_puffer[i]) return 0;
			CopyMem(ahi_basis, ahi_req[i], sizeof(struct AHIRequest));
		}

		ton_weg = TON_AHI;
		return 1;
	}

	if(weg == TON_PAULA)
	{
		static UBYTE karte[] = { 3, 5, 10, 12 };   /* je ein linker + rechter Kanal */

		pau_basis = (struct IOAudio*)CreateIORequest(ton_port, sizeof(struct IOAudio));
		if(!pau_basis) return 0;

		pau_basis->ioa_Request.io_Message.mn_Node.ln_Pri = 127;
		pau_basis->ioa_Data = karte;
		pau_basis->ioa_Length = sizeof(karte);
		pau_basis->ioa_AllocKey = 0;

		if(OpenDevice((CONST_STRPTR)AUDIONAME, 0, (struct IORequest*)pau_basis, 0))
		{
			DeleteIORequest((struct IORequest*)pau_basis);
			pau_basis = 0;
			return 0;
		}

		const ULONG maske = (ULONG)pau_basis->ioa_Request.io_Unit;
		int n = 0;

		for(int b = 0; b < 4 && n < 2; b++)
			if(maske & (1UL << b)) pau_kanaele[n++] = (UBYTE)(1 << b);

		if(n < 2) pau_kanaele[1] = pau_kanaele[0];

		const ULONG takt = (GfxBase->DisplayFlags & PAL) ? 3546895UL : 3579545UL;

		pau_periode = (UWORD)((takt + rate - 1) / rate);

		ton_slots = 4;
		ton_block = 16384;

		for(int i = 0; i < ton_slots; i++)
		{
			ton_puffer[i] = (BYTE*)AllocVec(ton_block, MEMF_CHIP);
			if(!ton_puffer[i]) return 0;

			for(int k = 0; k < 2; k++)
			{
				pau_req[i][k] = (struct IOAudio*)AllocVec(sizeof(struct IOAudio), MEMF_PUBLIC);
				if(!pau_req[i][k]) return 0;
				CopyMem(pau_basis, pau_req[i][k], sizeof(struct IOAudio));
			}
		}

		ton_weg = TON_PAULA;
		return 1;
	}

	return 0;
}

/* fertige Bloecke einsammeln */
static void ton_einsammeln(int warten_auf_alle)
{
	for(int i = 0; i < ton_slots; i++)
	{
		if(!ton_unterwegs[i]) continue;

		if(ton_weg == TON_AHI)
		{
			if(!warten_auf_alle && !CheckIO((struct IORequest*)ahi_req[i])) continue;
			WaitIO((struct IORequest*)ahi_req[i]);
			if(ahi_letzte == ahi_req[i]) ahi_letzte = 0;
		}
		else
		{
			if(!warten_auf_alle && (!CheckIO((struct IORequest*)pau_req[i][0]) ||
			                        !CheckIO((struct IORequest*)pau_req[i][1]))) continue;
			WaitIO((struct IORequest*)pau_req[i][0]);
			WaitIO((struct IORequest*)pau_req[i][1]);
		}

		ton_unterwegs[i] = 0;
		ton_fertig++;
	}
}

static int ton_frei(void)
{
	for(int i = 0; i < ton_slots; i++)
		if(!ton_unterwegs[i]) return i;

	return -1;
}

static int ton_leer(void)
{
	for(int i = 0; i < ton_slots; i++)
		if(ton_unterwegs[i]) return 0;

	return 1;
}

static void ton_senden(int i, ULONG len)
{
	if(ton_weg == TON_AHI)
	{
		struct AHIRequest *r = ahi_req[i];

		r->ahir_Std.io_Command = CMD_WRITE;
		r->ahir_Std.io_Data = ton_puffer[i];
		r->ahir_Std.io_Length = len;
		r->ahir_Std.io_Offset = 0;
		r->ahir_Type = AHIST_M8S;
		r->ahir_Frequency = ton_rate;
		r->ahir_Volume = 0x10000;
		r->ahir_Position = 0x8000;
		r->ahir_Link = ahi_letzte;
		SendIO((struct IORequest*)r);
		ahi_letzte = r;
	}
	else
	{
		len &= ~1UL;
		if(!len) return;

		for(int k = 0; k < 2; k++)
		{
			struct IOAudio *r = pau_req[i][k];

			r->ioa_Request.io_Command = CMD_WRITE;
			r->ioa_Request.io_Flags = ADIOF_PERVOL;
			r->ioa_Request.io_Unit = (struct Unit*)(ULONG)pau_kanaele[k];
			r->ioa_Data = (UBYTE*)ton_puffer[i];
			r->ioa_Length = len;
			r->ioa_Period = pau_periode;
			r->ioa_Volume = 64;
			r->ioa_Cycles = 1;
			BeginIO((struct IORequest*)r);
		}
	}

	ton_unterwegs[i] = 1;
	ton_gesendet++;
	ton_bytes += len;
}

static void ton_abbrechen(void)
{
	for(int i = 0; i < ton_slots; i++)
	{
		if(!ton_unterwegs[i]) continue;

		if(ton_weg == TON_AHI)
		{
			AbortIO((struct IORequest*)ahi_req[i]);
			WaitIO((struct IORequest*)ahi_req[i]);
		}
		else
			for(int k = 0; k < 2; k++)
			{
				AbortIO((struct IORequest*)pau_req[i][k]);
				WaitIO((struct IORequest*)pau_req[i][k]);
			}

		ton_unterwegs[i] = 0;
	}

	ahi_letzte = 0;
}

static void ton_zu(void)
{
	if(ton_weg != TON_AUS) ton_abbrechen();

	for(int i = 0; i < TON_SLOTS; i++)
	{
		if(ton_puffer[i]) FreeVec(ton_puffer[i]);
		if(ahi_req[i]) FreeVec(ahi_req[i]);
		for(int k = 0; k < 2; k++) if(pau_req[i][k]) FreeVec(pau_req[i][k]);
		ton_puffer[i] = 0; ahi_req[i] = 0; pau_req[i][0] = pau_req[i][1] = 0;
	}

	if(ahi_basis) { CloseDevice((struct IORequest*)ahi_basis); DeleteIORequest((struct IORequest*)ahi_basis); ahi_basis = 0; }
	if(pau_basis) { CloseDevice((struct IORequest*)pau_basis); DeleteIORequest((struct IORequest*)pau_basis); pau_basis = 0; }
	if(ton_port) { DeleteMsgPort(ton_port); ton_port = 0; }

	ton_weg = TON_AUS;
}

/* Tonstrom eines Films: Stuecke (Zeiger, Laenge) je Bild, der Reihe nach */
typedef struct
{
	const jv_kopf *k;
	const UBYTE   *datei;
	int    bild;         /* naechstes Bild mit Ton */
	ULONG  im_bild;      /* schon gelesene Bytes davon */
} tonstrom;

static ULONG tonstrom_holen(tonstrom *t, BYTE *ziel, ULONG max)
{
	ULONG n = 0;

	while(n < max && t->bild < t->k->bilder)
	{
		const jv_bild *b = &t->k->index[t->bild];
		const UBYTE *q = t->datei + b->ab + t->im_bild;
		ULONG rest = b->ton - t->im_bild;

		if(!rest) { t->bild++; t->im_bild = 0; continue; }
		if(rest > max - n) rest = max - n;

		/* 8 Bit vorzeichenlos -> vorzeichenbehaftet */
		for(ULONG i = 0; i < rest; i++) ziel[n + i] = (BYTE)(q[i] ^ 0x80);

		n += rest;
		t->im_bild += rest;
	}

	return n;
}

/* alle freien Plaetze mit Ton fuellen; Rueckgabe: noch Ton im Strom */
static int ton_nachfuellen(tonstrom *t)
{
	if(ton_weg == TON_AUS) return 0;

	ton_einsammeln(0);

	for(;;)
	{
		const int i = ton_frei();

		if(i < 0) return 1;

		const ULONG n = tonstrom_holen(t, ton_puffer[i], ton_block);

		if(!n) return 0;

		ton_senden(i, n);
	}
}

/* ------------------------------------------------------------ Schirm */

static struct Screen *scr = 0;
static struct Window *win = 0;
static UWORD *leerzeiger = 0;
static int scr_w = 0, scr_h = 0;

static int schirm_auf(int w, int h)
{
	ULONG id = BestCModeIDTags(CYBRBIDTG_Depth, 8,
	                           CYBRBIDTG_NominalWidth, w,
	                           CYBRBIDTG_NominalHeight, h,
	                           TAG_DONE);

	if(id == INVALID_ID) return 0;

	scr_w = (int)GetCyberIDAttr(CYBRIDATTR_WIDTH, id);
	scr_h = (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, id);

	if(scr_w < w || scr_h < h || GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8) return 0;

	scr = OpenScreenTags(NULL,
	                     SA_DisplayID, id,
	                     SA_Width, scr_w,
	                     SA_Height, scr_h,
	                     SA_Depth, 8,
	                     SA_Quiet, TRUE,
	                     SA_ShowTitle, FALSE,
	                     SA_Type, CUSTOMSCREEN,
	                     SA_Draggable, FALSE,
	                     SA_Exclusive, TRUE,
	                     TAG_DONE);
	if(!scr) return 0;

	{
		ULONG schwarz[2 + 256 * 3];

		memset(schwarz, 0, sizeof(schwarz));
		schwarz[0] = 256UL << 16;
		LoadRGB32(&scr->ViewPort, schwarz);
	}

	win = OpenWindowTags(NULL,
	                     WA_CustomScreen, (ULONG)scr,
	                     WA_Left, 0, WA_Top, 0,
	                     WA_Width, scr_w, WA_Height, scr_h,
	                     WA_Backdrop, TRUE,
	                     WA_Borderless, TRUE,
	                     WA_Activate, TRUE,
	                     WA_RMBTrap, TRUE,
	                     WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS,
	                     TAG_DONE);
	if(!win) return 0;

	leerzeiger = (UWORD*)AllocVec(16, MEMF_CHIP | MEMF_CLEAR);
	if(leerzeiger) SetPointer(win, leerzeiger, 1, 16, 0, 0);

	SetRast(&scr->RastPort, 0);

	return 1;
}

static void schirm_zu(void)
{
	if(win) { ClearPointer(win); CloseWindow(win); win = 0; }
	if(scr) { CloseScreen(scr); scr = 0; }
	if(leerzeiger) { FreeVec(leerzeiger); leerzeiger = 0; }
}

/* 0 nichts, 1 ESC (Film ueberspringen), 2 Strg-C (alles beenden) */
static int tasten(void)
{
	int r = 0;
	struct IntuiMessage *m;

	if(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) return 2;

	while(win && (m = (struct IntuiMessage*)GetMsg(win->UserPort)))
	{
		if(m->Class == IDCMP_RAWKEY && m->Code == 0x45) r = 1;   /* ESC */
		ReplyMsg((struct Message*)m);
	}

	return r;
}

static void palette_setzen(const UBYTE *pal768)
{
	ULONG tab[2 + 256 * 3];
	UBYTE rgb[768];

	jv_palette_8bit(pal768, rgb);

	tab[0] = 256UL << 16;
	for(int i = 0; i < 768; i++) tab[1 + i] = (ULONG)rgb[i] * 0x01010101UL;
	tab[1 + 768] = 0;

	LoadRGB32(&scr->ViewPort, tab);
}

static UWORD doppel[256];
static ULONG zeit_zeigen = 0, n_zeigen = 0;

static void bild_zeigen(const UBYTE *px, int w, int h, int dopp)
{
	const ULONG t0 = jetzt_ms();
	APTR basis = 0;
	ULONG bpr = 0;
	APTR lock = LockBitMapTags(scr->RastPort.BitMap,
	                           LBMI_BASEADDRESS, (ULONG)&basis,
	                           LBMI_BYTESPERROW, (ULONG)&bpr,
	                           TAG_DONE);

	if(!lock) return;

	const int f = dopp ? 2 : 1;
	const int ox = (scr_w - w * f) / 2, oy = (scr_h - h * f) / 2;
	UBYTE *z = (UBYTE*)basis + (ULONG)oy * bpr + (ULONG)ox;

	if(dopp)
	{
		static UWORD zeile[1024];

		for(int y = 0; y < h; y++)
		{
			const UBYTE *q = px + y * w;

			for(int x = 0; x < w; x++) zeile[x] = doppel[q[x]];

			CopyMem(zeile, z, (ULONG)w * 2);
			z += bpr;
			CopyMem(zeile, z, (ULONG)w * 2);
			z += bpr;
		}
	}
	else
		for(int y = 0; y < h; y++, z += bpr) CopyMem((APTR)(px + y * w), z, (ULONG)w);

	UnLockBitMap(lock);

	zeit_zeigen += jetzt_ms() - t0;
	n_zeigen++;
}

/* bis Zeitpunkt t (ms) warten, dabei Ton nachfuellen und Tasten pruefen */
static int warten_bis(ULONG t, tonstrom *ts)
{
	for(;;)
	{
		const int taste = tasten();

		if(taste) return taste;

		ton_nachfuellen(ts);

		const ULONG n = jetzt_ms();

		if((LONG)(t - n) <= 0) return 0;

		ULONG d = t - n;

		if(d > 20) d = 20;   /* Ton und Tasten weiter versorgen */

		timer_req->tr_node.io_Command = TR_ADDREQUEST;
		timer_req->tr_time.tv_secs = 0;
		timer_req->tr_time.tv_micro = d * 1000;
		DoIO((struct IORequest*)timer_req);
	}
}

/* ------------------------------------------------------------ Film */

static UBYTE *datei_laden(const char *name, ULONG *laenge)
{
	BPTR f = Open((CONST_STRPTR)name, MODE_OLDFILE);

	if(!f) return 0;

	Seek(f, 0, OFFSET_END);
	const LONG n = Seek(f, 0, OFFSET_BEGINNING);

	UBYTE *d = n > 0 ? (UBYTE*)AllocVec((ULONG)n, MEMF_ANY) : 0;

	if(d && Read(f, d, n) != n) { FreeVec(d); d = 0; }

	Close(f);
	*laenge = d ? (ULONG)n : 0;
	return d;
}

/* Rueckgabe: 0 fertig, 1 uebersprungen, 2 Strg-C, -1 Fehler */
static int film(const char *name, int dopp, int info)
{
	ULONG n = 0;
	UBYTE *d = datei_laden(name, &n);

	if(!d) { Printf("jvplay: %s nicht lesbar\n", (ULONG)name); return -1; }

	jv_kopf k;

	if(jv_kopf_lesen(d, n, &k))
	{
		Printf("jvplay: %s ist kein JV-Film\n", (ULONG)name);
		FreeVec(d);
		return -1;
	}

	if(info)
		Printf("%s: %ldx%ld, %ld Bilder zu %ld ms, Ton %ld Hz\n", (ULONG)name,
		       (LONG)k.breite, (LONG)k.hoehe, (LONG)k.bilder, (LONG)k.dauer_ms, (LONG)k.rate);

	UBYTE *px = (UBYTE*)AllocVec((ULONG)k.breite * k.hoehe, MEMF_ANY | MEMF_CLEAR);

	if(!px) { jv_frei(&k); FreeVec(d); return -1; }

	/* jeder Film beginnt schwarz */
	SetRast(&scr->RastPort, 0);

	tonstrom ts;

	ts.k = &k; ts.datei = d; ts.bild = 0; ts.im_bild = 0;

	ton_nachfuellen(&ts);

	const ULONG t0 = jetzt_ms();
	int erg = 0, gezeigt = 0, ausgelassen = 0, kaputt = 0;
	ULONG zeit_dek = 0;

	zeit_zeigen = 0; n_zeigen = 0;

	for(int i = 0; i < k.bilder && !erg; i++)
	{
		const jv_bild *b = &k.index[i];

		if(b->ab + b->gesamt > n) break;

		const UBYTE *bild = d + b->ab + b->ton;
		const ULONG td = jetzt_ms();

		if(b->bild && jv_bild_dekodieren(&k, b, bild, px) < 0) kaputt++;

		zeit_dek += jetzt_ms() - td;

		const ULONG soll = t0 + (ULONG)i * (ULONG)k.dauer_ms;
		const ULONG jetzt = jetzt_ms();

		/* mehr als ein Bild zurueck: Zeigen auslassen (nicht bei Palette) */
		if(b->bild && (LONG)(jetzt - soll) > k.dauer_ms && !b->palette)
		{
			ausgelassen++;
			erg = tasten();
			ton_nachfuellen(&ts);
			continue;
		}

		erg = warten_bis(soll, &ts);
		if(erg) break;

		if(b->bild) { bild_zeigen(px, k.breite, k.hoehe, dopp); gezeigt++; }
		if(b->palette) palette_setzen(bild + b->bild);
	}

	/* Ton auslaufen lassen -- hoechstens so lange, wie der restliche Ton
	 * dauert, plus 2 s (falls ein Ausgabeweg haengt) */
	if(!erg && ton_weg != TON_AUS)
	{
		ULONG gesamt = 0;

		for(int i = 0; i < k.bilder; i++) gesamt += k.index[i].ton;

		const ULONG ende = t0 + (ULONG)((double)gesamt * 1000.0 / (double)k.rate) + 2000;

		while(!erg && (ton_nachfuellen(&ts) || !ton_leer()) && (LONG)(ende - jetzt_ms()) > 0)
			erg = warten_bis(jetzt_ms() + 20, &ts);

		if(!ton_leer()) Printf("  Ton lief nicht aus: Zeitgrenze\n");
	}
	else if(!erg)
		erg = warten_bis(jetzt_ms() + (ULONG)k.dauer_ms, &ts);

	if(ton_weg != TON_AUS) ton_abbrechen();

	if(info && ton_weg != TON_AUS)
		Printf("  Ton: %ld Bloecke gesendet, %ld fertig, %ld Byte, Ende nach %ld ms (Bild %ld ms)\n",
		       (LONG)ton_gesendet, (LONG)ton_fertig, (LONG)ton_bytes,
		       (LONG)(jetzt_ms() - t0), (LONG)k.bilder * k.dauer_ms);

	if(info)
		Printf("  %ld gezeigt, %ld ausgelassen, %ld kaputt; je Bild dekodieren %ld us, zeigen %ld us%s\n",
		       (LONG)gezeigt, (LONG)ausgelassen, (LONG)kaputt,
		       (LONG)(k.bilder ? zeit_dek * 1000 / (ULONG)k.bilder : 0),
		       (LONG)(n_zeigen ? zeit_zeigen * 1000 / n_zeigen : 0),
		       (ULONG)(erg == 1 ? " (ESC)" : erg == 2 ? " (Strg-C)" : ""));

	FreeVec(px);
	jv_frei(&k);
	FreeVec(d);

	return erg;
}

/* ------------------------------------------------------------ MODES */

static void modi_zeigen(void)
{
	ULONG id = INVALID_ID;

	Printf("8-Bit-RTG-Modi (CLUT8):\n");

	while((id = NextDisplayInfo(id)) != INVALID_ID)
	{
		if(!IsCyberModeID(id)) continue;
		if(GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8) continue;

		struct NameInfo ni;
		const char *name = "";

		if(GetDisplayInfoData(NULL, (UBYTE*)&ni, sizeof(ni), DTAG_NAME, id)) name = (const char*)ni.Name;

		Printf("  0x%08lx %4ldx%-4ld %s\n", id, GetCyberIDAttr(CYBRIDATTR_WIDTH, id),
		       GetCyberIDAttr(CYBRIDATTR_HEIGHT, id), (ULONG)name);
	}
}

/* ------------------------------------------------------------ main */

enum { A_DATEIEN, A_RTG, A_AGA, A_AHI, A_PAULA, A_NOSOUND, A_SCREEN, A_DOUBLE,
       A_NOSER, A_MODES, A_INFO, A_N };

int main(void)
{
	LONG args[A_N];
	struct RDArgs *rd;
	int rc = 0;

	(void)ver;
	memset(args, 0, sizeof(args));

	rd = ReadArgs((CONST_STRPTR)"DATEIEN/M,RTG/S,AGA/S,AHI/S,PAULA/S,NOSOUND/S,SCREEN/K,DOUBLE/S,NOSER/S,MODES/S,INFO/S",
	              args, NULL);
	if(!rd) { PrintFault(IoErr(), (CONST_STRPTR)"jvplay"); return 20; }

	IntuitionBase = (struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
	GfxBase = (struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
	CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);

	if(!IntuitionBase || !GfxBase || !CyberGfxBase)
	{
		Printf("jvplay: intuition/graphics/cybergraphics.library fehlt\n");
		rc = 20;
		goto ende;
	}

	if(args[A_MODES]) { modi_zeigen(); goto ende; }

	if(args[A_AGA])
	{
		Printf("jvplay: AGA wird nicht unterstuetzt\n");
		rc = 20;
		goto ende;
	}

	{
		char **dateien = (char**)args[A_DATEIEN];

		if(!dateien || !dateien[0]) { Printf("jvplay: keine Datei\n"); rc = 10; goto ende; }

		timer_port = CreateMsgPort();
		timer_req = timer_port ? (struct timerequest*)CreateIORequest(timer_port, sizeof(struct timerequest)) : 0;
		if(!timer_req || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest*)timer_req, 0))
		{
			Printf("jvplay: timer.device\n");
			rc = 20;
			goto ende;
		}
		TimerBase = timer_req->tr_node.io_Device;
		{
			struct EClockVal ev;

			eclock_freq = ReadEClock(&ev);
		}

		int sw = 320, sh = 200;
		const int dopp = args[A_DOUBLE] ? 1 : 0;

		if(dopp) { sw = 640; sh = 400; }

		if(args[A_SCREEN])
		{
			LONG w = 0, h = 0;
			const char *s = (const char*)args[A_SCREEN];

			while(*s >= '0' && *s <= '9') w = w * 10 + (*s++ - '0');
			if(*s == 'x' || *s == 'X') s++;
			while(*s >= '0' && *s <= '9') h = h * 10 + (*s++ - '0');

			if(w >= sw && h >= sh) { sw = (int)w; sh = (int)h; }
		}

		for(int i = 0; i < 256; i++) doppel[i] = (UWORD)(i | (i << 8));

		if(!schirm_auf(sw, sh))
		{
			Printf("jvplay: kein 8-Bit-RTG-Modus ab %ldx%ld\n", (LONG)sw, (LONG)sh);
			rc = 20;
			goto ende;
		}

		/* Ton: Rate aus dem ersten lesbaren Film -- alle Filme der CD haben
		 * 22050 Hz; ein Film mit anderer Rate setzt den Ton neu auf. */
		int weg = args[A_NOSOUND] ? TON_AUS : args[A_PAULA] ? TON_PAULA : TON_AHI;

		for(int f = 0; dateien[f]; f++)
		{
			if(weg != TON_AUS)
			{
				/* Rate des Films aus dem Kopf (die ersten 104 Byte) */
				BPTR fh = Open((CONST_STRPTR)dateien[f], MODE_OLDFILE);
				UBYTE kopf[104];
				ULONG rate = 22050;

				if(fh)
				{
					if(Read(fh, kopf, 104) == 104) rate = (ULONG)(kopf[92] | (kopf[93] << 8));
					Close(fh);
				}

				if(ton_weg == TON_AUS || rate != ton_rate)
				{
					ton_zu();
					if(!ton_auf(weg, rate))
					{
						Printf("jvplay: Ton (%s) laesst sich nicht oeffnen -- stumm\n",
						       (ULONG)(weg == TON_AHI ? "AHI" : "Paula"));
						ton_zu();
						weg = TON_AUS;
					}
				}
			}

			const int e = film(dateien[f], dopp, args[A_INFO] ? 1 : 0);

			if(e == 2) { rc = 5; break; }
			if(e < 0) rc = 5;
		}
	}

ende:
	ton_zu();
	schirm_zu();
	if(timer_req)
	{
		if(TimerBase) CloseDevice((struct IORequest*)timer_req);
		DeleteIORequest((struct IORequest*)timer_req);
	}
	if(timer_port) DeleteMsgPort(timer_port);
	if(CyberGfxBase) CloseLibrary(CyberGfxBase);
	if(GfxBase) CloseLibrary((struct Library*)GfxBase);
	if(IntuitionBase) CloseLibrary((struct Library*)IntuitionBase);
	FreeArgs(rd);

	return rc;
}
