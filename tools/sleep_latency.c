/* How long do 20 ms waits really take here? poll, select, usleep, nanosleep,
 * kevent, each 50 times. A run loop built on any of these wakes no more
 * precisely than this. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void report(const char *name, double *v, int n)
{
	double sum = 0, max = 0, min = 1e9;
	for (int i = 0; i < n; i++) {
		sum += v[i];
		if (v[i] > max) max = v[i];
		if (v[i] < min) min = v[i];
	}
	printf("%-10s 20ms wait: min=%.1f avg=%.1f max=%.1f ms\n", name, min, sum / n, max);
}

int main(int argc, char **argv)
{
	/* "fg": leave the Darwin background state (PRIO_DARWIN_PROCESS = 4), which an
	 * ssh session starts in and which gives timers up to 100 ms of slack */
	if (argc > 1 && !strcmp(argv[1], "fg"))
		printf("setpriority fg: %d\n", setpriority(4, 0, 0));
	enum { N = 50 };
	double v[N];
	int kq = kqueue();
	for (int k = 0; k < 5; k++) {
		for (int i = 0; i < N; i++) {
			double t0 = now_ms();
			switch (k) {
			case 0: poll(NULL, 0, 20); break;
			case 1: { struct timeval tv = { 0, 20000 }; select(0, NULL, NULL, NULL, &tv); break; }
			case 2: usleep(20000); break;
			case 3: { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); break; }
			case 4: { struct kevent ev; struct timespec ts = { 0, 20000000 }; kevent(kq, NULL, 0, &ev, 1, &ts); break; }
			}
			v[i] = now_ms() - t0;
		}
		report((const char *[]){ "poll", "select", "usleep", "nanosleep", "kevent" }[k], v, N);
	}
	return 0;
}
