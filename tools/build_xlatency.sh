#!/bin/bash
# Cross-builds xlatency (tools/xlatency.c) for the Pi with the iokit repo's
# toolchain and X11 libraries, like iokit's build_xtest_type.sh.
#   tools/build_xlatency.sh      -> $IOKIT_DIR/tools/userland_staging/xlatency.macho
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd -P)"
IOKIT_DIR="${IOKIT_DIR:-$HERE/../iokit}"
cd "$IOKIT_DIR/tools/userland_staging"
. ./x11_common.sh
X11_DYLIBS=(libXtst libXi libXext libX11 libxcb libXdmcp libXau)
for l in "${X11_DYLIBS[@]}"; do
    [ -f "$OUT/system/$l.dylib" ] || { echo "error: $OUT/system/$l.dylib not found -- run build_$l.sh first" >&2; exit 1; }
done
x11_require_sys_dylibs
OBJ="$X11_ROOT/obj/xlatency"
rm -rf "$OBJ"
mkdir -p "$OBJ"
x11_compile "$OBJ" "$HERE/tools/xlatency.c"
LINK=()
for l in "${X11_DYLIBS[@]}"; do LINK+=("$OUT/system/$l.dylib"); done
"$CLANG" -isysroot "$SDK" -target arm64-apple-ios14.4 -nostdlib \
    -DCRT_DYNAMIC_LINKING -B"$NEWLD_BINDIR" -Wl,-fixup_chains \
    -o xlatency.macho \
    ../../third_party/Csu/start.s ../../third_party/Csu/crt.c \
    "$OBJ"/*.o "${LINK[@]}" "${X11_SYS_DYLIBS[@]}"
./bind_target_audit.sh xlatency.macho "${LINK[@]}"
echo "wrote $PWD/xlatency.macho"
