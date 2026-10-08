/* Cost of one zero-timeout select() versus poll() over N descriptors (pipes,
 * one ready), as a run loop pays it every iteration.  poll_vs_select [fds] [iterations] */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 14, iters = argc > 2 ? atoi(argv[2]) : 5000;
	int rd[64], wr[64], mx = 0;
	struct pollfd pf[64];
	for (int i = 0; i < n; i++) {
		int p[2];
		pipe(p);
		rd[i] = p[0];
		wr[i] = p[1];
		pf[i] = (struct pollfd){ p[0], POLLIN, 0 };
		if (p[0] > mx) mx = p[0];
	}
	write(wr[n - 1], "x", 1);       /* one fd ready, like a busy loop */
	double t0 = now_us();
	for (int k = 0; k < iters; k++) {
		fd_set rf;
		struct timeval tv = { 0, 0 };
		FD_ZERO(&rf);
		for (int i = 0; i < n; i++)
			FD_SET(rd[i], &rf);
		select(mx + 1, &rf, NULL, NULL, &tv);
	}
	double t1 = now_us();
	for (int k = 0; k < iters; k++) {
		for (int i = 0; i < n; i++)
			pf[i].revents = 0;
		poll(pf, n, 0);
	}
	double t2 = now_us();
	printf("%d fds: select %.1f us/call, poll %.1f us/call\n", n, (t1 - t0) / iters, (t2 - t1) / iters);
	return 0;
}
