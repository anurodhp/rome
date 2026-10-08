/* How long does the pty take to echo a typed byte? Runs `cat` on a pty, writes
 * a byte every `period` ms and times the echo's arrival on the master.
 *   pty_latency [count] [period_ms]
 * No X, no GNUstep: this is the kernel's tty and pty path alone. */
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200, period = argc > 2 ? atoi(argv[2]) : 50;
	int fd;
	alarm(25);      /* a poll() that never reports the echo would hang the test */
	pid_t pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid == 0) {
		execl("/bin/cat", "cat", (char *)NULL);
		_exit(127);
	}
	usleep(300000);
	char b[256];
	struct pollfd p = { fd, POLLIN, 0 };
	while (poll(&p, 1, 0) > 0 && read(fd, b, sizeof b) > 0)
		;
	double *lat = malloc(n * sizeof(double));
	int lost = 0;
	if (argc > 3 && !strcmp(argv[3], "loop")) {
		/* a run loop's shape: poll the master with a short timeout all the
		 * time, write a byte whenever `period` ms have passed, and time the
		 * echo from the write to the poll that reports it */
		/* optional extra descriptors in the poll set, as a run loop has: argv[4] =
		 * number of pipes, argv[5] = number of socketpairs, "pri" = also POLLPRI */
		struct pollfd set[64];
		int ns = 0, extra_pipes = argc > 4 ? atoi(argv[4]) : 0, extra_socks = argc > 5 ? atoi(argv[5]) : 0;
		set[ns++] = (struct pollfd){ fd, POLLIN, 0 };
		for (int i = 0; i < extra_pipes && ns < 60; i++) {
			int q[2];
			pipe(q);
			set[ns++] = (struct pollfd){ q[0], POLLIN, 0 };
		}
		for (int i = 0; i < extra_socks && ns < 60; i++) {
			int q[2];
			socketpair(1, 1, 0, q);        /* AF_UNIX, SOCK_STREAM */
			set[ns++] = (struct pollfd){ q[0], POLLIN, 0 };
		}
		double t_next = now_ms(), sent = 0, worst = 0, sum = 0;
		int pending = 0, done = 0, over = 0;
		while (done < n) {
			double now = now_ms();
			if (now >= t_next && !pending) {
				char c = 'a' + done % 26;
				write(fd, &c, 1);
				sent = now_ms();
				pending = 1;
				t_next = now + period;
			}
			for (int i = 0; i < ns; i++)
				set[i].revents = 0;
			if (argc > 6 && !strcmp(argv[6], "select")) {
				/* what GNUstep's run loop does on this port (no HAVE_POLL_F) */
				fd_set rf;
				struct timeval tv = { 0, 20000 };
				int mx = 0;
				FD_ZERO(&rf);
				for (int i = 0; i < ns; i++) {
					FD_SET(set[i].fd, &rf);
					if (set[i].fd > mx) mx = set[i].fd;
				}
				if (select(mx + 1, &rf, NULL, NULL, &tv) > 0 && FD_ISSET(set[0].fd, &rf))
					set[0].revents = POLLIN;
			} else {
				poll(set, ns, 20);
			}
			if (set[0].revents & POLLIN) {
				ssize_t r = read(fd, b, sizeof b);
				if (r > 0 && pending) {
					double l = now_ms() - sent;
					sum += l;
					if (l > worst) worst = l;
					if (l > 50) { over++; printf("  #%d %.1fms\n", done, l); }
					pending = 0;
					done++;
				}
			}
		}
		printf("pty loop: n=%d avg=%.2fms max=%.2fms over50ms=%d\n", n, sum / n, worst, over);
		kill(pid, SIGKILL);
		return 0;
	}
	for (int i = 0; i < n; i++) {
		char c = 'a' + i % 26;
		double t0 = now_ms();
		write(fd, &c, 1);
		int got = 0;
		while (now_ms() - t0 < 2000) {
			if (poll(&p, 1, 2000) > 0 && read(fd, b, sizeof b) > 0) {
				got = 1;
				break;
			}
		}
		lat[i] = got ? now_ms() - t0 : -1;
		if (!got)
			lost++;
		double wait = period - (now_ms() - t0);
		if (wait > 0)
			usleep((useconds_t)(wait * 1000));
	}
	double sum = 0, max = 0;
	int slow = 0, ok = 0;
	for (int i = 0; i < n; i++) {
		if (lat[i] < 0)
			continue;
		ok++;
		sum += lat[i];
		if (lat[i] > max)
			max = lat[i];
		if (lat[i] > 50)
			slow++;
	}
	printf("pty echo: n=%d lost=%d avg=%.2fms max=%.2fms over50ms=%d\n", n, lost, ok ? sum / ok : 0, max, slow);
	for (int i = 0; i < n; i++)
		if (lat[i] > 50)
			printf("  #%d %.1fms\n", i, lat[i]);
	kill(pid, SIGKILL);
	return 0;
}
