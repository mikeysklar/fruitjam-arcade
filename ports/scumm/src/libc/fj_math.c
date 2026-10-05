/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The libm functions the MAME OPL emulator needs to build its tables
 * (once, at start-up): sin, pow and log10, in plain double precision
 * series. Accurate to about 1e-12, which is far more than the tables keep.
 */

#include <stdint.h>
#include <string.h>

#define PI 3.14159265358979323846
#define LN2 0.69314718055994530942
#define LN10 2.30258509299404568402

static double scale2(double x, int e) {
	/* x * 2^e by building the power of two from its bits */
	while (e > 1000) {
		x *= 8.98846567431158e307; /* 2^1023 */
		e -= 1023;
	}
	while (e < -1000) {
		x *= 1.1125369292536007e-308; /* 2^-1022 */
		e += 1022;
	}
	uint64_t bits = (uint64_t)(e + 1023) << 52;
	double p;
	memcpy(&p, &bits, 8);
	return x * p;
}

double sin(double x) {
	double k = x / (2 * PI);
	k = (double)(long long)(k + (k >= 0 ? 0.5 : -0.5));
	x -= k * 2 * PI; /* now in [-pi, pi] */
	if (x > PI / 2)
		x = PI - x;
	else if (x < -PI / 2)
		x = -PI - x;
	double x2 = x * x, term = x, sum = x;
	for (int n = 1; n < 12; n++) {
		term *= -x2 / ((2 * n) * (2 * n + 1));
		sum += term;
	}
	return sum;
}

static double fj_log(double x) {
	if (x <= 0)
		return -1e308;
	uint64_t bits;
	memcpy(&bits, &x, 8);
	int e = (int)((bits >> 52) & 0x7ff) - 1023;
	bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL; /* mantissa in [1, 2) */
	double m;
	memcpy(&m, &bits, 8);
	if (m > 1.41421356237309504880) {
		m *= 0.5;
		e++;
	}
	double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
	for (int n = 1; n < 40; n += 2) {
		sum += term / n;
		term *= s2;
	}
	return 2 * sum + e * LN2;
}

static double fj_exp(double x) {
	if (x > 709)
		return 1e308;
	if (x < -745)
		return 0;
	int k = (int)(x / LN2 + (x >= 0 ? 0.5 : -0.5));
	double r = x - k * LN2, term = 1, sum = 1;
	for (int n = 1; n < 25; n++) {
		term *= r / n;
		sum += term;
	}
	return scale2(sum, k);
}

double log10(double x) {
	return fj_log(x) / LN10;
}

double pow(double x, double y) {
	if (y == 0)
		return 1;
	if (x == 0)
		return 0;
	long long iy = (long long)y;
	if ((double)iy == y && iy > -64 && iy < 64) {
		double r = 1, b = x;
		long long n = iy < 0 ? -iy : iy;
		while (n) {
			if (n & 1)
				r *= b;
			b *= b;
			n >>= 1;
		}
		return iy < 0 ? 1 / r : r;
	}
	if (x < 0)
		return 0; /* not needed here (would be NaN) */
	return fj_exp(y * fj_log(x));
}
