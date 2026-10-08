#!/bin/bash
# Host test of the terminal core (RomeTerm.c) against the real libghostty-vt,
# with a stub renderer: feeds escape sequences, renders, and prints the cells,
# the drawn spans, the scrollback, the selection and what keys, mouse, paste
# and queries send to the pty. Read the output; there are no assertions on it.
#
#   tools/term_test.sh [ghostty checkout]     (default ../ghostty-src; needs Zig 0.16)
#   TRACE=1 tools/term_test.sh                also prints every row span drawn
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd -P)"
GH="${1:-$HERE/../ghostty-src}"
ZIG="${ZIG_BIN:-$(command -v zig || true)}"
"$ZIG" version | grep -q '^0\.16\.' || ZIG="$(brew --prefix zig@0.16)/bin/zig"
B="$HERE/.build-test"
mkdir -p "$B"
[ -f "$B/lib/libghostty-vt.a" ] || (cd "$GH" && "$ZIG" build -Dsimd=false -Doptimize=ReleaseFast -Demit-lib-vt=true --prefix "$B")
clang -std=gnu99 -g -Wall -I"$B/include" -I"$HERE" "$HERE/tools/term_test.c" "$B/lib/libghostty-vt.a" -o "$B/term_test"
"$B/term_test"
