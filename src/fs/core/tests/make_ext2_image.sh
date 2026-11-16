# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Makes the ext2 images that tests/ext2_image_tests.cpp and the fscli tests read, with mke2fs -d
# from e2fsprogs, and fixes owners, times and link modes with debugfs so they do not depend on
# who builds them. Every image is 8 MiB with the same contents:
#
#   1k          1 KiB blocks, 128-byte inodes, filetype
#   4k          4 KiB blocks, 256-byte inodes with nanosecond times, filetype
#   nofiletype  2 KiB blocks, 256-byte inodes, without the filetype feature
#   rev0        revision 0: 1 KiB blocks, 128-byte inodes, no features at all
#   groups      1 KiB blocks, 128-byte inodes, in eight groups of 1024 blocks
#   mbr         the 1k volume as the only partition (type 0x83) of an MBR disk, from sector 2048
#   ext3        an ext3 volume with a clean journal
#   ext4        an ext4 volume, which the ext2 driver must refuse
#
#   sh make_ext2_image.sh <mke2fs> <debugfs> <image> <variant>
#
# The contents must match what the tests expect.

set -eu

mke2fs="$1"
debugfs="$2"
image="$3"
variant="$4"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

source="$work/source"
mkdir -p "$source/a/b/c" "$source/many"
printf 'Hello from ext2.\n' >"$source/hello.txt"
ln "$source/hello.txt" "$source/hard.txt"
: >"$source/empty.txt"
# 12800 lines of 35 bytes: past the 268 KiB that direct and single indirect blocks reach with
# 1 KiB blocks.
awk 'BEGIN { for (line = 0; line < 12800; line++) printf "line %05d of a file with indirect\n", line }' \
    >"$source/big.txt"
# Holes around three short runs; the last one, at 70 MiB, needs a triple indirect block with
# 1 KiB blocks. mke2fs leaves blocks of zeros out, so the hole stays a hole on the volume.
printf 'head' >"$source/sparse.bin"
printf 'middle' | dd of="$source/sparse.bin" bs=1 seek=300000 conv=notrunc 2>/dev/null
printf 'tail' | dd of="$source/sparse.bin" bs=1 seek=73400320 conv=notrunc 2>/dev/null
printf 'deep\n' >"$source/a/b/c/deep.txt"
ln -s a/b/c/deep.txt "$source/fast-link"
ln -s a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt "$source/slow-link"
awk -v directory="$source/many" 'BEGIN {
    for (number = 0; number < 300; number++) {
        name = sprintf("%s/entry-with-a-longer-name-%03d.txt", directory, number)
        printf "entry %d\n", number > name
        close(name)
    }
}'
printf '#!/bin/sh\n' >"$source/tool"
# A 200-byte name, four directories of them deep under a/, so the file's path in the volume is
# 1006 bytes: close to FeatherOS's PATH_MAX of 1024 once mounted. That is past macOS's own limit
# for the source tree, so it is made one directory at a time, and chmod runs from inside each.
long=$(awk 'BEGIN { while (length(name) < 200) name = name "a-long-name-"; print substr(name, 1, 200) }')
(
    cd "$source/a"
    for _ in 1 2 3 4; do
        mkdir "$long"
        cd "$long"
    done
    printf 'far\n' >"$long"
)
# Fixed modes, whatever the umask.
chmod 0755 "$source"
find "$source" -mindepth 1 -type d -execdir chmod 0755 {} +
find "$source" -mindepth 1 -type f -execdir chmod 0644 {} +
chmod 0640 "$source/hello.txt"
chmod 4755 "$source/tool"
mkfifo -m 0644 "$source/pipe"

case "$variant" in
    1k | mbr) options="-t ext2 -b 1024 -I 128" ;;
    4k) options="-t ext2 -b 4096 -I 256" ;;
    nofiletype) options="-t ext2 -b 2048 -I 256 -O ^filetype" ;;
    rev0) options="-t ext2 -b 1024" ;;
    groups) options="-t ext2 -b 1024 -I 128 -g 1024" ;;
    ext3) options="-t ext3 -b 1024 -I 256" ;;
    ext4) options="-t ext4 -b 1024" ;;
    *)
        printf 'unknown variant %s\n' "$variant" >&2
        exit 2
        ;;
esac

volume="$work/volume.img"
# mke2fs warns about 128-byte inodes even with -q, so its output is shown only when it fails.
# shellcheck disable=SC2086
if [ "$variant" = rev0 ]; then
    # e2fsprogs 1.47 replaced -r with -E revision=; older releases know only -r.
    "$mke2fs" -q -F $options -E revision=0 -d "$source" "$volume" 8M >"$work/log" 2>&1 \
        || "$mke2fs" -q -F $options -r 0 -d "$source" "$volume" 8M >"$work/log" 2>&1 \
        || {
            cat "$work/log" >&2
            exit 1
        }
elif ! "$mke2fs" -q -F $options -d "$source" "$volume" 8M >"$work/log" 2>&1; then
    cat "$work/log" >&2
    exit 1
fi

{
    printf 'sif /hello.txt uid 100000\n'
    printf 'sif /hello.txt gid 4242\n'
    printf 'sif /hello.txt atime @1600000000\n'
    printf 'sif /hello.txt mtime @1577934245\n'
    printf 'sif /hello.txt ctime @1614834367\n'
    printf 'sif /fast-link mode 0120777\n'
    printf 'sif /slow-link mode 0120777\n'
    case "$variant" in
        # 4000 >> 2: 1000 nanoseconds, with no extra epoch bits.
        4k | nofiletype | ext3) printf 'sif /hello.txt mtime_extra 4000\n' ;;
    esac
} >"$work/commands"
# debugfs echoes each command after "debugfs:" and exits 0 even when one fails, so any other line
# is an error.
if ! "$debugfs" -w -f "$work/commands" "$volume" >"$work/log" 2>&1 \
    || grep -v '^debugfs' "$work/log" >/dev/null; then
    cat "$work/log" >&2
    exit 1
fi

rm -f "$image"
if [ "$variant" = mbr ]; then
    dd if=/dev/zero of="$image" bs=512 count=0 seek=18432 2>/dev/null
    dd if="$volume" of="$image" bs=512 seek=2048 conv=notrunc 2>/dev/null
    # Type 0x83 (Linux) from sector 2048 for 16384 sectors, little-endian, then the signature.
    printf '\000\000\002\000\203\376\377\377\000\010\000\000\000\100\000\000' \
        | dd of="$image" bs=1 seek=446 conv=notrunc 2>/dev/null
    printf '\125\252' | dd of="$image" bs=1 seek=510 conv=notrunc 2>/dev/null
else
    mv "$volume" "$image"
fi
