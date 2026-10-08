/* Wake-up latency between two processes, as a run loop sees it: the parent
 * blocks in read() or poll() (with a long timeout) on a pipe, the child writes
 * its own timestamp into it every ~40 ms, and the parent reports now - stamp.
 *   wake_latency [writes] [read|poll|select|kevent] */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100;
	const char *how = argc > 2 ? argv[2] : "poll";
	int p[2];
	pipe(p);
	pid_t pid = fork();
	if (pid == 0) {
		for (int i = 0; i < n; i++) {
			usleep(40000);
			double t = now_ms();
			write(p[1], &t, sizeof t);
		}
		_exit(0);
	}
	int kq = kqueue();
	struct kevent kev;
	EV_SET(&kev, p[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
	kevent(kq, &kev, 1, NULL, 0, NULL);
	double *v = malloc(n * sizeof(double)), sum = 0, max = 0;
	int slow = 0;
	for (int i = 0; i < n; i++) {
		double t;
		if (!strcmp(how, "poll")) {
			struct pollfd pf = { p[0], POLLIN, 0 };
			poll(&pf, 1, 5000);
		} else if (!strcmp(how, "select")) {
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(p[0], &fs);
			struct timeval tv = { 5, 0 };
			select(p[0] + 1, &fs, NULL, NULL, &tv);
		} else if (!strcmp(how, "kevent")) {
			struct kevent out;
			struct timespec ts = { 5, 0 };
			kevent(kq, NULL, 0, &out, 1, &ts);
		}
		if (read(p[0], &t, sizeof t) != sizeof t)
			break;
		v[i] = now_ms() - t;
		sum += v[i];
		if (v[i] > max) max = v[i];
		if (v[i] > 20) slow++;
	}
	printf("%-7s wake: n=%d avg=%.2fms max=%.2fms over20ms=%d\n", how, n, sum / n, max, slow);
	kill(pid, SIGKILL);
	return 0;
}
