#!/bin/bash
# Cross-builds tools/font_probe.c (with RomeFont.c) for the Pi against the iokit repo's FreeType and
# fontconfig, like build.sh does for Rome:  tools/build_font_probe.sh -> $IOKIT_DIR/.../font_probe.macho
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd -P)"
IOKIT_DIR="${IOKIT_DIR:-$HERE/../iokit}"
cd "$IOKIT_DIR/tools/userland_staging"
. ./x11_common.sh
for l in libfontconfig libfreetype libexpat libpng16 libz libsystem_c libsystem_kernel libsystem_platform libsystem_malloc libsystem_pthread libdyld libSystem.B; do
    [ -f "$OUT/system/$l.dylib" ] || { echo "error: $OUT/system/$l.dylib not found" >&2; exit 1; }
done
OBJ="$X11_ROOT/obj/font_probe"
rm -rf "$OBJ"; mkdir -p "$OBJ"
FLAGS=(-isysroot "$SDK" -target arm64-apple-ios14.4 -c -O1 -fno-stack-protector -D_FORTIFY_SOURCE=0 -I"${ROMEFONT_INC:-$HERE}" -I"$HERE" -I"$X11_INC" -I"$X11_INC/freetype2")
"$CLANG" "${FLAGS[@]}" "${ROMEFONT_SRC:-$HERE/RomeFont.c}" -o "$OBJ/RomeFont.o"
"$CLANG" "${FLAGS[@]}" "$HERE/tools/font_probe.c" -o "$OBJ/font_probe.o"
S="$OUT/system"
"$CLANG" -isysroot "$SDK" -target arm64-apple-ios14.4 -nostdlib -DCRT_DYNAMIC_LINKING -B"$NEWLD_BINDIR" -Wl,-fixup_chains \
    -o font_probe.macho ../../third_party/Csu/start.s ../../third_party/Csu/crt.c "$OBJ"/*.o \
    "$S/libfontconfig.dylib" "$S/libfreetype.dylib" "$S/libexpat.dylib" "$S/libpng16.dylib" "$S/libz.dylib" \
    "$S/libsystem_c.dylib" "$S/libsystem_pthread.dylib" "$S/libsystem_malloc.dylib" "$S/libsystem_platform.dylib" \
    "$S/libsystem_kernel.dylib" "$S/libdyld.dylib" "$S/libSystem.B.dylib" 2>&1 | grep -v "^ld: warning" || true
[ -f font_probe.macho ] || { echo "error: link failed" >&2; exit 1; }
echo "wrote $PWD/font_probe.macho"
