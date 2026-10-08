/* Opens Rome's font and rasterises the code points given on the command line (hex), printing the
 * slot each gets. For finding which glyph breaks the font code, with no window involved.
 *   font_probe [-f family] [-s px] 05e9 0041 ...                                          */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RomeFont.h"

int main(int argc, char **argv)
{
	const char *family = "DejaVu Sans Mono";
	double px = 13;
	int i = 1;
	for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
		if (!strcmp(argv[i], "-f")) family = argv[i + 1];
		else if (!strcmp(argv[i], "-s")) px = atof(argv[i + 1]);
	}
	RomeFont *f = rome_font_new(family, px);
	if (f == NULL) { printf("no font\n"); return 1; }
	printf("font %s %.0fpx: cell %dx%d ascent %d\n", f->family ? f->family : "?", px, f->cell_w, f->cell_h, f->ascent);
	fflush(stdout);
	for (; i < argc; i++) {
		unsigned long cp = strtoul(argv[i], NULL, 16);
		printf("U+%04lX: ", cp);
		fflush(stdout);
		int slot = rome_font_glyph(f, (uint32_t)cp, 0, 0);
		int ink = 0;
		if (slot >= 0)
			for (int y = 0; y < f->cell_h; y++)
				for (int x = 0; x < f->cell_w; x++)
					ink += f->atlas[(size_t)(rome_font_slot_y(f, slot) + y) * f->atlas_w + rome_font_slot_x(f, slot) + x] > 0;
		printf("slot %d, %d lit pixels\n", slot, ink);
		fflush(stdout);
	}
	rome_font_free(f);
	return 0;
}
