#ifndef ZOD_JVDEC_H
#define ZOD_JVDEC_H
/*
 * Bitmap Brothers JV (Filme der Z-CD, Verzeichnis CUTS, Endung .JV) -- Lesen und
 * Entschluesseln, ohne Plattformbezug. Benutzt vom Amiga-Player
 * (port/jv/jvplay_amiga.c) und vom Prueftool auf dem PC (port/jv/jvdump.c).
 *
 * Eigene Umsetzung nach der Formatbeschreibung; das Format ist auch in FFmpeg
 * dokumentiert (libavformat/jvdec.c, libavcodec/jvdec.c), gegen dessen
 * Ausgabe tools/jv_vergleich.sh Bild fuer Bild prueft.
 *
 * Datei (alle Zahlen Little Endian):
 *   0    "JV"                 Kennung (die Z-CD hat "JV00")
 *   4    " Compression by John M Phillips ..."
 *   80   u16 Breite, u16 Hoehe, u16 Bilder, u16 Bilddauer in ms
 *   88   4 Byte frei
 *   92   u16 Abtastrate (Ton: 8 Bit vorzeichenlos, mono)
 *   94   10 Byte frei
 *   104  je Bild 16 Byte: u32 Gesamtgroesse, u32 Ton, u32 Bild,
 *        u8 Palette (!=0: 768 Byte), u8 Tonart, u8 Bildart, u8 frei
 * Danach die Bilder: Ton, Bild, Palette (256 x RGB, je 6 Bit), Rest.
 *
 * Bildart 0/1: Bitstrom (hoechstes Bit zuerst), 8x8-Bloecke zeilenweise,
 * je Block 2 Bit Art (0 unveraendert, 1 eine Farbe, 2 zwei Farben + 1 Bit
 * je Punkt, 3 vier Teilbloecke 4x4 -> 2x2, auf 2x2 vier rohe Punkte).
 * Bildart 2: ein Byte, ganzes Bild in dieser Farbe.
 */

#include <stdint.h>

typedef struct
{
	uint32_t gesamt, ton, bild;
	uint8_t  palette, tonart, bildart;
	uint32_t ab;             /* Dateiversatz des Bildes */
} jv_bild;

typedef struct
{
	int      breite, hoehe, bilder, dauer_ms, rate;
	jv_bild *index;          /* bilder Eintraege */
} jv_kopf;

/* Kopf + Index aus den ersten Bytes der Datei (mindestens 104 + 16 * Bilder).
 * Rueckgabe 0 = gut. index wird mit malloc angelegt (jv_frei). */
int  jv_kopf_lesen(const uint8_t *daten, uint32_t laenge, jv_kopf *k);
void jv_frei(jv_kopf *k);

/* Ein Bild entschluesseln. bild zeigt auf die Bilddaten (k->index[i].bild
 * Byte), pixel auf breite*hoehe Byte, die das vorige Bild enthalten (Art 0
 * laesst Punkte stehen). Rueckgabe 0 = gut, <0 = kaputt. */
int  jv_bild_dekodieren(const jv_kopf *k, const jv_bild *b, const uint8_t *bild,
                        uint8_t *pixel);

/* 768 Byte (6 Bit je Anteil) -> 256 x RGB mit 8 Bit je Anteil. */
void jv_palette_8bit(const uint8_t *pal768, uint8_t *rgb768);

#endif
