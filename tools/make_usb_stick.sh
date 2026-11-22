#!/usr/bin/env bash
# Builds a GPT disk image with one FAT32 EFI system partition holding Limine, the kernel and the
# initramfs from build/x86_64/, to write to a USB stick for an x86-64 UEFI machine.
#
# usage: tools/make_usb_stick.sh [image]            (default build/x86_64/featheros-usb.img)
# Needs the artifacts of "./build.sh --arch x86_64 build", python3 and mtools (mformat, mcopy).
# LIMINE_TIMEOUT=N (default 5) sets Limine's menu delay; KERNEL_CMDLINE adds kernel arguments.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/x86_64}"
IMAGE="${1:-$BUILD_DIR/featheros-usb.img}"
TIMEOUT="${LIMINE_TIMEOUT:-5}"
CMDLINE="${KERNEL_CMDLINE:-}"
LIMINE_EFI="$ROOT_DIR/deps/limine/BOOTX64.EFI"
CONFIG="$ROOT_DIR/configs/x86_64/limine.conf"

die() {
    printf 'make_usb_stick: %s\n' "$*" >&2
    exit 1
}

case "$TIMEOUT" in
    "" | *[!0-9]*) die "LIMINE_TIMEOUT must be a number of seconds, not '$TIMEOUT'" ;;
esac
for tool in mformat mcopy mmd python3; do
    command -v "$tool" >/dev/null || die "$tool not found (mtools and python3 are needed)"
done
for file in "$BUILD_DIR/kernel.elf" "$BUILD_DIR/initramfs.tar" "$LIMINE_EFI" "$CONFIG"; do
    [ -f "$file" ] || die "$file not found (run './build.sh --arch x86_64 build' first)"
done

# Sectors of 512 bytes; the partition starts at 1 MiB and 1 MiB at the end holds the backup GPT.
file_bytes=$(($(wc -c <"$BUILD_DIR/kernel.elf") + $(wc -c <"$BUILD_DIR/initramfs.tar") + 1048576))
partition_mib=$(((file_bytes * 11 / 10 + 1048575) / 1048576 + 8))
if [ "$partition_mib" -lt 64 ]; then
    partition_mib=64
fi
partition_sectors=$((partition_mib * 2048))
total_sectors=$(((partition_mib + 2) * 2048))

config_file="$(mktemp)"
trap 'rm -f "$config_file"' EXIT
sed "s/^timeout: .*/timeout: $TIMEOUT/" "$CONFIG" >"$config_file"
if [ -n "$CMDLINE" ]; then
    printf '\n    cmdline: %s\n' "$CMDLINE" >>"$config_file"
fi

mkdir -p "$(dirname "$IMAGE")"
rm -f "$IMAGE"
dd if=/dev/zero of="$IMAGE" bs=1048576 count=0 seek=$((partition_mib + 2)) 2>/dev/null

python3 - "$IMAGE" "$total_sectors" "$partition_sectors" <<'PY'
import struct
import sys
import uuid
import zlib

path, total, partition = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
first = 2048
last = first + partition - 1
entries_count, entry_size = 128, 128
entry_sectors = entries_count * entry_size // 512
disk_guid = uuid.uuid4()
esp_type = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")

entry = bytearray(entry_size)
entry[0:16] = esp_type.bytes_le
entry[16:32] = uuid.uuid4().bytes_le
struct.pack_into("<QQQ", entry, 32, first, last, 0)
entry[56 : 56 + 18] = "FeatherOS".encode("utf-16-le")
entries = bytes(entry) + bytes(entries_count * entry_size - entry_size)
entries_crc = zlib.crc32(entries)


def header(current, backup, entries_at):
    raw = bytearray(92)
    raw[0:8] = b"EFI PART"
    struct.pack_into("<IIII", raw, 8, 0x00010000, 92, 0, 0)
    struct.pack_into("<QQQQ", raw, 24, current, backup, 34, total - 34)
    raw[56:72] = disk_guid.bytes_le
    struct.pack_into("<QIII", raw, 72, entries_at, entries_count, entry_size, entries_crc)
    struct.pack_into("<I", raw, 16, zlib.crc32(bytes(raw)))
    return bytes(raw) + bytes(512 - 92)


protective = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, total - 1)
mbr = bytearray(512)
mbr[446:462] = protective
mbr[510:512] = b"\x55\xaa"
with open(path, "r+b") as disk:
    disk.write(mbr)
    disk.write(header(1, total - 1, 2))
    disk.write(entries)
    disk.seek((total - 1 - entry_sectors) * 512)
    disk.write(entries)
    disk.write(header(total - 1, 1, total - 1 - entry_sectors))
PY

target="$IMAGE@@1M"
mformat -i "$target" -F -T "$partition_sectors" -v FEATHEROS ::
mmd -i "$target" ::/EFI ::/EFI/BOOT
mcopy -i "$target" "$LIMINE_EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$target" "$config_file" ::/limine.conf
mcopy -i "$target" "$BUILD_DIR/kernel.elf" ::/kernel.elf
mcopy -i "$target" "$BUILD_DIR/initramfs.tar" ::/initramfs.tar

printf 'Wrote %s (%d MiB, Limine timeout %s s).\n' "$IMAGE" "$((partition_mib + 2))" "$TIMEOUT"
printf 'Write it to a stick with: sudo dd if=%s of=/dev/rdiskN bs=4m (macOS; diskutil list, unmount first)\n' "$IMAGE"
