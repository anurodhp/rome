#include "../RomeTerm.c"
#include <assert.h>

#define R 6
#define C 20
static RomeCell grid[R][C];
static int draws, last_x0, last_x1, cleared, presented;
static RomeFont font = { .cell_w = 8, .cell_h = 16 };

static int s_resize(RomeRenderer *r, int w, int h) { r->width = w; r->height = h; return 1; }
static void s_begin(RomeRenderer *r) { (void)r; }
static void s_clear(RomeRenderer *r, int c, int ro) { (void)r; (void)c; (void)ro; cleared++; }
static void s_draw(RomeRenderer *r, int row, int x0, int x1, const RomeCell *cells) {
	(void)r; draws++; last_x0 = x0; last_x1 = x1; if (getenv("TRACE")) printf("  draw row %d [%d,%d)\n", row, x0, x1);
	for (int i = x0; i < x1; i++) grid[row][i] = cells[i];
}
static int s_scroll(RomeRenderer *r, int a, int b, int dy) { (void)r; (void)a; (void)b; (void)dy; return 0; }
static void s_present(RomeRenderer *r, const RomeRect *rc, int n) { (void)r; (void)rc; presented += n; }
static void s_destroy(RomeRenderer *r) { (void)r; }

static void dump(const char *label) {
	printf("--- %s\n", label);
	for (int y = 0; y < R; y++) {
		printf("|");
		for (int x = 0; x < C; x++) {
			RomeCell *c = &grid[y][x];
			if (c->width == 0) { printf("_"); continue; }
			if (c->ch < 128 && c->ch >= 32) printf("%c", c->ch);
			else if (c->ch == 0) printf(" ");
			else printf("?");
		}
		printf("|\n");
	}
}

static void feed(RomeTerm *t, const char *s) { rome_term_feed(t, s, strlen(s)); }
static void out_dump(RomeTerm *t, const char *label) {
	printf("%s out(%zu):", label, t->out_len);
	for (size_t i = 0; i < t->out_len; i++) printf(" %02x", (unsigned char)t->out[i]);
	printf("\n");
	t->out_len = 0;
}
static int render(RomeTerm *t, RomeRenderer *r) { draws = 0; presented = 0; return rome_term_render(t, r); }

static void title_cb(void *o, const char *s) { (void)o; printf("TITLE: %s\n", s); }
static void bell_cb(void *o) { (void)o; printf("BELL\n"); }

int main(void) {
	RomeTheme th = { .fg = 0xffffff, .bg = 0x000000, .cursor = 0x4d4d4d, .selection = 0x414f78,
	  .palette = { 0x000000, 0xcc3333, 0x00a600, 0x999900, 0x4a6bff, 0xb200b2, 0x00a6b2, 0xbfbfbf,
	               0x666666, 0xe50000, 0x00d900, 0xe5e500, 0x6b8cff, 0xe500e5, 0x00e5e5, 0xffffff } };
	RomeTermCallbacks cb = { title_cb, bell_cb, NULL };
	RomeTerm *t = rome_term_new(R, C, 1000, &th, &cb, NULL);
	assert(t);
	RomeRenderer r = { .name = "stub", .font = &font, .resize = s_resize, .begin = s_begin, .clear = s_clear,
	                   .draw_row = s_draw, .scroll = s_scroll, .present = s_present, .destroy = s_destroy };
	r.width = C * 8; r.height = R * 16;
	rome_term_resize(t, R, C, C * 8, R * 16);

	feed(t, "Hello \033[1;31mred\033[0m\r\nline2 \033[7mINV\033[0m w\r\n\xe6\x97\xa5\xe6\x9c\xac" "x\033]2;mytitle\007\007");
	printf("needs_render=%d\n", rome_term_needs_render(t));
	int d = render(t, &r);
	printf("first frame: drawn rows=%d cleared=%d presented=%d\n", d, cleared, presented);
	dump("after feed");
	printf("red cell fg=%06x bold=%d  INV fg=%06x bg=%06x  wide width=%d tail width=%d\n",
	    grid[0][6].fg, !!(grid[0][6].attrs & ROME_ATTR_BOLD), grid[1][6].fg, grid[1][6].bg, grid[2][0].width, grid[2][1].width);
	printf("cursor cell bg=%06x (cursor %06x)\n", grid[2][3].bg, th.cursor);

	/* incremental */
	printf("needs_render after render=%d\n", rome_term_needs_render(t));
	feed(t, "Z");
	d = render(t, &r);
	printf("one char: drawn rows=%d span=[%d,%d) presented rects=%d\n", d, last_x0, last_x1, presented);

	/* idle cursor blink */
	rome_term_set_cursor_phase(t, 0);
	d = render(t, &r);
	printf("blink off: drawn rows=%d span=[%d,%d)\n", d, last_x0, last_x1);
	rome_term_set_cursor_phase(t, 1);
	render(t, &r);

	/* scrollback */
	for (int i = 0; i < 40; i++) { char b[32]; snprintf(b, sizeof b, "row %d\r\n", i); feed(t, b); }
	render(t, &r);
	dump("after 40 lines");
	printf("scrollback=%d view_offset=%d alt=%d\n", rome_term_scrollback_lines(t), rome_term_view_offset(t), rome_term_altscreen(t));
	rome_term_scroll_view(t, 10);
	d = render(t, &r);
	printf("scrolled 10 back: view_offset=%d drawn rows=%d\n", rome_term_view_offset(t), d);
	dump("scrolled");
	rome_term_scroll_view(t, -3);
	printf("scrolled forward 3: view_offset=%d\n", rome_term_view_offset(t));
	rome_term_scroll_to_bottom(t);
	printf("bottom: view_offset=%d\n", rome_term_view_offset(t));
	render(t, &r);

	/* selection */
	rome_term_select_begin(t, 1, 0, 1);
	rome_term_select_extend(t, 2, 3);
	printf("has_sel=%d\n", rome_term_has_selection(t));
	d = render(t, &r);
	printf("selection render: drawn rows=%d sel bg at (2,0)=%06x\n", d, grid[2][0].bg);
	char *s = rome_term_selection_text(t);
	printf("selection text: [%s]\n", s ? s : "(null)");
	free(s);
	rome_term_select_begin(t, 0, 2, 2);
	s = rome_term_selection_text(t);
	printf("word select: [%s]\n", s ? s : "(null)");
	free(s);
	rome_term_select_begin(t, 3, 2, 3);
	s = rome_term_selection_text(t);
	printf("line select: [%s]\n", s ? s : "(null)");
	free(s);
	rome_term_select_clear(t);
	printf("has_sel after clear=%d\n", rome_term_has_selection(t));
	render(t, &r);

	/* keys */
	t->out_len = 0;
	rome_term_key(t, ROME_KEY_UP, 0); out_dump(t, "UP");
	rome_term_key(t, ROME_KEY_ENTER, 0); out_dump(t, "ENTER");
	rome_term_key(t, ROME_KEY_BACKSPACE, 0); out_dump(t, "BKSP");
	rome_term_key(t, ROME_KEY_TAB, 0); out_dump(t, "TAB");
	rome_term_key(t, ROME_KEY_TAB, ROME_MOD_SHIFT); out_dump(t, "S-TAB");
	rome_term_key(t, ROME_KEY_ESCAPE, 0); out_dump(t, "ESC");
	rome_term_key(t, ROME_KEY_F(1), 0); out_dump(t, "F1");
	rome_term_key(t, ROME_KEY_F(5), ROME_MOD_CTRL); out_dump(t, "C-F5");
	rome_term_key(t, ROME_KEY_HOME, 0); out_dump(t, "HOME");
	rome_term_key(t, ROME_KEY_DEL, 0); out_dump(t, "DEL");
	rome_term_key(t, ROME_KEY_PAGEUP, 0); out_dump(t, "PGUP");
	rome_term_key(t, ROME_KEY_LEFT, ROME_MOD_CTRL); out_dump(t, "C-LEFT");
	feed(t, "\033[?1h");
	rome_term_key(t, ROME_KEY_UP, 0); out_dump(t, "UP (app cursor)");
	feed(t, "\033[?1l");
	rome_term_key_char(t, 'c', ROME_MOD_CTRL); out_dump(t, "C-c");
	rome_term_key_char(t, 'x', ROME_MOD_ALT); out_dump(t, "M-x");
	rome_term_key_char(t, 0x20ac, 0); out_dump(t, "euro");

	/* replies */
	feed(t, "\033[c"); out_dump(t, "DA1");
	feed(t, "\033[>c"); out_dump(t, "DA2");
	feed(t, "\033[6n"); out_dump(t, "CPR");
	feed(t, "\033[5n"); out_dump(t, "DSR");

	/* paste */
	rome_term_paste(t, "a\nb", 3); out_dump(t, "paste plain");
	feed(t, "\033[?2004h");
	rome_term_paste(t, "a\nb", 3); out_dump(t, "paste bracketed");
	feed(t, "\033[?2004l");

	/* mouse */
	printf("mouse mode=%d\n", rome_term_mouse_mode(t));
	feed(t, "\033[?1000h\033[?1006h");
	printf("mouse mode=%d\n", rome_term_mouse_mode(t));
	rome_term_mouse(t, 2, 3, 1, 1, 0); out_dump(t, "mouse press");
	rome_term_mouse(t, 2, 3, 1, 0, 0); out_dump(t, "mouse release");
	rome_term_mouse(t, 2, 3, 4, 1, 0); out_dump(t, "wheel up");
	feed(t, "\033[?1002h");
	printf("mouse mode=%d\n", rome_term_mouse_mode(t));
	rome_term_mouse(t, 2, 3, 1, 1, 0); out_dump(t, "press");
	rome_term_mouse(t, 2, 4, 0, 0, 0); out_dump(t, "drag");
	rome_term_mouse(t, 2, 4, 1, 0, 0); out_dump(t, "release");
	feed(t, "\033[?1002l\033[?1000l");

	/* alt screen */
	feed(t, "\033[?1049h" "ALT SCREEN\033[1;1H");
	render(t, &r);
	dump("alt screen");
	printf("alt=%d scrollback=%d\n", rome_term_altscreen(t), rome_term_scrollback_lines(t));
	feed(t, "\033[?1049l");
	render(t, &r);
	dump("back");

	/* resize */
	rome_term_resize(t, 4, 12, 12 * 8, 4 * 16);
	printf("resized: %dx%d full=%d\n", rome_term_cols(t), rome_term_rows(t), t->full);
	d = render(t, &r);
	printf("after resize render: drawn=%d\n", d);

	/* bell + title done above */
	rome_term_free(t);
	printf("OK\n");
	return 0;
}
