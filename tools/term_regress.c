/* Regression tests for bugs an adversarial review found in the terminal core. Exits non-zero on the
 * first failure.  Run by tools/term_test.sh (after the printed walkthrough). */
#include "../RomeTerm.c"
#include <assert.h>

#define R 8
#define C 40
static RomeCell grid[R][C];
static RomeFont font = { .cell_w = 8, .cell_h = 16 };
static int s_resize(RomeRenderer *r, int w, int h) { r->width = w; r->height = h; return 1; }
static void s_begin(RomeRenderer *r) { (void)r; }
static void s_clear(RomeRenderer *r, int c, int ro) { (void)r; (void)c; (void)ro; }
static void s_draw(RomeRenderer *r, int row, int x0, int x1, const RomeCell *cells)
{
	(void)r;
	for (int i = x0; i < x1; i++) grid[row][i] = cells[i];
}
static int s_scroll(RomeRenderer *r, int a, int b, int d) { (void)r; (void)a; (void)b; (void)d; return 0; }
static void s_present(RomeRenderer *r, const RomeRect *rc, int n) { (void)r; (void)rc; (void)n; }
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static RomeTheme theme = { .fg = 0xffffff, .bg = 0x000000, .cursor = 0x4d4d4d, .selection = 0x414f78 };
static RomeRenderer rnd = { .name = "stub", .font = &font, .resize = s_resize, .begin = s_begin, .clear = s_clear,
    .draw_row = s_draw, .scroll = s_scroll, .present = s_present };

static RomeTerm *fresh(int rows, int cols, int sb)
{
	RomeTerm *t = rome_term_new(rows, cols, sb, &theme, NULL, NULL);
	rnd.width = cols * 8;
	rnd.height = rows * 16;
	rome_term_resize(t, rows, cols, cols * 8, rows * 16);
	memset(grid, 0, sizeof grid);
	return t;
}
static void feed(RomeTerm *t, const char *s) { rome_term_feed(t, s, strlen(s)); }
static char *out_take(RomeTerm *t)
{
	static char buf[4096];
	size_t n = t->out_len - t->out_off;
	if (n > sizeof buf - 1) n = sizeof buf - 1;
	memcpy(buf, t->out + t->out_off, n);
	buf[n] = 0;
	t->out_len = t->out_off = 0;
	return buf;
}

int main(void)
{
	char *s;
	RomeTerm *t;

	/* a soft-wrapped line copies as one line */
	t = fresh(6, 20, 100);
	feed(t, "0123456789ABCDEFGHIJKLMNOPQRST");
	rome_term_render(t, &rnd);
	rome_term_select_begin(t, 0, 0, 3);
	s = rome_term_selection_text(t);
	CHECK(s && strchr(s, '\n') == NULL, "wrapped line copied with a newline: [%s]", s ? s : "(null)");
	free(s);
	rome_term_free(t);

	/* double-click then drag backwards selects the words in between, not the gap */
	t = fresh(6, 40, 100);
	feed(t, "git status --short now");
	rome_term_render(t, &rnd);
	rome_term_select_begin(t, 0, 14, 2);        /* in "short" */
	rome_term_select_extend(t, 0, 1);           /* back onto "git" */
	s = rome_term_selection_text(t);
	CHECK(s && !strcmp(s, "git status --short"), "backward word drag: [%s]", s ? s : "(null)");
	free(s);
	rome_term_free(t);

	/* a double-click on blank cells leaves no selection behind */
	t = fresh(6, 40, 100);
	feed(t, "word");
	rome_term_render(t, &rnd);
	rome_term_select_begin(t, 0, 1, 2);
	CHECK(rome_term_has_selection(t), "word not selected");
	rome_term_select_begin(t, 3, 5, 2);
	CHECK(!rome_term_has_selection(t), "old selection kept after a blank double-click");
	rome_term_free(t);

	/* the terminal changing its own size (DECCOLM) is put back */
	t = fresh(6, 40, 100);
	feed(t, "\033[?40h\033[?3h");
	rome_term_render(t, &rnd);
	{
		uint16_t c = 0;
		ghostty_terminal_get(t->gt, GHOSTTY_TERMINAL_DATA_COLS, &c);
		CHECK(c == 40, "terminal is %d columns after DECCOLM, Rome has 40", c);
	}
	rome_term_free(t);

	/* a default-colour change repaints what is already on screen */
	t = fresh(6, 40, 100);
	feed(t, "hello");
	rome_term_render(t, &rnd);
	feed(t, "\033]11;#ff0000\007");
	rome_term_render(t, &rnd);
	CHECK(grid[3][20].bg == 0xff0000, "background after OSC 11 is %06x, not ff0000", grid[3][20].bg);
	feed(t, "\033]111\007");
	rome_term_render(t, &rnd);
	CHECK(grid[3][20].bg == 0x000000, "background after OSC 111 is %06x", grid[3][20].bg);
	rome_term_free(t);

	/* replies to a program that never reads are bounded */
	t = fresh(6, 40, 100);
	for (int i = 0; i < 20000; i++)
		feed(t, "\033[c\033[c\033[c\033[c\033[c");
	CHECK(t->out_len - t->out_off < 70000, "reply buffer grew to %zu bytes", t->out_len - t->out_off);
	/* ... but typed input is not dropped even then */
	rome_term_key_char(t, 'x', 0);
	CHECK(t->out[t->out_len - 1] == 'x', "typed input lost while the reply buffer was full");
	rome_term_free(t);

	/* Kitty keyboard protocol: text keys are reported as key events */
	t = fresh(6, 40, 100);
	feed(t, "\033[>1u");                          /* disambiguate */
	out_take(t);
	rome_term_key_char(t, 'c', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[99;5u"), "Ctrl-C under the Kitty protocol is [%s]", s);
	rome_term_key_char(t, 'a', ROME_MOD_ALT);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[97;3u"), "Alt-a under the Kitty protocol is [%s]", s);
	feed(t, "\033[<u");                           /* pop it: plain bytes again */
	out_take(t);
	rome_term_key_char(t, 'c', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(s[0] == 3 && s[1] == 0, "Ctrl-C without the protocol is not 0x03");
	rome_term_key_char(t, '/', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(s[0] == 0x1f, "Ctrl-/ is %02x", s[0]);
	rome_term_key_char(t, 0xd800, 0);             /* a lone surrogate sends nothing */
	CHECK(t->out_len == 0, "lone surrogate was sent");
	rome_term_free(t);

	/* mouse motion that does not leave its cell is not reported again */
	t = fresh(6, 40, 100);
	feed(t, "\033[?1002h\033[?1006h");
	rome_term_mouse(t, 2, 3, 1, 1, 0);
	out_take(t);
	rome_term_mouse(t, 2, 4, 0, 0, 0);
	CHECK(t->out_len > 0, "first drag into a new cell not reported");
	out_take(t);
	rome_term_mouse(t, 2, 4, 0, 0, 0);
	rome_term_mouse(t, 2, 4, 0, 0, 0);
	CHECK(t->out_len == 0, "repeated motion within one cell reported %zu bytes", t->out_len);
	rome_term_free(t);

	/* a closed tab's shell is collected, not left as a zombie */
	t = fresh(6, 40, 100);
	{
		char *argv[] = { "/bin/sleep", "30", NULL };
		pid_t pid;
		int st;
		rome_term_spawn(t, argv);
		pid = rome_term_pid(t);
		usleep(100000);
		rome_term_free(t);
		for (int i = 0; i < 50; i++) {
			rome_term_reap_orphans();
			if (waitpid(pid, &st, WNOHANG) < 0 && errno == ECHILD)
				break;
			usleep(20000);
		}
		CHECK(kill(pid, 0) < 0, "the shell of a closed terminal is still there (zombie)");
	}

	/* combining marks reach the cell; joiners and ZWJ emoji do not */
	t = fresh(6, 40, 100);
	feed(t, "e\xcc\x81" "x\r\n");                                 /* e + U+0301, x */
	feed(t, "\xe0\xb8\x81\xe0\xb8\xb4\xe0\xb8\xb8\r\n");           /* Thai: ko kai + sara i + sara u */
	feed(t, "\033[?2027h\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\r\n");   /* man ZWJ woman, clustered */
	rome_term_render(t, &rnd);
	CHECK(grid[0][0].ch == 'e' && grid[0][0].mark[0] == 0x301, "e + U+0301: ch %x mark %x", grid[0][0].ch, grid[0][0].mark[0]);
	CHECK(grid[0][1].ch == 'x' && grid[0][1].mark[0] == 0, "the x after it was disturbed");
	CHECK(grid[1][0].ch == 0xe01 && grid[1][0].mark[0] == 0xe34 && grid[1][0].mark[1] == 0xe38, "Thai marks: %x %x", grid[1][0].mark[0], grid[1][0].mark[1]);
	CHECK(grid[2][0].ch == 0x1f468 && grid[2][0].mark[0] == 0, "ZWJ sequence draws extra glyphs: mark %x", grid[2][0].mark[0]);
	rome_term_free(t);

	/* key releases and repeats are sent under the Kitty protocol's event-type flag, and only then */
	t = fresh(6, 40, 100);
	rome_term_key_event(t, ROME_KEY_RELEASE, ROME_KEY_NONE, 'c', ROME_MOD_CTRL);
	rome_term_key_event(t, ROME_KEY_RELEASE, ROME_KEY_UP, 0, 0);
	CHECK(t->out_len == 0, "a release without the Kitty protocol sent %zu bytes", t->out_len);
	rome_term_key_event(t, ROME_KEY_REPEAT, ROME_KEY_NONE, 'x', 0);
	s = out_take(t);
	CHECK(!strcmp(s, "x"), "a repeat without the protocol is [%s], not a press", s);
	feed(t, "\033[>3u");                         /* disambiguate + report event types */
	out_take(t);
	rome_term_key_event(t, ROME_KEY_PRESS, ROME_KEY_NONE, 'c', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[99;5u"), "Ctrl-C press under event types: [%s]", s);
	rome_term_key_event(t, ROME_KEY_RELEASE, ROME_KEY_NONE, 'c', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[99;5:3u"), "Ctrl-C release under event types: [%s]", s);
	rome_term_key_event(t, ROME_KEY_RELEASE, ROME_KEY_UP, 0, 0);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[1;1:3A"), "Up release under event types: [%s]", s);
	rome_term_key_event(t, ROME_KEY_REPEAT, ROME_KEY_NONE, 'c', ROME_MOD_CTRL);
	s = out_take(t);
	CHECK(!strcmp(s, "\033[99;5:2u"), "Ctrl-C repeat under event types: [%s]", s);
	rome_term_free(t);

	/* a paste that would run lines asks first, unless the program asked for bracketed pastes */
	t = fresh(6, 40, 100);
	CHECK(rome_term_paste_needs_confirm(t, "a\nb", 3), "multi-line paste not flagged");
	CHECK(!rome_term_paste_needs_confirm(t, "ab", 2), "one-line paste flagged");
	feed(t, "\033[?2004h");
	CHECK(!rome_term_paste_needs_confirm(t, "a\nb", 3), "bracketed paste flagged");
	rome_term_free(t);

	printf(failures ? "%d regression test(s) FAILED\n" : "all regression tests passed\n", failures);
	return failures != 0;
}
