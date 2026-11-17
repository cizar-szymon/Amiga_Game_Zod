/* Host test: zod_blit_key0_long == zod_blit_key_bytes(key 0), byte for byte.
 *
 *   gcc -O2 -std=gnu99 -Iport/amiga tests/blitkey_test.c -o /tmp/bk && /tmp/bk
 *
 * Random sources with runs of zeros (transparent), all source/destination
 * alignments 0..7, widths 1..40, heights 1..5. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blitkey.h"

#define SW 64
#define SH 8
#define DW 80
#define DH 10

int main(void)
{
	static unsigned char src[SH * SW + 16], d1[DH * DW + 16], d2[DH * DW + 16];
	unsigned long tests = 0, fails = 0;

	srand(12345);

	for(int round = 0; round < 400; round++)
	{
		for(int i = 0; i < (int)sizeof(src); i++)
		{
			const int r = rand() % 10;

			src[i] = (r < 4) ? 0 : (unsigned char)(1 + rand() % 255);
		}

		for(int so = 0; so < 8; so++)
			for(int doff = 0; doff < 8; doff++)
				for(int w = 1; w <= 40; w += (w < 10 ? 1 : 3))
				{
					const int h = 1 + rand() % 5;

					for(int i = 0; i < (int)sizeof(d1); i++) d1[i] = (unsigned char)rand();
					memcpy(d2, d1, sizeof(d1));

					zod_blit_key_bytes(d1 + doff, src + so, w, h, DW, SW, 0);
					zod_blit_key0_long(d2 + doff, src + so, w, h, DW, SW);

					tests++;
					if(memcmp(d1, d2, sizeof(d1)))
					{
						if(fails < 5) printf("FAIL so=%d do=%d w=%d h=%d\n", so, doff, w, h);
						fails++;
					}
				}
	}

	printf("blitkey: %lu tests, %lu fails\n", tests, fails);

	return fails ? 1 : 0;
}
