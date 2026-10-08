/*
 * xlatency: drive and time a window from outside, through the X server, so any
 * terminal can be measured the same way.
 *
 *   xlatency now                      monotonic milliseconds (to time shell commands)
 *   xlatency pids                     list top-level windows with their _NET_WM_PID
 *   xlatency type PID TEXT...         focus PID's window, type each TEXT then Return
 *   xlatency ctrld PID                send Control-D (end of input) to PID's window
 *   xlatency latency PID N GAP_MS     key-to-pixels: focus PID's window, type a letter N
 *                                     times GAP_MS apart, time each until the top rows of
 *                                     the window change; prints min/avg/p50/p95/max in ms
 *   xlatency waitwin PID [MS]        ms until PID has a viewable window
 *   xlatency floor PID                the cost of the probe itself (XGetImage of the same
 *                                     region, nothing changing)
 *
 * Latency includes the XTEST injection, the server, the application's event
 * handling and drawing, and one XGetImage round trip (see `floor`), the same for
 * every application. $DISPLAY and $XAUTHORITY as for any X client.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

static Display *dpy;
static Atom a_pid;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static long window_pid(Window w)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	long pid = -1;
	if (XGetWindowProperty(dpy, w, a_pid, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data) {
		if (n == 1 && fmt == 32)
			pid = *(long *)data;
		XFree(data);
	}
	return pid;
}

static Window leader_of(Window w)
{
	Atom type, a = XInternAtom(dpy, "WM_CLIENT_LEADER", False);
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	Window l = 0;
	if (XGetWindowProperty(dpy, w, a, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after, &data) == Success && data) {
		if (n == 1 && fmt == 32)
			l = *(Window *)data;
		XFree(data);
	}
	if (!l) {
		XWMHints *h = XGetWMHints(dpy, w);
		if (h) {
			if (h->flags & WindowGroupHint)
				l = h->window_group;
			XFree(h);
		}
	}
	return l;
}

static int viewable_window(Window w)
{
	XWindowAttributes at;
	return XGetWindowAttributes(dpy, w, &at) && at.map_state == IsViewable && at.width >= 100 && at.height >= 60;
}

/* depth-limited search.  An application's pid is on its hidden group-leader window; the window
 * wanted is the mapped, window-sized one whose WM_CLIENT_LEADER (or group hint) is that leader. */
static Window find_leader(Window w, long pid, int depth)
{
	Window root, parent, *kids = NULL, found = 0;
	unsigned int n = 0;
	if (window_pid(w) == pid)
		return w;
	if (depth == 0 || !XQueryTree(dpy, w, &root, &parent, &kids, &n))
		return 0;
	for (unsigned int i = 0; i < n && !found; i++)
		found = find_leader(kids[i], pid, depth - 1);
	if (kids)
		XFree(kids);
	return found;
}

static Window find_client(Window w, Window leader, int depth)
{
	Window root, parent, *kids = NULL, found = 0;
	unsigned int n = 0;
	if (w != leader && viewable_window(w) && leader_of(w) == leader)
		return w;
	if (depth == 0 || !XQueryTree(dpy, w, &root, &parent, &kids, &n))
		return 0;
	for (unsigned int i = 0; i < n && !found; i++)
		found = find_client(kids[i], leader, depth - 1);
	if (kids)
		XFree(kids);
	return found;
}

static void list_windows(Window w, int depth)
{
	Window root, parent, *kids = NULL;
	unsigned int n = 0;
	long p = window_pid(w);
	if (p >= 0)
		printf("window 0x%lx pid %ld (leader window)\n", w, p);
	else if (viewable_window(w) && leader_of(w))
		printf("window 0x%lx leader 0x%lx\n", w, leader_of(w));
	if (depth == 0 || !XQueryTree(dpy, w, &root, &parent, &kids, &n))
		return;
	for (unsigned int i = 0; i < n; i++)
		list_windows(kids[i], depth - 1);
	if (kids)
		XFree(kids);
}

static Window find_pid(Window root, long pid, int depth, int list)
{
	Window leader;
	(void)list;
	leader = find_leader(root, pid, depth);
	return leader ? find_client(root, leader, depth) : 0;
}

static void key(KeySym sym)
{
	KeyCode kc = XKeysymToKeycode(dpy, sym);
	int shift = 0;
	if (kc == 0)
		return;
	if (XkbKeycodeToKeysym(dpy, kc, 0, 0) != sym && XkbKeycodeToKeysym(dpy, kc, 0, 1) == sym)
		shift = 1;
	if (shift)
		XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), True, 0);
	XTestFakeKeyEvent(dpy, kc, True, 0);
	XTestFakeKeyEvent(dpy, kc, False, 0);
	if (shift)
		XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), False, 0);
}

static void type_string(const char *s)
{
	for (; *s; s++) {
		key((KeySym)(unsigned char)*s);     /* Latin-1 keysyms are the character codes */
		XFlush(dpy);
		usleep(15000);
	}
}

static uint64_t grab_hash(Window w, int x, int y, int cw, int ch)
{
	XImage *im = XGetImage(dpy, w, x, y, cw, ch, AllPlanes, ZPixmap);
	uint64_t h = 1469598103934665603ULL;
	if (!im)
		return 0;
	for (int row = 0; row < ch; row++) {
		const unsigned char *p = (const unsigned char *)im->data + (size_t)row * im->bytes_per_line;
		for (int i = 0; i < cw * 4; i++)
			h = (h ^ p[i]) * 1099511628211ULL;
	}
	XDestroyImage(im);
	return h;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static void focus(Window w)
{
	XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
	XSync(dpy, False);
	usleep(100000);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: see the header of xlatency.c\n");
		return 2;
	}
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "xlatency: cannot open the display\n");
		return 1;
	}
	a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
	Window root = DefaultRootWindow(dpy);
	if (!strcmp(argv[1], "now")) {          /* monotonic ms, for timing shell commands */
		printf("%.0f\n", now_ms());
		return 0;
	}
	if (!strcmp(argv[1], "pids")) {
		list_windows(root, 4);
		return 0;
	}
	if (argc < 3)
		return 2;
	long pid = atol(argv[2]);
	if (!strcmp(argv[1], "waitwin")) {
		/* ms until PID has a viewable window (start this right after launching it) */
		double t0 = now_ms(), limit = argc > 3 ? atof(argv[3]) : 20000;
		while (now_ms() - t0 < limit) {
			Window f = find_pid(root, pid, 4, 0);
			XWindowAttributes at;
			if (f && XGetWindowAttributes(dpy, f, &at) && at.map_state == IsViewable) {
				printf("window after %.0f ms\n", now_ms() - t0);
				return 0;
			}
			usleep(2000);
		}
		printf("no window after %.0f ms\n", limit);
		return 1;
	}
	Window w = find_pid(root, pid, 4, 0);
	if (!w) {
		fprintf(stderr, "xlatency: no window for pid %ld\n", pid);
		return 1;
	}
	if (!strcmp(argv[1], "ctrld")) {
		focus(w);
		XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_L), True, 0);
		key(XK_d);
		XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_L), False, 0);
		XFlush(dpy);
		return 0;
	}
	if (!strcmp(argv[1], "type")) {
		focus(w);
		for (int i = 3; i < argc; i++) {
			type_string(argv[i]);
			key(XK_Return);
			XFlush(dpy);
			usleep(100000);
		}
		return 0;
	}
	/* the region compared: the top rows of the window, where `cat`'s echo lands */
	Window r;
	int gx, gy;
	unsigned int gw, gh, bw, depth;
	XGetGeometry(dpy, w, &r, &gx, &gy, &gw, &gh, &bw, &depth);
	int cw = gw < 420 ? (int)gw : 420, ch = gh < 26 ? (int)gh : 26;
	if (getenv("XLAT_DEBUG")) {
		Window ch_root, ch_parent, *kids = NULL;
		unsigned int nk = 0;
		fprintf(stderr, "window 0x%lx %ux%u at %d,%d; ", w, gw, gh, gx, gy);
		if (XQueryTree(dpy, w, &ch_root, &ch_parent, &kids, &nk)) {
			for (unsigned int i = 0; i < nk; i++) {
				XWindowAttributes a;
				XGetWindowAttributes(dpy, kids[i], &a);
				fprintf(stderr, "child 0x%lx %dx%d at %d,%d map=%d; ", kids[i], a.width, a.height, a.x, a.y, a.map_state);
			}
			if (kids) XFree(kids);
		}
		fprintf(stderr, "\n");
	}
	if (!strcmp(argv[1], "floor")) {
		enum { N = 300 };
		double v[N];
		for (int i = 0; i < N; i++) {
			double t = now_ms();
			grab_hash(w, 0, 0, cw, ch);
			v[i] = now_ms() - t;
		}
		qsort(v, N, sizeof v[0], cmp);
		printf("probe floor (XGetImage %dx%d): p50=%.2f p95=%.2f max=%.2f ms\n", cw, ch, v[N / 2], v[N * 95 / 100], v[N - 1]);
		return 0;
	}
	if (!strcmp(argv[1], "latency")) {
		int n = argc > 3 ? atoi(argv[3]) : 40, gap = argc > 4 ? atoi(argv[4]) : 250;
		double *v = calloc(n, sizeof *v);
		int ok = 0, timeouts = 0;
		focus(w);
		/* is the strip stable (no blinking cursor, clock...)? */
		uint64_t h0 = grab_hash(w, 0, 0, cw, ch);
		int unstable = 0;
		for (int i = 0; i < 100; i++) {
			usleep(10000);
			if (grab_hash(w, 0, 0, cw, ch) != h0)
				unstable++, h0 = grab_hash(w, 0, 0, cw, ch);
		}
		if (unstable)
			fprintf(stderr, "xlatency: warning, the region changed %d times with no input\n", unstable);
		for (int i = 0; i < n; i++) {
			static const char *letters = "abcdefghijklmnopqrstuvwxyz";
			uint64_t base = grab_hash(w, 0, 0, cw, ch);
			double t0 = now_ms(), t1 = 0;
			key(XStringToKeysym((char[]){ letters[i % 26], 0 }));
			XFlush(dpy);
			while ((t1 = now_ms()) - t0 < 1000)
				if (grab_hash(w, 0, 0, cw, ch) != base)
					break;
			if (t1 - t0 >= 1000)
				timeouts++;
			else
				v[ok++] = t1 - t0;
			usleep(gap * 1000);
		}
		if (!ok) {
			printf("latency: no key ever changed the window (%d timeouts)\n", timeouts);
			return 1;
		}
		qsort(v, ok, sizeof v[0], cmp);
		double sum = 0;
		for (int i = 0; i < ok; i++)
			sum += v[i];
		printf("key->pixels n=%d timeouts=%d min=%.1f avg=%.1f p50=%.1f p95=%.1f max=%.1f ms\n", ok, timeouts, v[0], sum / ok,
		    v[ok / 2], v[ok * 95 / 100], v[ok - 1]);
		return 0;
	}
	return 2;
}
