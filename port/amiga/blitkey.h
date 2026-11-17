#ifndef ZOD_BLITKEY_H
#define ZOD_BLITKEY_H
/*
 * 8-Bit-Blit mit Farbschluessel 0, vier Bildpunkte je Schritt (Warp F3).
 *
 * Gemessen auf Amiga 2 (Plan 6j): ein sichtbarer Stein kostet rund 40 us --
 * drei 16x16-Kacheln, jede durch die Byteschleife in SDL_UpperBlit (Lesen,
 * Vergleich, bedingtes Schreiben je Bildpunkt).
 *
 * Hier wird je Langwort entschieden:
 *   alle vier Punkte 0      -> nichts schreiben
 *   kein Punkt 0            -> das Langwort schreiben
 *   gemischt                -> die vier Punkte einzeln wie bisher
 * "Hat ein Byte 0" ist der bekannte Test (v - 0x01010101) & ~v & 0x80808080;
 * er ist fuer die FRAGE, OB ein Nullbyte vorkommt, exakt.
 *
 * Das Ergebnis ist Byte fuer Byte dasselbe wie in der Byteschleife: Jeder
 * Zielpunkt bekommt den Quellpunkt, wenn dieser nicht 0 ist, sonst bleibt er.
 * Geschrieben wird erst ab einer durch 4 teilbaren Zieladresse (Kopf und
 * Schwanz byteweise); die Quelle darf beliebig liegen -- der 68060 liest
 * unausgerichtete Langwoerter selbst (gcc: -m68060 ohne -mstrict-align).
 *
 * Die alte Byteschleife steht daneben (zod_blit_key_bytes) fuer die
 * Pruefung ZOD_BLITCHECK und fuer den Test auf dem PC (tests/blitkey_test.c).
 */
#include <stdint.h>

typedef uint32_t __attribute__((__may_alias__)) zod_u32a;

static inline void zod_blit_key_bytes(unsigned char *dp, const unsigned char *sp,
                                      int w, int h, int dpitch, int spitch,
                                      unsigned char key)
{
	for(int y = 0; y < h; y++)
	{
		for(int x = 0; x < w; x++)
		{
			const unsigned char v = sp[x];

			if(v != key) dp[x] = v;
		}

		sp += spitch;
		dp += dpitch;
	}
}

static inline void zod_blit_key0_long(unsigned char *dp, const unsigned char *sp,
                                      int w, int h, int dpitch, int spitch)
{
	for(int y = 0; y < h; y++)
	{
		int x = 0;

		/* Kopf bis zur ausgerichteten Zieladresse */
		while(x < w && (((uintptr_t)(dp + x)) & 3))
		{
			const unsigned char v = sp[x];

			if(v) dp[x] = v;
			x++;
		}

		for(; x + 4 <= w; x += 4)
		{
			const uint32_t v = *(const zod_u32a*)(sp + x);

			if(!v) continue;

			if(!((v - 0x01010101u) & ~v & 0x80808080u))
			{
				*(zod_u32a*)(dp + x) = v;
				continue;
			}

			if(sp[x])     dp[x]     = sp[x];
			if(sp[x + 1]) dp[x + 1] = sp[x + 1];
			if(sp[x + 2]) dp[x + 2] = sp[x + 2];
			if(sp[x + 3]) dp[x + 3] = sp[x + 3];
		}

		for(; x < w; x++)
		{
			const unsigned char v = sp[x];

			if(v) dp[x] = v;
		}

		sp += spitch;
		dp += dpitch;
	}
}

#endif
