#!/bin/sh
# Build IntelSSTDMIC.kext.
#
#   ./build.sh            incremental build
#   ./build.sh clean      clean build
#
# Override dependency locations if needed:
#   SDK=/path/to/MacKernelSDK LILU=/path/to/Lilu.kext/Contents/Resources ./build.sh
set -eu

BASE=$(cd "$(dirname "$0")" && pwd)
SDK="${SDK:-$BASE/MacKernelSDK}"
LILU="${LILU:-$BASE/Lilu}"

[ -d "$SDK/Headers" ] || {
    echo "ERROR: MacKernelSDK not found at $SDK (set SDK=...)" >&2
    exit 1
}
[ -d "$LILU/Headers" ] || {
    echo "ERROR: Lilu resources not found at $LILU (set LILU=...)" >&2
    exit 1
}

if [ "${1:-}" = "clean" ]; then
    make -C "$BASE/kext" clean
    shift
fi

make -C "$BASE/kext" SDK="$SDK" LILU="$LILU" "$@"

echo
echo "Built: $BASE/kext/build/IntelSSTDMIC.kext"
echo "Zip:   $BASE/kext/build/IntelSSTDMIC.kext.zip"
