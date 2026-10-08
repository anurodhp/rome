#!/bin/bash
# Cross-builds Rome.app for the Pi (Darwin arm64) with the iokit repo's
# toolchain; this checkout is built as-is, not the pinned third_party/rome.
#
#   ./build.sh                 build; output lands in iokit's libc_build/gnustep/root
#   ./build.sh deploy          build, then copy Rome.app to the Pi
#
# IOKIT_DIR   the iokit checkout (default ../iokit)
# DEPLOY_HOST ssh target for "deploy" (default root@10.0.0.142; needs ssh access)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
IOKIT_DIR="${IOKIT_DIR:-$HERE/../iokit}"
STAGING="$IOKIT_DIR/tools/userland_staging"
[ -x "$STAGING/build_rome.sh" ] || { echo "error: $STAGING/build_rome.sh not found; set IOKIT_DIR" >&2; exit 1; }

ROME_SRC="$HERE" "$STAGING/build_rome.sh"

[ "${1:-}" = deploy ] || exit 0
HOST="${DEPLOY_HOST:-root@10.0.0.142}"
GSROOT="$(cd "$STAGING/libc_build/gnustep/root/usr/GNUstep/System" && pwd -P)"
tar -C "$GSROOT/Applications" -cf - Rome.app |
    ssh "$HOST" 'rm -rf /Applications/Rome.app && tar -C /Applications -xf -'
echo "deployed Rome.app to $HOST"
