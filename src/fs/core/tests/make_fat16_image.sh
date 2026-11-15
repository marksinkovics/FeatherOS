# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Makes the FAT16 image that tests/fat_mtools_tests.cpp reads, with mtools: 16384 sectors of one
# sector per cluster and two FATs, without a partition table.
#
#   sh make_fat16_image.sh <image>
#
# The contents must match what the tests expect.

set -eu

image="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

rm -f "$image"
mformat -C -i "$image" -t 128 -h 4 -s 32 -c 1 -v FEATHERDISK ::

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

mcopy -i "$image" "$work/readme" "::Read Me First.txt"
mcopy -i "$image" "$work/big" ::big.txt
mmd -i "$image" ::docs ::docs/archive
mcopy -i "$image" "$work/notes" ::docs/notes.txt
mcopy -i "$image" "$work/old" "::docs/archive/Old Plans.txt"
mcopy -s -i "$image" "$work/many" ::
