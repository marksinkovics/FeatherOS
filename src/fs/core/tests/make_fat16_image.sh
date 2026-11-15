# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Makes the FAT16 image that tests/fat_mtools_tests.cpp and the fscli tests read, with mtools:
# 16384 sectors of one sector per cluster and two FATs, without a partition table, or with "mbr"
# as the only partition of an MBR disk, starting at sector 2048.
#
#   sh make_fat16_image.sh <image> [mbr]
#
# The contents must match what the tests expect.

set -eu

image="$1"
layout="${2:-bare}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

rm -f "$image"
if [ "$layout" = mbr ]; then
    volume="$image@@1048576"
    dd if=/dev/zero of="$image" bs=512 count=0 seek=18432 2>/dev/null
    mformat -i "$volume" -H 2048 -T 16384 -c 1 -v FEATHERDISK ::
    # Type 0x06 (FAT16) from sector 2048 for 16384 sectors, little-endian, then the signature.
    printf '\000\000\002\000\006\376\377\377\000\010\000\000\000\100\000\000' \
        | dd of="$image" bs=1 seek=446 conv=notrunc 2>/dev/null
    printf '\125\252' | dd of="$image" bs=1 seek=510 conv=notrunc 2>/dev/null
else
    volume="$image"
    mformat -C -i "$volume" -t 128 -h 4 -s 32 -c 1 -v FEATHERDISK ::
fi

printf 'Long names from mtools work.\n' >"$work/readme"
awk 'BEGIN { for (line = 0; line < 300; line++) printf "line %03d of a file over many clusters\n", line }' \
    >"$work/big"
printf 'notes\n' >"$work/notes"
printf 'old plans\n' >"$work/old"
mkdir "$work/many"
number=0
while [ "$number" -lt 40 ]; do
    printf 'file %d\n' "$number" >"$work/many/f$number.txt"
    number=$((number + 1))
done

mcopy -i "$volume" "$work/readme" "::Read Me First.txt"
mcopy -i "$volume" "$work/big" ::big.txt
mmd -i "$volume" ::docs ::docs/archive
mcopy -i "$volume" "$work/notes" ::docs/notes.txt
mcopy -i "$volume" "$work/old" "::docs/archive/Old Plans.txt"
mcopy -s -i "$volume" "$work/many" ::
