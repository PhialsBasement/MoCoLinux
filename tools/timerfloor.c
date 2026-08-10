/*
 * timerfloor -- measure THE floor: the smallest sleep Windows can deliver
 * on this machine, using the best documented primitive in the OS
 * (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), with the multimedia clock raised.
 * Prints the distribution; no interpretation, just the numbers.
 *
 * x86_64-w64-mingw32-gcc -O2 timerfloor.c -o timerfloor.exe -lwinmm
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER f;

static double now_us(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}

static int cmp(const void *a, const void *b)
{
	double d = *(const double *)a - *(const double *)b;
	return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void measure(HANDLE timer, double req_us, int n)
{
	double *xs = malloc(n * sizeof(double));
	LARGE_INTEGER due;
	int i;

	for (i = 0; i < n; i++) {
		double t0;

		due.QuadPart = -(LONGLONG)(req_us * 10.0);
		t0 = now_us();
		SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
		WaitForSingleObject(timer, INFINITE);
		xs[i] = now_us() - t0;
	}
	qsort(xs, n, sizeof(double), cmp);
	printf("req %7.0f us: min %7.0f  p50 %7.0f  p95 %7.0f  max %7.0f\n",
	       req_us, xs[0], xs[n / 2], xs[(int)(n * 0.95)], xs[n - 1]);
	free(xs);
}

int main(void)
{
	HANDLE t;

	QueryPerformanceFrequency(&f);
	timeBeginPeriod(1);

	t = CreateWaitableTimerExW(NULL, NULL,
				   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
				   TIMER_ALL_ACCESS);
	if (!t) {
		printf("high-resolution timer NOT available (err %lu);"
		       " falling back\n", GetLastError());
		t = CreateWaitableTimerW(NULL, FALSE, NULL);
	} else {
		printf("high-resolution waitable timer: available\n");
	}

	printf("-- the floor of this machine, user mode, %d samples each --\n",
	       200);
	measure(t, 50, 200);
	measure(t, 100, 200);
	measure(t, 500, 200);
	measure(t, 1000, 200);
	measure(t, 5000, 100);

	timeEndPeriod(1);
	return 0;
}
