/*
 * rome: monospaced font and glyph atlas.
 *
 * Fonts come from fontconfig and are rasterised with FreeType into one
 * 8-bit coverage atlas. Every atlas slot is two cells wide and one cell
 * high, and a glyph is rasterised into its slot at the cell's baseline and
 * clipped to the cell, so a glyph is always drawn as exactly one cell (two
 * for a wide character): the GL renderer draws one quad per glyph, the X11
 * renderer blends one cell-sized block, and nothing overlaps a neighbour.
 * Slot 0 is solid (coverage 255) for the GL renderer's background quads.
 */
#ifndef ROME_FONT_H
#define ROME_FONT_H

#include <stdint.h>

enum {
	ROME_STYLE_REGULAR = 0,
	ROME_STYLE_BOLD = 1,
	ROME_STYLE_ITALIC = 2,
	ROME_STYLE_BOLD_ITALIC = 3,
	/* or'd into a style for rome_font_glyph(): the code point is a combining mark, drawn in the
	 * cell of the character it combines with (zero-advance glyphs are placed back over it) */
	ROME_STYLE_MARK = 4,
};

typedef struct RomeFont RomeFont;

struct RomeFont {
	int cell_w, cell_h;     /* cell size in pixels */
	int ascent;             /* baseline, pixels from the top of a cell */
	int underline_y;        /* underline row inside the cell */
	int line_px;            /* underline/cursor thickness */
	uint8_t *atlas;         /* atlas_w x atlas_h coverage */
	int atlas_w, atlas_h;
	int slot_w, slot_h, slots_per_row;
	/* Atlas rows changed since rome_font_take_dirty() (GL upload), and a
	 * counter that changes whenever the whole atlas was reset. */
	int dirty_y0, dirty_y1;
	unsigned generation;
	char *family;           /* the matched family, for the window */
	double px;
	void *priv;
};

/* `family` is a fontconfig pattern ("DejaVu Sans Mono", "monospace"),
 * `px` the size in pixels. NULL if no font can be opened. */
RomeFont *rome_font_new(const char *family, double px);
void rome_font_free(RomeFont *f);

/* Atlas slot of a glyph, or -1 for a glyph that draws nothing (space). The
 * glyph is rasterised on first use. `wide` selects a two-cell glyph. */
int rome_font_glyph(RomeFont *f, uint32_t cp, int style, int wide);

static inline int rome_font_slot_x(const RomeFont *f, int slot)
{
	return (slot % f->slots_per_row) * f->slot_w;
}
static inline int rome_font_slot_y(const RomeFont *f, int slot)
{
	return (slot / f->slots_per_row) * f->slot_h;
}

#endif
