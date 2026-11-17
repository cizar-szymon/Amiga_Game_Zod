/* Bitmap Brothers JV -- siehe jvdec.h. */
#include "jvdec.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int jv_kopf_lesen(const uint8_t *d, uint32_t n, jv_kopf *k)
{
	memset(k, 0, sizeof(*k));

	if(n < 104 || d[0] != 'J' || d[1] != 'V') return -1;

	k->breite   = rd16(d + 80);
	k->hoehe    = rd16(d + 82);
	k->bilder   = rd16(d + 84);
	k->dauer_ms = rd16(d + 86);
	k->rate     = rd16(d + 92);

	if(k->breite <= 0 || k->hoehe <= 0 || (k->breite & 7) || (k->hoehe & 7)) return -2;
	if(n < 104u + 16u * (uint32_t)k->bilder) return -3;

	k->index = (jv_bild*)malloc(sizeof(jv_bild) * (size_t)(k->bilder ? k->bilder : 1));
	if(!k->index) return -4;

	uint32_t ab = 104u + 16u * (uint32_t)k->bilder;

	for(int i = 0; i < k->bilder; i++)
	{
		const uint8_t *e = d + 104 + 16 * i;
		jv_bild *b = &k->index[i];

		b->gesamt  = rd32(e);
		b->ton     = rd32(e + 4);
		b->bild    = rd32(e + 8);
		b->palette = e[12];
		b->tonart  = e[13];
		b->bildart = e[14];
		b->ab      = ab;

		/* unplausibel: wie FFmpeg alles verwerfen, Platz aber behalten */
		if(b->ton > 0xFFFFFF || b->bild > 0xFFFFFF ||
		   b->ton + b->bild + (b->palette ? 768u : 0u) > b->gesamt)
			b->ton = b->bild = 0, b->palette = 0;

		ab += b->gesamt;
	}

	return 0;
}

void jv_frei(jv_kopf *k)
{
	free(k->index);
	k->index = 0;
}

/* Bitleser, hoechstes Bit zuerst. Liest hinter dem Ende Nullen. */
typedef struct
{
	const uint8_t *p, *ende;
	uint32_t puffer;      /* linksbuendig */
	int      bits;        /* gueltige Bits im Puffer */
} bitleser;

static inline void bl_fuellen(bitleser *b)
{
	while(b->bits <= 24)
	{
		uint32_t v = (b->p < b->ende) ? *b->p++ : 0;

		b->puffer |= v << (24 - b->bits);
		b->bits += 8;
	}
}

static inline unsigned bl_hol(bitleser *b, int n)
{
	if(b->bits < n) bl_fuellen(b);

	unsigned v = b->puffer >> (32 - n);

	b->puffer <<= n;
	b->bits -= n;
	return v;
}

static inline unsigned bl_1(bitleser *b) { return bl_hol(b, 1); }

static void block2(bitleser *b, uint8_t *d, int z)
{
	uint8_t v[2];

	switch(bl_hol(b, 2))
	{
	case 1:
		v[0] = (uint8_t)bl_hol(b, 8);
		d[0] = d[1] = d[z] = d[z + 1] = v[0];
		break;
	case 2:
		v[0] = (uint8_t)bl_hol(b, 8);
		v[1] = (uint8_t)bl_hol(b, 8);
		d[0]     = v[bl_1(b)];
		d[1]     = v[bl_1(b)];
		d[z]     = v[bl_1(b)];
		d[z + 1] = v[bl_1(b)];
		break;
	case 3:
		d[0]     = (uint8_t)bl_hol(b, 8);
		d[1]     = (uint8_t)bl_hol(b, 8);
		d[z]     = (uint8_t)bl_hol(b, 8);
		d[z + 1] = (uint8_t)bl_hol(b, 8);
		break;
	}
}

static void block4(bitleser *b, uint8_t *d, int z)
{
	uint8_t v[2];

	switch(bl_hol(b, 2))
	{
	case 1:
		v[0] = (uint8_t)bl_hol(b, 8);
		for(int y = 0; y < 4; y++) memset(d + y * z, v[0], 4);
		break;
	case 2:
		v[0] = (uint8_t)bl_hol(b, 8);
		v[1] = (uint8_t)bl_hol(b, 8);
		/* Zeilen in der Folge 2, 3, 0, 1 */
		for(int y = 2; y >= 0; y -= 2)
		{
			for(int x = 0; x < 4; x++) d[y * z + x] = v[bl_1(b)];
			for(int x = 0; x < 4; x++) d[(y + 1) * z + x] = v[bl_1(b)];
		}
		break;
	case 3:
		block2(b, d, z);
		block2(b, d + 2, z);
		block2(b, d + 2 * z, z);
		block2(b, d + 2 * z + 2, z);
		break;
	}
}

static void block8(bitleser *b, uint8_t *d, int z)
{
	uint8_t v[2];

	switch(bl_hol(b, 2))
	{
	case 1:
		v[0] = (uint8_t)bl_hol(b, 8);
		for(int y = 0; y < 8; y++) memset(d + y * z, v[0], 8);
		break;
	case 2:
		v[0] = (uint8_t)bl_hol(b, 8);
		v[1] = (uint8_t)bl_hol(b, 8);
		/* Zeilen von unten nach oben */
		for(int y = 7; y >= 0; y--)
			for(int x = 0; x < 8; x++) d[y * z + x] = v[bl_1(b)];
		break;
	case 3:
		block4(b, d, z);
		block4(b, d + 4, z);
		block4(b, d + 4 * z, z);
		block4(b, d + 4 * z + 4, z);
		break;
	}
}

int jv_bild_dekodieren(const jv_kopf *k, const jv_bild *bi, const uint8_t *bild,
                       uint8_t *pixel)
{
	const int w = k->breite, h = k->hoehe;

	if(!bi->bild) return 0;                 /* nur Palette oder nichts */

	if(bi->bildart == 0 || bi->bildart == 1)
	{
		if((uint32_t)(h / 8) * (uint32_t)(w / 8) > 4u * bi->bild) return -1;

		bitleser b;

		b.p = bild;
		b.ende = bild + bi->bild;
		b.puffer = 0;
		b.bits = 0;

		for(int y = 0; y < h; y += 8)
			for(int x = 0; x < w; x += 8)
				block8(&b, pixel + y * w + x, w);

		return 0;
	}

	if(bi->bildart == 2)
	{
		memset(pixel, bild[0], (size_t)w * h);
		return 0;
	}

	return -2;
}

void jv_palette_8bit(const uint8_t *p, uint8_t *rgb)
{
	for(int i = 0; i < 768; i++)
	{
		const uint8_t v = p[i] & 63;

		rgb[i] = (uint8_t)((v << 2) | (v >> 4));
	}
}
