# TODO

Remaining work (to become Plane work items in the Rome project).

1. **Text shaping (HarfBuzz)**: Arabic letter joining, Devanagari conjuncts. Needs HarfBuzz cross-built for the Pi (not in iokit's third_party); shape per grapheme-cluster run, glyph IDs in the atlas instead of code points.
2. **Bidirectional text (FriBidi)**: reorder right-to-left runs per row; map cursor, selection and mouse columns to display order.
3. **Colour emoji**: colour (RGBA) glyph atlas and Noto Color Emoji (CBDT/COLRv1) in both renderers; the atlas is single-channel today (monochrome Noto Emoji).
4. **Emoji sequences**: ZWJ, skin-tone modifiers, flags; only the first emoji is drawn today. Depends on 1 and 3.
5. **Image protocols**: Sixel and Kitty graphics (ignored today).
