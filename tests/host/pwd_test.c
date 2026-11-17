/* Erschoepfender Test: COMMON::points_within_distance -- alte Fassung (floor,
 * sqrt) gegen die ganzzahlige (Warp F3, plan 6u). Alle dx, dy in
 * [-d-2, d+2] fuer d = -3 .. 1100.
 *
 *   gcc -O2 tests/host/pwd_test.c -o pwd_test -lm && ./pwd_test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int alt(int x1, int y1, int x2, int y2, int distance)
{
	if(x2 < x1 - distance) return 0;
	if(x2 > x1 + distance) return 0;
	if(y2 < y1 - distance) return 0;
	if(y2 > y1 + distance) return 0;

	int sh_dist = (int)(floor(distance * 0.707106781 + 0.5));
	int dx = abs(x1 - x2);
	int dy = abs(y1 - y2);
	if(dx < sh_dist && dy < sh_dist) return 1;

	if(sqrt((dx * dx) + (dy * dy)) > distance) return 0;

	return 1;
}

static int neu(int x1, int y1, int x2, int y2, int distance)
{
	if(x2 < x1 - distance) return 0;
	if(x2 > x1 + distance) return 0;
	if(y2 < y1 - distance) return 0;
	if(y2 > y1 + distance) return 0;

	const int dx = x1 - x2, dy = y1 - y2;

	return dx * dx + dy * dy <= distance * distance;
}

int main(void)
{
	unsigned long long n = 0, falsch = 0;

	for(int d = -3; d <= 1100; d++)
	{
		const int r = (d < 0 ? -d : d) + 2;

		for(int dx = -r; dx <= r; dx++)
			for(int dy = -r; dy <= r; dy++)
			{
				n++;
				if(alt(1000, 2000, 1000 + dx, 2000 + dy, d) != neu(1000, 2000, 1000 + dx, 2000 + dy, d))
				{
					if(falsch < 10) printf("falsch: d=%d dx=%d dy=%d\n", d, dx, dy);
					falsch++;
				}
			}
	}

	printf("geprueft %llu, falsch %llu\n", n, falsch);
	return falsch ? 1 : 0;
}
