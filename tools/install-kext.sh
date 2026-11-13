#!/bin/sh
# Install ~/IntelSSTDMIC-new/IntelSSTDMIC.kext into EFI/OC/Kexts on the given EFI partition.
# Usage: sh install-sstdmic.sh [diskXsY]   (default disk1s1)
set -eu

DISK="${1:-disk1s1}"
BASE=$(cd "$(dirname "$0")" && pwd)
SRC="$BASE/IntelSSTDMIC-new/IntelSSTDMIC.kext"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo /bin/sh "$0" "$@"
fi

[ -d "$SRC/Contents" ] || { echo "ERROR: $SRC not found"; exit 1; }
[ -f "$SRC/Contents/MacOS/IntelSSTDMIC" ] || { echo "ERROR: kext binary missing"; exit 1; }
[ -f "$SRC/Contents/Info.plist" ] || { echo "ERROR: Info.plist missing"; exit 1; }

INFO=$(diskutil info "$DISK" 2>/dev/null) || { echo "ERROR: $DISK not found"; exit 1; }
printf '%s\n' "$INFO" | grep -q "Partition Type:.*EFI" || {
    echo "ERROR: $DISK does not look like an EFI partition"
    printf '%s\n' "$INFO" | grep -E "Partition Type|Volume Name|File System" || true
    exit 1
}

MOUNTED_BY_US=0
MP=$(printf '%s\n' "$INFO" | awk -F': ' '/Mount Point/ {print $2}' | head -1)
case "$MP" in
    ""|"Not mounted"|"Not applicable") MP="" ;;
esac

cleanup() {
    if [ "$MOUNTED_BY_US" = "1" ] && [ -n "$MP" ]; then
        diskutil unmount "$MP" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

if [ -z "$MP" ]; then
    echo "Mounting $DISK ..."
    diskutil mount "$DISK" >/dev/null
    MP=$(diskutil info "$DISK" | awk -F': ' '/Mount Point/ {print $2}' | head -1)
    MOUNTED_BY_US=1
    [ -n "$MP" ] && [ "$MP" != "Not mounted" ] || { echo "ERROR: mount failed"; exit 1; }
fi

KEXTDIR="$MP/EFI/OC/Kexts"
[ -d "$KEXTDIR" ] || {
    echo "ERROR: $KEXTDIR not found. Is OpenCore installed on $DISK?"
    exit 1
}

DEST="$KEXTDIR/IntelSSTDMIC.kext"
if [ -d "$DEST" ]; then
    BACKUP="$BASE/IntelSSTDMIC.kext.backup.$(date +%Y%m%d-%H%M%S)"
    echo "Backing up old kext -> $BACKUP"
    ditto "$DEST" "$BACKUP"
    rm -rf "$DEST"
fi

echo "Installing $SRC"
echo "        -> $DEST"
ditto "$SRC" "$DEST"
chown -R root:wheel "$DEST" 2>/dev/null || true
sync

echo
echo "Installed. Layout IDs in Info.plist:"
plutil -p "$DEST/Contents/Info.plist" 2>/dev/null | grep -A6 "IntelSSTDMLayoutIDs" || true

echo
echo "Kexts currently in $KEXTDIR:"
ls -1 "$KEXTDIR"
if [ -d "$KEXTDIR/TGLDmic.kext" ]; then
    echo "WARNING: TGLDmic.kext is also present; disable/remove it to avoid conflicts."
fi

echo
echo "Boot arguments:"
nvram boot-args 2>/dev/null || true
echo
echo "Reboot now. Keep -sstdmiclayout=auto; -tgldmiclilu is no longer needed."
