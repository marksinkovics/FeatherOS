# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Makes the ext2 images that tests/ext2_image_tests.cpp and the fscli tests read, with mke2fs -d
# from e2fsprogs, and fixes owners, times and link modes with debugfs so they do not depend on
# who builds them. Every image is 8 MiB with the same contents, except ext4-extents:
#
#   1k          1 KiB blocks, 128-byte inodes, filetype
#   4k          4 KiB blocks, 256-byte inodes with nanosecond times, filetype
#   nofiletype  2 KiB blocks, 256-byte inodes, without the filetype feature
#   rev0        revision 0: 1 KiB blocks, 128-byte inodes, no features at all
#   groups      1 KiB blocks, 128-byte inodes, in eight groups of 1024 blocks
#   mbr         the 1k volume as the only partition (type 0x83) of an MBR disk, from sector 2048
#   ext3        an ext3 volume with a clean journal
#   ext4        mke2fs's default ext4 with 1 KiB blocks: extents, 64bit, flex_bg, metadata_csum
#   ext4-extents    the same in 32 MiB, with more: holes.bin (3 MiB, every eighth block a hole
#               that debugfs punched: an extent tree of depth 2), fragmented.bin (400 blocks
#               written by debugfs into the gaps of a filler directory removed again: depth 1),
#               unwritten.bin (a written block, then 39 that debugfs fallocated, which reuse
#               blocks holes.bin left: unwritten extents over old data) and large/ (3000 entries,
#               a hash index of two levels after e2fsck -D)
#   ext4-blockmap   ext4 without extents or 64bit: 1 KiB blocks, metadata_csum with
#               metadata_csum_seed, flex_bg, huge_file, dir_nlink, extra_isize, four groups;
#               e2fsck -D gives many/ a hash index
#   ext4-64bit  2 KiB blocks, 64bit with 64-byte group descriptors, metadata_csum seeded from the
#               UUID, flex_bg, four groups; mke2fs makes 64bit only with extents, so the volume
#               is made without, tune2fs and resize2fs -b convert it, and debugfs takes the
#               extent feature away again (the files keep their block maps)
#   ext4-gdtcsum    4 KiB blocks, CRC16 group descriptor checksums (uninit_bg) instead of
#               metadata_csum, flex_bg, no journal, four groups
#   ext4-csum   ext4-blockmap without the journal (and so without orphan_file), which the core
#               writes: groups 1 to 3 with uninitialised inode tables, group 2 without a block
#               bitmap; e2fsck -D gives many/ a hash index
#   ext4-csum64 ext4-64bit without the journal, made the same way; many/ hashed too
#   ext4-htree  ext4-csum in 16 MiB with 4096 inodes and large/: 3000 empty files, a hash index of
#               two levels after e2fsck -D
#   ext4-htree-gdtcsum  the same with uninit_bg instead of metadata_csum
#   ext4-nojournal  ext4-extents without the journal, which the core writes: mke2fs's default
#               ext4 otherwise (extents, 64bit, flex_bg, metadata_csum)
#   ext4-mixed  4 KiB blocks, metadata_csum, flex_bg, no journal, four groups, made without
#               extents and given the feature by tune2fs afterwards: the image's files keep their
#               block maps on a volume whose new files get extents; many/ hashed
#   ext4-journal-gdtcsum  ext4 in 1 KiB blocks with CRC16 group descriptor checksums (uninit_bg)
#               instead of metadata_csum, so its journal has 64-bit block numbers and no checksums
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

if [ "$variant" = ext4-htree ] || [ "$variant" = ext4-htree-gdtcsum ]; then
    mkdir "$source/large"
    awk -v directory="$source/large" 'BEGIN {
        for (number = 0; number < 3000; number++) {
            name = sprintf("%s/file-with-a-long-name-for-htree-%05d.txt", directory, number)
            printf "" > name
            close(name)
        }
    }'
    chmod 0755 "$source/large"
    find "$source/large" -type f -exec chmod 0644 {} +
fi

if [ "$variant" = ext4-extents ] || [ "$variant" = ext4-nojournal ]; then
    mkdir "$source/large" "$source/filler"
    awk -v directory="$source/large" 'BEGIN {
        for (number = 0; number < 3000; number++) {
            name = sprintf("%s/file-with-a-long-name-for-htree-%05d.txt", directory, number)
            printf "%d\n", number > name
            close(name)
        }
    }'
    awk -v directory="$source/filler" 'BEGIN {
        for (number = 0; number < 600; number++) {
            name = sprintf("%s/%03d", directory, number)
            printf "%1024s", "x" > name
            close(name)
        }
    }'
    # 32 lines of 32 bytes per 1 KiB block.
    awk 'BEGIN { for (block = 0; block < 3072; block++) for (line = 0; line < 32; line++)
        printf "block %04d line %02d with holes!!\n", block, line }' >"$source/holes.bin"
    awk 'BEGIN { for (block = 0; block < 400; block++) for (line = 0; line < 32; line++)
        printf "piece %04d line %02d fragmented\n", block, line }' >"$work/fragmented.bin"
    printf 'start\n' >"$work/unwritten.bin"
    chmod 0755 "$source/large" "$source/filler"
    chmod 0644 "$source/holes.bin" "$work/fragmented.bin" "$work/unwritten.bin"
fi

size=8M
case "$variant" in
    1k | mbr) options="-t ext2 -b 1024 -I 128" ;;
    4k) options="-t ext2 -b 4096 -I 256" ;;
    nofiletype) options="-t ext2 -b 2048 -I 256 -O ^filetype" ;;
    rev0) options="-t ext2 -b 1024" ;;
    groups) options="-t ext2 -b 1024 -I 128 -g 1024" ;;
    ext3) options="-t ext3 -b 1024 -I 256" ;;
    ext4) options="-t ext4 -b 1024" ;;
    ext4-extents)
        options="-t ext4 -b 1024 -N 6144"
        size=32M
        ;;
    ext4-blockmap) options="-t ext4 -b 1024 -g 2048 -O ^extent,^64bit" ;;
    ext4-64bit) options="-t ext4 -b 2048 -g 1024 -O ^extent,^64bit,^metadata_csum_seed" ;;
    ext4-gdtcsum)
        options="-t ext4 -b 4096 -g 512 -O ^extent,^64bit,^metadata_csum,^has_journal,uninit_bg"
        ;;
    ext4-csum) options="-t ext4 -b 1024 -g 2048 -O ^extent,^64bit,^has_journal" ;;
    ext4-csum64)
        options="-t ext4 -b 2048 -g 1024 -O ^extent,^64bit,^metadata_csum_seed,^has_journal"
        ;;
    ext4-htree)
        options="-t ext4 -b 1024 -N 4096 -O ^extent,^64bit,^has_journal"
        size=16M
        ;;
    ext4-htree-gdtcsum)
        options="-t ext4 -b 1024 -N 4096 -O ^extent,^64bit,^has_journal,^metadata_csum,uninit_bg"
        size=16M
        ;;
    ext4-journal-gdtcsum) options="-t ext4 -b 1024 -O ^metadata_csum,uninit_bg" ;;
    ext4-nojournal)
        options="-t ext4 -b 1024 -N 6144 -O ^has_journal"
        size=32M
        ;;
    ext4-mixed)
        options="-t ext4 -b 4096 -g 1024 -O ^extent,^64bit,^has_journal"
        size=16M
        ;;
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
    "$mke2fs" -q -F $options -E revision=0 -d "$source" "$volume" "$size" >"$work/log" 2>&1 \
        || "$mke2fs" -q -F $options -r 0 -d "$source" "$volume" "$size" >"$work/log" 2>&1 \
        || {
            cat "$work/log" >&2
            exit 1
        }
elif ! "$mke2fs" -q -F $options -d "$source" "$volume" "$size" >"$work/log" 2>&1; then
    cat "$work/log" >&2
    exit 1
fi

tools="$(dirname "$mke2fs")"

if [ "$variant" = ext4-extents ] || [ "$variant" = ext4-nojournal ]; then
    {
        awk 'BEGIN { for (number = 0; number < 600; number += 2) printf "rm /filler/%03d\n", number }'
        printf 'write %s /fragmented.bin\n' "$work/fragmented.bin"
        awk 'BEGIN { for (block = 5; block < 3072; block += 8) printf "punch /holes.bin %d %d\n", block, block }'
        printf 'write %s /unwritten.bin\n' "$work/unwritten.bin"
        printf 'fallocate /unwritten.bin 1 39\n'
        printf 'sif /unwritten.bin size 40000\n'
        awk 'BEGIN { for (number = 1; number < 600; number += 2) printf "rm /filler/%03d\n", number }'
        printf 'rmdir /filler\n'
    } >"$work/commands"
    # rm prints an empty line, and write the inode it allocated.
    if ! "$debugfs" -w -f "$work/commands" "$volume" >"$work/log" 2>&1 \
        || grep -v -e '^debugfs' -e '^$' -e '^Allocated inode: ' "$work/log" >/dev/null; then
        cat "$work/log" >&2
        exit 1
    fi
fi

if [ "$variant" = ext4-blockmap ] || [ "$variant" = ext4-extents ] \
    || [ "$variant" = ext4-csum ] || [ "$variant" = ext4-csum64 ] \
    || [ "$variant" = ext4-htree ] || [ "$variant" = ext4-htree-gdtcsum ] \
    || [ "$variant" = ext4-nojournal ] || [ "$variant" = ext4-mixed ]; then
    # Exit status 1 means e2fsck changed the volume, as -D asks.
    status=0
    "$tools/e2fsck" -fyD "$volume" >"$work/log" 2>&1 || status=$?
    if [ "$status" -gt 1 ]; then
        cat "$work/log" >&2
        exit 1
    fi
fi

# After e2fsck -D, which would put the extent feature back on a 64bit volume.
if [ "$variant" = ext4-64bit ] || [ "$variant" = ext4-csum64 ]; then
    if ! "$tools/tune2fs" -O extent "$volume" >"$work/log" 2>&1 \
        || ! "$tools/resize2fs" -b "$volume" >"$work/log" 2>&1 \
        || ! "$debugfs" -w -R "feature -extent" "$volume" >"$work/log" 2>&1; then
        cat "$work/log" >&2
        exit 1
    fi
fi

if [ "$variant" = ext4-mixed ]; then
    if ! "$tools/tune2fs" -O extent "$volume" >"$work/log" 2>&1; then
        cat "$work/log" >&2
        exit 1
    fi
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
        4k | nofiletype | ext3 | ext4 | ext4-*) printf 'sif /hello.txt mtime_extra 4000\n' ;;
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
