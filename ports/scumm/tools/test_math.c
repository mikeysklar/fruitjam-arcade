/* Host test for src/libc/fj_math.c against the system libm. */
#include <math.h>
#include <stdio.h>
double t_sin(double), t_pow(double, double), t_log10(double);
int main(void) {
	double worst = 0;
	for (double x = -50; x < 50; x += 0.0137) {
		double e = fabs(t_sin(x) - sin(x));
		if (e > worst) worst = e;
	}
	for (double x = 0.001; x < 1000; x *= 1.013) {
		double e = fabs(t_log10(x) - log10(x));
		if (e > worst) worst = e;
		for (double y = -3.3; y < 8.5; y += 0.71) {
			double r = pow(x, y), d = fabs(t_pow(x, y) - r) / (fabs(r) > 1 ? fabs(r) : 1);
			if (d > worst) worst = d;
		}
	}
	printf("math: worst error %.3g, %s\n", worst, worst < 1e-9 ? "ok" : "FAIL");
	return worst >= 1e-9;
}
