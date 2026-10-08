/*
 * rome: fontconfig + FreeType glyph atlas (see RomeFont.h).
 */
#include "RomeFont.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_SIZE 16384          /* > 2 x the slot count, power of two */
#define MAX_FALLBACK 24

typedef struct {
	FT_Library ft;
	FT_Face face[4];             /* by ROME_STYLE_*; may repeat */
	FcFontSet *fallback_set;     /* fontconfig's sorted list, for missing glyphs */
	FcPattern *query;            /* the pattern that list was sorted for: FcFontRenderPrepare needs it */
	FT_Face fallback[MAX_FALLBACK];
	int nfallback_tried;
	uint32_t *keys;              /* key + 1, 0 = empty */
	int *vals;
	int next_slot, nslots;
} Priv;

static FT_Face
open_face(Priv *p, FcPattern *pat, double px)
{
	FcChar8 *file = NULL;
	int index = 0;
	FT_Face face;
	if (FcPatternGetString(pat, FC_FILE, 0, &file) != FcResultMatch)
		return NULL;
	FcPatternGetInteger(pat, FC_INDEX, 0, &index);
	if (FT_New_Face(p->ft, (const char *)file, index, &face) != 0)
		return NULL;
	if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(px * 64), 72, 72) != 0) {
		FT_Done_Face(face);
		return NULL;
	}
	return face;
}

static FcPattern *
make_pattern(const char *family, double px, int style)
{
	FcPattern *pat = FcNameParse((const FcChar8 *)family);
	if (pat == NULL)
		return NULL;
	FcPatternDel(pat, FC_PIXEL_SIZE);
	FcPatternAddDouble(pat, FC_PIXEL_SIZE, px);
	FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
	if (style & ROME_STYLE_BOLD)
		FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (style & ROME_STYLE_ITALIC)
		FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	return pat;
}

static FT_Face
match_face(Priv *p, const char *family, double px, int style, char **matched_family)
{
	FcResult res;
	FcPattern *pat = make_pattern(family, px, style), *m;
	FT_Face face = NULL;
	if (pat == NULL)
		return NULL;
	m = FcFontMatch(NULL, pat, &res);
	if (m != NULL) {
		face = open_face(p, m, px);
		if (matched_family != NULL) {
			FcChar8 *fam = NULL;
			if (FcPatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch)
				*matched_family = strdup((const char *)fam);
		}
		FcPatternDestroy(m);
	}
	if (style == ROME_STYLE_REGULAR && p->fallback_set == NULL) {
		p->fallback_set = FcFontSort(NULL, pat, FcTrue, NULL, &res);
		p->query = FcPatternDuplicate(pat);
	}
	FcPatternDestroy(pat);
	return face;
}

RomeFont *
rome_font_new(const char *family, double px)
{
	RomeFont *f = calloc(1, sizeof(*f));
	Priv *p = calloc(1, sizeof(*p));
	if (f == NULL || p == NULL || !FcInit() || FT_Init_FreeType(&p->ft) != 0)
		goto fail;
	f->priv = p;
	f->px = px;
	p->face[0] = match_face(p, family, px, ROME_STYLE_REGULAR, &f->family);
	if (p->face[0] == NULL)
		goto fail;
	for (int s = 1; s < 4; s++) {
		p->face[s] = match_face(p, family, px, s, NULL);
		if (p->face[s] == NULL)
			p->face[s] = (s & ROME_STYLE_BOLD) && p->face[1] != NULL ? p->face[1] : p->face[0];
	}

	FT_Face face = p->face[0];
	const FT_Size_Metrics *sm = &face->size->metrics;
	int asc = (int)ceil(sm->ascender / 64.0);
	int desc = (int)ceil(-sm->descender / 64.0);
	double adv = sm->max_advance / 64.0;
	if (FT_Load_Char(face, 'M', FT_LOAD_DEFAULT) == 0)
		adv = face->glyph->advance.x / 64.0;
	f->cell_w = (int)lround(adv);
	f->cell_h = asc + desc;
	if (f->cell_w < 1 || f->cell_h < 1)
		goto fail;
	f->ascent = asc;
	f->line_px = f->cell_h >= 24 ? 2 : 1;
	f->underline_y = asc + (desc > 2 ? 1 : 0);
	if (f->underline_y > f->cell_h - f->line_px)
		f->underline_y = f->cell_h - f->line_px;

	f->slot_w = 2 * f->cell_w;
	f->slot_h = f->cell_h;
	f->atlas_w = 1024;
	f->slots_per_row = f->atlas_w / f->slot_w;
	/* room for ~4000 glyphs at any size, at most 2048 rows (GL 2.1 limit
	 * on vc4 is 2048) */
	int rows = (4096 + f->slots_per_row - 1) / f->slots_per_row;
	if (rows * f->slot_h > 2048)
		rows = 2048 / f->slot_h;
	f->atlas_h = rows * f->slot_h;
	int pow2 = 64;
	while (pow2 < f->atlas_h)
		pow2 *= 2;
	f->atlas_h = pow2;   /* GL 2.1 needs no power of two, GLES 2.0 mipmaps do; cheap anyway */
	p->nslots = f->slots_per_row * (f->atlas_h / f->slot_h);
	f->atlas = calloc((size_t)f->atlas_w * f->atlas_h, 1);
	p->keys = calloc(HASH_SIZE, sizeof(*p->keys));
	p->vals = calloc(HASH_SIZE, sizeof(*p->vals));
	if (f->atlas == NULL || p->keys == NULL || p->vals == NULL)
		goto fail;
	/* slot 0: solid */
	for (int y = 0; y < f->slot_h; y++)
		memset(f->atlas + (size_t)y * f->atlas_w, 255, f->slot_w);
	p->next_slot = 1;
	f->dirty_y0 = 0;
	f->dirty_y1 = f->atlas_h;
	return f;
fail:
	if (f != NULL) {
		f->priv = p;
		rome_font_free(f);
	}
	return NULL;
}

void
rome_font_free(RomeFont *f)
{
	if (f == NULL)
		return;
	Priv *p = f->priv;
	if (p != NULL) {
		for (int s = 0; s < 4; s++) {
			int dup = 0;
			for (int t = 0; t < s; t++)
				dup |= p->face[t] == p->face[s];
			if (p->face[s] != NULL && !dup)
				FT_Done_Face(p->face[s]);
		}
		for (int i = 0; i < MAX_FALLBACK; i++)
			if (p->fallback[i] != NULL)
				FT_Done_Face(p->fallback[i]);
		if (p->fallback_set != NULL)
			FcFontSetDestroy(p->fallback_set);
		if (p->query != NULL)
			FcPatternDestroy(p->query);
		if (p->ft != NULL)
			FT_Done_FreeType(p->ft);
		free(p->keys);
		free(p->vals);
		free(p);
	}
	free(f->atlas);
	free(f->family);
	free(f);
}

/* A face that has `cp`: the style's own, else the first fontconfig fallback
 * that covers it (opened on first need). */
static FT_Face
face_for(RomeFont *f, uint32_t cp, int style, FT_UInt *gi)
{
	Priv *p = f->priv;
	FT_Face face = p->face[style];
	if ((*gi = FT_Get_Char_Index(face, cp)) != 0)
		return face;
	if (style != 0 && (*gi = FT_Get_Char_Index(p->face[0], cp)) != 0)
		return p->face[0];
	if (p->fallback_set == NULL)
		return p->face[0];
	for (int i = 0; i < p->fallback_set->nfont && i < MAX_FALLBACK; i++) {
		FcCharSet *cs = NULL;
		if (FcPatternGetCharSet(p->fallback_set->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch ||
		    !FcCharSetHasChar(cs, cp))
			continue;
		if (p->fallback[i] == NULL) {
			FcPattern *m = FcFontRenderPrepare(NULL, p->query, p->fallback_set->fonts[i]);
			if (m == NULL)
				continue;
			p->fallback[i] = open_face(p, m, f->px);
			FcPatternDestroy(m);
			if (p->fallback[i] == NULL)
				continue;
		}
		if ((*gi = FT_Get_Char_Index(p->fallback[i], cp)) != 0)
			return p->fallback[i];
	}
	*gi = 0;
	return p->face[0];
}

static void
atlas_reset(RomeFont *f)
{
	Priv *p = f->priv;
	memset(p->keys, 0, HASH_SIZE * sizeof(*p->keys));
	memset(f->atlas + (size_t)f->slot_h * f->atlas_w, 0, (size_t)(f->atlas_h - f->slot_h) * f->atlas_w);
	p->next_slot = 1;
	f->generation++;
	f->dirty_y0 = 0;
	f->dirty_y1 = f->atlas_h;
}

static void
rasterise(RomeFont *f, int slot, uint32_t cp, int style, int wide)
{
	FT_UInt gi;
	int mark = (style & ROME_STYLE_MARK) != 0;
	style &= 3;
	FT_Face face = face_for(f, cp, style, &gi);
	int sx = rome_font_slot_x(f, slot), sy = rome_font_slot_y(f, slot);
	int w = wide ? 2 * f->cell_w : f->cell_w;
	if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
		return;
	FT_GlyphSlot g = face->glyph;
	FT_Bitmap *bm = &g->bitmap;
	if (bm->pixel_mode != FT_PIXEL_MODE_GRAY)
		return;
	int ox = g->bitmap_left, oy = f->ascent - g->bitmap_top;
	if (mark) {
		/* A zero-advance mark is positioned relative to the pen after its base character, so it
		 * sits at a negative offset: move it from the end of the cell back over the base. */
		if (g->advance.x == 0)
			ox += w;
	} else if ((int)bm->width > w) {
		/* A glyph wider than its cell (fallback fonts) is centred and clipped. */
		ox = (w - (int)bm->width) / 2;
	} else if (ox + (int)bm->width > w) {
		ox = w - (int)bm->width;
	}
	if (!mark && ox < 0 && (int)bm->width <= w)
		ox = 0;
	for (unsigned r = 0; r < bm->rows; r++) {
		int y = oy + (int)r;
		if (y < 0 || y >= f->cell_h)
			continue;
		const uint8_t *src = bm->buffer + (long)r * bm->pitch;
		uint8_t *dst = f->atlas + (size_t)(sy + y) * f->atlas_w + sx;
		for (unsigned c = 0; c < bm->width; c++) {
			int x = ox + (int)c;
			if (x >= 0 && x < w)
				dst[x] = src[c];
		}
	}
}

/* Box drawing (U+2500-U+257F) and block elements (U+2580-U+259F) are drawn
 * by hand for the common shapes, so lines join across cells whatever the
 * font's line spacing; the rest fall through to the font. */
static int
draw_box(RomeFont *f, int slot, uint32_t cp)
{
	int sx = rome_font_slot_x(f, slot), sy = rome_font_slot_y(f, slot);
	int w = f->cell_w, h = f->cell_h, t = f->line_px;
	/* light lines: which arms (left, right, up, down) */
	int l = 0, r = 0, u = 0, d = 0, heavy = 0;
	switch (cp) {
	case 0x2500: l = r = 1; break;              /* ─ */
	case 0x2501: l = r = 1; heavy = 1; break;   /* ━ */
	case 0x2502: u = d = 1; break;              /* │ */
	case 0x2503: u = d = 1; heavy = 1; break;   /* ┃ */
	case 0x250c: r = d = 1; break;              /* ┌ */
	case 0x2510: l = d = 1; break;              /* ┐ */
	case 0x2514: r = u = 1; break;              /* └ */
	case 0x2518: l = u = 1; break;              /* ┘ */
	case 0x251c: u = d = r = 1; break;          /* ├ */
	case 0x2524: u = d = l = 1; break;          /* ┤ */
	case 0x252c: l = r = d = 1; break;          /* ┬ */
	case 0x2534: l = r = u = 1; break;          /* ┴ */
	case 0x253c: l = r = u = d = 1; break;      /* ┼ */
	case 0x256d: r = d = 1; break;              /* ╭ (drawn square) */
	case 0x256e: l = d = 1; break;              /* ╮ */
	case 0x256f: l = u = 1; break;              /* ╯ */
	case 0x2570: r = u = 1; break;              /* ╰ */
	default: break;
	}
	uint8_t *a = f->atlas;
	int aw = f->atlas_w;
#define FILL(x0, y0, x1, y1) \
	for (int yy = (y0); yy < (y1); yy++) \
		memset(a + (size_t)(sy + yy) * aw + sx + (x0), 255, (size_t)((x1) - (x0)))
	if (l || r || u || d) {
		int th = heavy ? 2 * t : t;
		int hx = (w - th) / 2, hy = (h - th) / 2;
		if (l) FILL(0, hy, hx + th, hy + th);
		if (r) FILL(hx, hy, w, hy + th);
		if (u) FILL(hx, 0, hx + th, hy + th);
		if (d) FILL(hx, hy, hx + th, h);
		return 1;
	}
	if (cp >= 0x2580 && cp <= 0x2590) {
		switch (cp) {
		case 0x2580: FILL(0, 0, w, h / 2); return 1;            /* ▀ */
		case 0x2584: FILL(0, h - h / 2, w, h); return 1;        /* ▄ */
		case 0x2588: FILL(0, 0, w, h); return 1;                /* █ */
		case 0x258c: FILL(0, 0, w / 2, h); return 1;            /* ▌ */
		case 0x2590: FILL(w - w / 2, 0, w, h); return 1;        /* ▐ */
		default:
			if (cp >= 0x2581 && cp <= 0x2587) {               /* ▁ .. ▇ */
				int n = (int)(cp - 0x2580);
				FILL(0, h - h * n / 8, w, h);
				return 1;
			}
			if (cp >= 0x2589 && cp <= 0x258f) {               /* ▉ .. ▏ */
				int n = 8 - (int)(cp - 0x2588);
				FILL(0, 0, w * n / 8, h);
				return 1;
			}
		}
	}
	if (cp >= 0x2591 && cp <= 0x2593) {                         /* ░ ▒ ▓ */
		uint8_t v = cp == 0x2591 ? 64 : cp == 0x2592 ? 128 : 192;
		for (int yy = 0; yy < h; yy++)
			memset(a + (size_t)(sy + yy) * aw + sx, v, (size_t)w);
		return 1;
	}
#undef FILL
	return 0;
}

int
rome_font_glyph(RomeFont *f, uint32_t cp, int style, int wide)
{
	Priv *p = f->priv;
	if (cp == 0 || cp == ' ' || cp == 0xa0)
		return -1;
	uint32_t key = (cp & 0x1fffff) | ((uint32_t)(style & 3) << 21) | ((uint32_t)(wide != 0) << 23) |
	    ((uint32_t)((style & ROME_STYLE_MARK) != 0) << 24);
	uint32_t h = (key * 2654435761u) & (HASH_SIZE - 1);
	while (p->keys[h] != 0) {
		if (p->keys[h] == key + 1)
			return p->vals[h];
		h = (h + 1) & (HASH_SIZE - 1);
	}
	if (p->next_slot >= p->nslots) {
		atlas_reset(f);
		h = (key * 2654435761u) & (HASH_SIZE - 1);
	}
	int slot = p->next_slot++;
	if ((style & ROME_STYLE_MARK) || !draw_box(f, slot, cp))
		rasterise(f, slot, cp, style, wide);
	p->keys[h] = key + 1;
	p->vals[h] = slot;
	int y0 = rome_font_slot_y(f, slot);
	if (f->dirty_y1 <= f->dirty_y0) {
		f->dirty_y0 = y0;
		f->dirty_y1 = y0 + f->slot_h;
	} else {
		if (y0 < f->dirty_y0)
			f->dirty_y0 = y0;
		if (y0 + f->slot_h > f->dirty_y1)
			f->dirty_y1 = y0 + f->slot_h;
	}
	return slot;
}
