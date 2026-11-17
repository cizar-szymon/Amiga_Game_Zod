/* Prueftool auf dem PC: JV-Datei -> rohe RGB24-Bilder + WAV.
 *
 *   jvdump DATEI.JV bilder.rgb ton.wav
 *
 * Geschrieben wird je Bild MIT Bilddaten ein Bild (wie FFmpeg mit
 * -fps_mode passthrough), damit tools/jv_vergleich.sh beide Stroeme Byte fuer
 * Byte vergleichen kann. */
#include "jvdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE *f, unsigned long v) { w16(f, v & 0xFFFF); w16(f, (v >> 16) & 0xFFFF); }

int main(int argc, char **argv)
{
	if(argc < 4) { fprintf(stderr, "jvdump DATEI.JV bilder.rgb ton.wav\n"); return 10; }

	FILE *f = fopen(argv[1], "rb");
	if(!f) { perror(argv[1]); return 10; }

	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);

	uint8_t *d = (uint8_t*)malloc((size_t)n);
	if(!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "lesen\n"); return 10; }
	fclose(f);

	jv_kopf k;
	int r = jv_kopf_lesen(d, (uint32_t)n, &k);

	if(r) { fprintf(stderr, "kein JV (%d)\n", r); return 10; }

	printf("%dx%d, %d Bilder, %d ms, %d Hz\n", k.breite, k.hoehe, k.bilder, k.dauer_ms, k.rate);

	FILE *fb = fopen(argv[2], "wb");
	FILE *ft = fopen(argv[3], "wb");
	uint8_t *px = (uint8_t*)calloc((size_t)k.breite * k.hoehe, 1);
	uint8_t pal[768], rgb[768], *zeile = (uint8_t*)malloc((size_t)k.breite * 3);
	unsigned long tonbytes = 0;
	int bilder = 0, kaputt = 0;

	memset(pal, 0, sizeof(pal));
	memset(rgb, 0, sizeof(rgb));

	/* WAV-Kopf, Laengen am Ende */
	fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, ft);
	w32(ft, 16); w16(ft, 1); w16(ft, 1); w32(ft, (unsigned long)k.rate); w32(ft, (unsigned long)k.rate);
	w16(ft, 1); w16(ft, 8);
	fwrite("data\0\0\0\0", 1, 8, ft);

	for(int i = 0; i < k.bilder; i++)
	{
		const jv_bild *b = &k.index[i];

		if((long)b->ab + (long)b->gesamt > n) { fprintf(stderr, "Bild %d hinter dem Dateiende\n", i); break; }

		const uint8_t *p = d + b->ab;

		fwrite(p, 1, b->ton, ft);
		tonbytes += b->ton;
		p += b->ton;

		if(b->bild && jv_bild_dekodieren(&k, b, p, px) < 0) kaputt++;

		if(b->palette)
		{
			memcpy(pal, p + b->bild, 768);
			jv_palette_8bit(pal, rgb);
		}

		if(b->bild)
		{
			for(int y = 0; y < k.hoehe; y++)
			{
				for(int x = 0; x < k.breite; x++)
				{
					const uint8_t c = px[y * k.breite + x];

					zeile[x * 3] = rgb[c * 3];
					zeile[x * 3 + 1] = rgb[c * 3 + 1];
					zeile[x * 3 + 2] = rgb[c * 3 + 2];
				}
				fwrite(zeile, 1, (size_t)k.breite * 3, fb);
			}
			bilder++;
		}
	}

	fseek(ft, 4, SEEK_SET); w32(ft, 36 + tonbytes);
	fseek(ft, 40, SEEK_SET); w32(ft, tonbytes);
	fclose(ft);
	fclose(fb);

	printf("%d Bilder geschrieben, %d kaputt, Ton %lu Byte (%.2f s)\n",
	       bilder, kaputt, tonbytes, (double)tonbytes / k.rate);

	jv_frei(&k);
	free(d);
	return kaputt ? 5 : 0;
}
