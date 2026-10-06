/*
 * rome: the terminal core. A pty running a shell, libvterm's VT220/xterm
 * state and screen, the scrollback, damage tracking, selection, and the
 * frame loop that hands changed cells to a RomeRenderer. Plain C; the AppKit
 * side (RomeView.m) owns the windows, the run loop and input.
 */
#ifndef ROME_TERM_H
#define ROME_TERM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "RomeRender.h"

typedef struct RomeTerm RomeTerm;

typedef struct {
	void (*title)(void *owner, const char *utf8);
	void (*bell)(void *owner);
	/* the pty's write side is full: call rome_term_write_pending() when
	 * the fd is writable */
	void (*want_write)(void *owner);
} RomeTermCallbacks;

typedef struct {
	uint32_t fg, bg, cursor, selection;
	uint32_t palette[16];
} RomeTheme;

RomeTerm *rome_term_new(int rows, int cols, int scrollback, const RomeTheme *theme,
    const RomeTermCallbacks *cb, void *owner);
void rome_term_free(RomeTerm *t);

/* Start `argv` (argv[0] is the path; NULL = the user's login shell) on a new
 * pty. Returns the master fd (non-blocking) or -1. */
int rome_term_spawn(RomeTerm *t, char *const argv[]);
pid_t rome_term_pid(RomeTerm *t);
int rome_term_fd(RomeTerm *t);

/* Read what the pty has (up to `budget` bytes) into the emulator. Returns
 * the bytes read, 0 when nothing was there, -1 when the child is gone. */
long rome_term_read(RomeTerm *t, long budget);
/* Feed bytes straight into the emulator (tests, the exit message). */
void rome_term_feed(RomeTerm *t, const char *bytes, size_t len);
/* Write pending output (keyboard, paste) to the pty. Returns 1 when some
 * is still pending. */
int rome_term_write_pending(RomeTerm *t);

/* Keyboard: a Unicode character or a libvterm key, with VTERM_MOD_* bits. */
void rome_term_key_char(RomeTerm *t, uint32_t c, int mods);
void rome_term_key(RomeTerm *t, int vtermkey, int mods);
void rome_term_paste(RomeTerm *t, const char *utf8, size_t len);
void rome_term_send_raw(RomeTerm *t, const char *bytes, size_t len);
/* Mouse reporting to the application; returns 0 when it has not asked. */
int rome_term_mouse_mode(RomeTerm *t);
void rome_term_mouse(RomeTerm *t, int row, int col, int button, int pressed, int mods);
int rome_term_altscreen(RomeTerm *t);

int rome_term_rows(RomeTerm *t);
int rome_term_cols(RomeTerm *t);
/* New grid size: libvterm reflows, the pty gets TIOCSWINSZ (SIGWINCH). */
void rome_term_resize(RomeTerm *t, int rows, int cols, int xpixels, int ypixels);

/* Scrollback view: lines scrolled back from the bottom. */
void rome_term_scroll_view(RomeTerm *t, int delta);
void rome_term_scroll_to_bottom(RomeTerm *t);
int rome_term_view_offset(RomeTerm *t);
int rome_term_scrollback_lines(RomeTerm *t);

/* Selection in grid coordinates of the visible rows. */
void rome_term_select(RomeTerm *t, int row0, int col0, int row1, int col1);
void rome_term_select_word(RomeTerm *t, int row, int col);
void rome_term_select_line(RomeTerm *t, int row);
void rome_term_select_all(RomeTerm *t);
void rome_term_select_clear(RomeTerm *t);
int rome_term_has_selection(RomeTerm *t);
/* The selected text as UTF-8 (malloc'd, NUL-terminated), or NULL. */
char *rome_term_selection_text(RomeTerm *t);

/* Cursor display state. */
void rome_term_set_focus(RomeTerm *t, int focused);
void rome_term_set_cursor_phase(RomeTerm *t, int on);
int rome_term_cursor_blinks(RomeTerm *t);

/* Frames. */
int rome_term_needs_render(RomeTerm *t);
void rome_term_expose(RomeTerm *t, int x, int y, int w, int h);
void rome_term_damage_all(RomeTerm *t);
/* Draw what changed and put it on the screen. Returns the rows drawn. */
int rome_term_render(RomeTerm *t, RomeRenderer *r);

#endif
