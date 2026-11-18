# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Runs fscli against the images tests/make_fat16_image.sh and tests/make_ext2_image.sh made and
# checks what it prints. Cases that write work on copies, and e2fsck -fn must pass on every ext2
# volume fscli changed.
#
#   sh fscli_tests.sh <fscli> <bare image> <mbr image> <work directory> <case> [<ext2 images>]
#       [<e2fsck>] [<debugfs>]
#
# Exits 77, which CTest reports as skipped, when the images a case needs were not made (no mtools
# or no e2fsprogs) or e2fsck is missing for a case that writes ext2.

set -eu

fscli="$1"
bare="$2"
mbr="$3"
work="$4"
case="$5"
ext="${6:-}"
e2fsck="${7:-}"
debugfs="${8:-}"

rm -rf "$work"
mkdir -p "$work"

fail() {
    printf 'FAILED: %s\n' "$1" >&2
    if [ -f "$work/out" ]; then
        printf -- '--- standard output:\n' >&2
        cat "$work/out" >&2
    fi
    if [ -f "$work/err" ]; then
        printf -- '--- standard error:\n' >&2
        cat "$work/err" >&2
    fi
    exit 1
}

# Runs fscli with the arguments, expecting the exit status given first.
run() {
    expected="$1"
    shift
    status=0
    "$fscli" "$@" >"$work/out" 2>"$work/err" || status=$?
    [ "$status" -eq "$expected" ] || fail "fscli $* exited $status, expected $expected"
}

has_line() {
    grep -Fqx -- "$1" "$work/out" || fail "no line '$1' in the output"
}

error_has() {
    grep -Fq -- "$1" "$work/err" || fail "no '$1' in the error output"
}

need_images() {
    if [ -z "$bare" ] || [ ! -f "$bare" ] || [ -z "$mbr" ] || [ ! -f "$mbr" ]; then
        printf 'mtools made no images, so this test is skipped\n'
        exit 77
    fi
}

need_ext_images() {
    if [ -z "$ext" ] || [ ! -f "$ext/1k.img" ] || [ ! -f "$ext/mbr.img" ]; then
        printf 'e2fsprogs made no images, so this test is skipped\n'
        exit 77
    fi
}

need_e2fsck() {
    need_ext_images
    if [ -z "$e2fsck" ] || [ ! -x "$e2fsck" ]; then
        printf 'e2fsck is not installed, so this test is skipped\n'
        exit 77
    fi
}

# e2fsck -fn has nothing to report about the ext2 volume in the image.
check_clean() {
    status=0
    "$e2fsck" -fn "$1" >"$work/e2fsck" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        cat "$work/e2fsck" >&2
        fail "e2fsck -fn $1 exited $status"
    fi
}

# What debugfs reads from a path in the image matches a host file.
debugfs_matches() {
    if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
        "$debugfs" -R "cat $2" "$1" 2>/dev/null | cmp -s - "$3" \
            || fail "debugfs reads $2 differently from $3"
    fi
}

big_file() {
    awk 'BEGIN { for (line = 0; line < 300; line++) printf "line %03d of a file over many clusters\n", line }'
}

case "$case" in
    help)
        run 0 --help
        grep -q '^usage: fscli' "$work/out" || fail "no usage line"
        run 2
        error_has 'usage: fscli'
        run 2 format "$work"
        error_has "unknown command 'format'"
        ;;
    info)
        need_images
        run 0 info "$bare"
        has_line "disk: $bare, 16384 sectors of 512 bytes (8.0 MiB)"
        has_line 'table: none, one filesystem on the whole disk'
        has_line 'filesystem: fat16'
        grep -Eq '^  [0-9]+ blocks of 512 bytes, [0-9]+ free \(' "$work/out" \
            || fail "no volume statistics"
        ;;
    ls)
        need_images
        run 0 ls "$bare"
        # FAT has no permissions; a read-only mount reports none of the write bits.
        has_line '-r--r--r--         29  Read Me First.txt'
        has_line '-r--r--r--      11400  big.txt'
        grep -Eq '^dr-xr-xr-x +[0-9]+  docs$' "$work/out" || fail "docs is not listed as a directory"
        grep -Eq '^dr-xr-xr-x +[0-9]+  many$' "$work/out" || fail "many is not listed as a directory"
        [ "$(wc -l <"$work/out")" -eq 4 ] || fail "the root should hold 4 entries"
        run 0 ls "$bare" /docs/archive
        has_line '-r--r--r--         10  Old Plans.txt'
        run 0 ls "$bare" many
        [ "$(wc -l <"$work/out")" -eq 40 ] || fail "many should hold 40 entries"
        has_line '-r--r--r--          7  f7.txt'
        has_line '-r--r--r--          8  f39.txt'
        run 0 ls "$bare" /docs/notes.txt
        has_line '-r--r--r--          6  notes.txt'
        ;;
    cat)
        need_images
        run 0 cat "$bare" "/Read Me First.txt"
        printf 'Long names from mtools work.\n' | cmp -s - "$work/out" || fail "wrong long-name file"
        run 0 cat "$bare" "docs/archive/old plans.txt"
        printf 'old plans\n' | cmp -s - "$work/out" || fail "wrong nested file"
        run 0 cat "$bare" big.txt
        big_file | cmp -s - "$work/out" || fail "wrong multi-cluster file"
        ;;
    cp)
        need_images
        run 0 cp "$bare" /big.txt "$work/copy.txt"
        big_file | cmp -s - "$work/copy.txt" || fail "wrong copy of big.txt"
        mkdir "$work/into"
        run 0 cp "$bare" "/docs/archive/Old Plans.txt" "$work/into"
        printf 'old plans\n' | cmp -s - "$work/into/Old Plans.txt" || fail "wrong copy into a directory"
        run 1 cp "$bare" /docs "$work/docs-copy"
        error_has 'is a directory'
        [ ! -e "$work/docs-copy" ] || fail "a failed copy left a file behind"
        run 1 cp "$bare" /missing.txt "$work/missing-copy"
        [ ! -e "$work/missing-copy" ] || fail "a failed copy left a file behind"
        ;;
    cp-in)
        need_images
        cp "$bare" "$work/fat.img"
        big_file >"$work/big.txt"
        run 0 cp-in "$work/fat.img" "$work/big.txt" /COPIED.TXT
        run 0 cat "$work/fat.img" /copied.txt
        cmp -s "$work/big.txt" "$work/out" || fail "wrong file copied into FAT16"
        run 0 cp-in "$work/fat.img" "$work/big.txt" /docs
        run 0 cat "$work/fat.img" /docs/big.txt
        cmp -s "$work/big.txt" "$work/out" || fail "wrong file copied into a FAT16 directory"
        run 0 rm "$work/fat.img" /COPIED.TXT
        run 1 cat "$work/fat.img" /COPIED.TXT
        error_has 'not found'
        ;;
    tree)
        need_images
        cp "$bare" "$work/fat.img"
        run 0 mkdir "$work/fat.img" /made
        run 0 mkdir "$work/fat.img" "/made/Long Directory Name"
        run 0 mv "$work/fat.img" /big.txt "/made/Long Directory Name/moved.txt"
        run 0 cat "$work/fat.img" "/made/long directory name/moved.txt"
        big_file | cmp -s - "$work/out" || fail "wrong file after a move on FAT16"
        run 0 mv "$work/fat.img" "/made/Long Directory Name" /
        run 0 ls "$work/fat.img" "/Long Directory Name"
        has_line '-r--r--r--      11400  moved.txt'
        run 1 rmdir "$work/fat.img" "/Long Directory Name"
        error_has 'directory not empty'
        run 0 rmdir "$work/fat.img" /made
        run 1 ln -s "$work/fat.img" big.txt /link
        error_has 'not supported by this filesystem'
        ;;
    partition)
        need_images
        run 0 info "$mbr"
        has_line 'table: mbr'
        has_line 'partition 1: start 2048, 16384 sectors (8.0 MiB), fat16'
        grep -Eq '^  [0-9]+ blocks of 512 bytes, [0-9]+ free \(' "$work/out" \
            || fail "no volume statistics"
        run 0 ls "$mbr:1" /docs
        has_line '-r--r--r--          6  notes.txt'
        run 0 cat "$mbr" /docs/notes.txt
        printf 'notes\n' | cmp -s - "$work/out" || fail "wrong file from the first partition"
        run 1 ls "$mbr:2"
        error_has 'has no partition 2'
        ;;
    errors)
        need_images
        run 1 info "$work/no-such.img"
        error_has 'No such file or directory'
        run 1 info "$work"
        error_has 'is a directory'
        run 1 ls "$bare" /missing
        error_has '/missing: not found'
        run 1 ls "$bare" /big.txt/inner
        run 1 cat "$bare" /docs
        error_has 'is a directory'
        run 1 ls "$bare:1"
        error_has 'has no partition table'
        run 2 ls
        run 2 ls "$bare" / extra
        run 2 cat "$bare"
        run 2 cp "$bare" /big.txt
        printf 'not a filesystem' >"$work/junk.img"
        dd if=/dev/zero bs=512 count=64 >>"$work/junk.img" 2>/dev/null
        run 0 info "$work/junk.img"
        has_line 'table: none'
        has_line 'no partitions or filesystems found'
        run 1 ls "$work/junk.img"
        error_has 'no filesystem found'
        ;;
    ext2-info)
        need_ext_images
        run 0 info "$ext/4k.img"
        has_line 'table: none, one filesystem on the whole disk'
        has_line 'filesystem: ext2'
        grep -Eq '^  2048 blocks of 4096 bytes, [0-9]+ free \(' "$work/out" \
            || fail "no volume statistics"
        grep -Eq '^  [0-9]+ nodes, [0-9]+ free$' "$work/out" || fail "no node counts"
        grep -Eq '^  format: ext2, features: .*filetype' "$work/out" || fail "no ext2 features"
        run 0 info "$ext/ext3.img"
        has_line 'filesystem: ext3'
        grep -Eq '^  format: ext3, features: has_journal ' "$work/out" || fail "no ext3 features"
        ;;
    ext4-read)
        need_ext_images
        run 0 info "$ext/ext4-64bit.img"
        has_line 'filesystem: ext4'
        grep -Eq '^  format: ext4, features: .* 64bit .*metadata_csum' "$work/out" \
            || fail "no ext4 features"
        grep -Eq '^  4096 blocks of 2048 bytes, ' "$work/out" || fail "no volume statistics"
        run 0 info "$ext/ext4.img"
        grep -Eq '^  format: ext4, features: .* extent ' "$work/out" || fail "no extent feature"
        grep -Eq '^  8192 blocks of 1024 bytes, ' "$work/out" || fail "ext4 with extents did not mount"
        for image in ext4 ext4-blockmap ext4-64bit ext4-gdtcsum; do
            run 0 ls "$ext/$image.img"
            has_line '-rw-r-----         17  hello.txt'
            has_line 'lrwxrwxrwx         64  slow-link -> a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt'
            [ "$(wc -l <"$work/out")" -eq 12 ] || fail "the root of $image should hold 12 entries"
            run 0 ls "$ext/$image.img" many
            [ "$(wc -l <"$work/out")" -eq 300 ] || fail "many in $image should hold 300 entries"
            run 0 cat "$ext/$image.img" big.txt
            awk 'BEGIN { for (line = 0; line < 12800; line++) printf "line %05d of a file with indirect\n", line }' \
                | cmp -s - "$work/out" || fail "wrong file over indirect blocks in $image"
        done
        # A journal on another device keeps the volume read-only, byte for byte.
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            cp "$ext/ext4-blockmap.img" "$work/ext4.img"
            "$debugfs" -w -R "ssv journal_inum 0" "$work/ext4.img" >/dev/null 2>&1
            cp "$work/ext4.img" "$work/ext4-before.img"
            printf 'new\n' >"$work/new.txt"
            run 1 cp-in "$work/ext4.img" "$work/new.txt" /new.txt
            error_has 'the volume mounted read-only'
            cmp -s "$work/ext4-before.img" "$work/ext4.img" || fail "the read-only volume changed"
        fi
        ;;
    ext4-extents)
        need_ext_images
        image="$ext/ext4-extents.img"
        run 0 ls "$image"
        has_line '-rw-r--r--    3145728  holes.bin'
        has_line '-rw-r--r--     384000  fragmented.bin'
        has_line '-rw-r--r--      40000  unwritten.bin'
        has_line 'lrwxrwxrwx         64  slow-link -> a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt'
        grep -Eq '^drwxr-xr-x +[0-9]+  large$' "$work/out" || fail "large is not listed as a directory"
        [ "$(wc -l <"$work/out")" -eq 16 ] || fail "the root should hold 16 entries"
        run 0 ls "$image" large
        [ "$(wc -l <"$work/out")" -eq 3000 ] || fail "large should hold 3000 entries"
        has_line '-rw-r--r--          5  file-with-a-long-name-for-htree-02999.txt'
        run 0 cat "$image" large/file-with-a-long-name-for-htree-01234.txt
        printf '1234\n' | cmp -s - "$work/out" || fail "wrong file from the hashed directory"
        run 0 cat "$image" fragmented.bin
        awk 'BEGIN { for (block = 0; block < 400; block++) for (line = 0; line < 32; line++)
            printf "piece %04d line %02d fragmented\n", block, line }' \
            | cmp -s - "$work/out" || fail "wrong fragmented file"
        # Every eighth block of holes.bin is a hole, which awk cannot print, so debugfs is the
        # reference; without it the size and a block of text are checked.
        run 0 cp "$image" /holes.bin "$work/holes.bin"
        [ "$(wc -c <"$work/holes.bin")" -eq 3145728 ] || fail "wrong size of holes.bin"
        [ "$(dd if="$work/holes.bin" bs=1024 skip=4 count=1 2>/dev/null | head -c 32)" = 'block 0004 line 00 with holes!!' ] \
            || fail "wrong block before the first hole"
        [ "$(dd if="$work/holes.bin" bs=1024 skip=5 count=1 2>/dev/null | tr -d '\000' | wc -c)" -eq 0 ] \
            || fail "the first hole is not zeros"
        debugfs_matches "$image" /holes.bin "$work/holes.bin"
        run 0 cp "$image" /unwritten.bin "$work/unwritten.bin"
        [ "$(wc -c <"$work/unwritten.bin")" -eq 40000 ] || fail "wrong size of unwritten.bin"
        [ "$(tr -d '\000' <"$work/unwritten.bin")" = start ] || fail "unwritten blocks are not zeros"
        ;;
    ext2-ls)
        need_ext_images
        run 0 ls "$ext/1k.img"
        has_line '-rw-r-----         17  hello.txt'
        has_line '-rw-r-----         17  hard.txt'
        has_line '-rw-r--r--     448000  big.txt'
        has_line '-rw-r--r--   73400324  sparse.bin'
        has_line '-rwsr-xr-x         10  tool'
        has_line '?rw-r--r--          0  pipe'
        has_line 'lrwxrwxrwx         14  fast-link -> a/b/c/deep.txt'
        has_line 'lrwxrwxrwx         64  slow-link -> a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt'
        grep -Eq '^drwxr-xr-x +1024  a$' "$work/out" || fail "a is not listed as a directory"
        grep -Eq '^drwx------ +[0-9]+  lost\+found$' "$work/out" || fail "no lost+found"
        [ "$(wc -l <"$work/out")" -eq 12 ] || fail "the root should hold 12 entries"
        run 0 ls "$ext/4k.img" many
        [ "$(wc -l <"$work/out")" -eq 300 ] || fail "many should hold 300 entries"
        has_line '-rw-r--r--         10  entry-with-a-longer-name-299.txt'
        run 0 ls "$ext/1k.img" /fast-link
        has_line 'lrwxrwxrwx         14  fast-link -> a/b/c/deep.txt'
        run 0 ls "$ext/1k.img" a/b/c/deep.txt
        has_line '-rw-r--r--          5  deep.txt'
        ;;
    ext2-cat)
        need_ext_images
        run 0 cat "$ext/1k.img" /hello.txt
        printf 'Hello from ext2.\n' | cmp -s - "$work/out" || fail "wrong small file"
        run 0 cat "$ext/1k.img" big.txt
        awk 'BEGIN { for (line = 0; line < 12800; line++) printf "line %05d of a file with indirect\n", line }' \
            | cmp -s - "$work/out" || fail "wrong file over indirect blocks"
        run 0 cp "$ext/1k.img" /sparse.bin "$work/sparse.bin"
        [ "$(wc -c <"$work/sparse.bin")" -eq 73400324 ] || fail "wrong size of the sparse file"
        [ "$(head -c 4 "$work/sparse.bin")" = head ] || fail "wrong start of the sparse file"
        [ "$(tail -c 4 "$work/sparse.bin")" = tail ] || fail "wrong end of the sparse file"
        [ "$(dd if="$work/sparse.bin" bs=1 skip=300000 count=6 2>/dev/null)" = middle ] \
            || fail "wrong middle of the sparse file"
        ;;
    ext2-partition)
        need_ext_images
        run 0 info "$ext/mbr.img"
        has_line 'table: mbr'
        has_line 'partition 1: start 2048, 16384 sectors (8.0 MiB), ext2'
        run 0 ls "$ext/mbr.img:1" /a/b/c
        has_line '-rw-r--r--          5  deep.txt'
        run 0 cat "$ext/mbr.img" /a/b/c/deep.txt
        printf 'deep\n' | cmp -s - "$work/out" || fail "wrong file from the Linux partition"
        ;;
    ext2-errors)
        need_ext_images
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            cp "$ext/ext4.img" "$work/inline.img"
            "$debugfs" -w -R "feature inline_data" "$work/inline.img" >/dev/null 2>&1
            run 1 ls "$work/inline.img"
            error_has 'cannot mount ext4: not supported by this filesystem'
            error_has 'unsupported incompatible features'
        fi
        run 1 cat "$ext/1k.img" /fast-link
        error_has 'is a symbolic link'
        run 1 cp "$ext/1k.img" /pipe "$work/pipe-copy"
        error_has 'is a device, FIFO or socket'
        [ ! -e "$work/pipe-copy" ] || fail "a failed copy left a file behind"
        run 1 ls "$ext/1k.img" /hello.txt/inner
        error_has 'not a directory'
        run 1 ls "$ext/1k.img" /HELLO.TXT
        error_has 'not found'
        ;;
    ext2-cp-in)
        need_e2fsck
        cp "$ext/1k.img" "$work/1k.img"
        printf 'small file\n' >"$work/small.txt"
        # 310000 bytes reach double indirect blocks with 1 KiB blocks.
        awk 'BEGIN { for (line = 0; line < 10000; line++) printf "copied line %06d of 31 bytes\n", line }' \
            >"$work/large.txt"
        chmod 0640 "$work/large.txt"
        run 0 cp-in "$work/1k.img" "$work/small.txt" /small.txt
        run 0 cp-in "$work/1k.img" "$work/large.txt" /a/b
        run 0 cp-in "$work/1k.img" "$work/large.txt" /a/b/c/renamed.txt
        # Replacing a file that is there.
        run 0 cp-in "$work/1k.img" "$work/small.txt" /big.txt
        run 0 cat "$work/1k.img" /small.txt
        cmp -s "$work/small.txt" "$work/out" || fail "wrong small file"
        run 0 cat "$work/1k.img" /a/b/large.txt
        cmp -s "$work/large.txt" "$work/out" || fail "wrong file copied into a directory"
        run 0 cat "$work/1k.img" /big.txt
        cmp -s "$work/small.txt" "$work/out" || fail "the replaced file still has its old bytes"
        run 0 ls "$work/1k.img" /a/b/large.txt
        has_line '-rw-r-----     310000  large.txt'
        check_clean "$work/1k.img"
        debugfs_matches "$work/1k.img" /a/b/c/renamed.txt "$work/large.txt"
        debugfs_matches "$work/1k.img" /big.txt "$work/small.txt"
        # The partition of an MBR disk, and refusals.
        cp "$ext/mbr.img" "$work/mbr.img"
        run 0 cp-in "$work/mbr.img:1" "$work/small.txt" /a/small.txt
        run 0 cat "$work/mbr.img" /a/small.txt
        cmp -s "$work/small.txt" "$work/out" || fail "wrong file in the Linux partition"
        run 1 cp-in "$work/1k.img" "$work/small.txt" /missing/small.txt
        error_has 'not found'
        run 1 cp-in "$work/1k.img" "$work/small.txt" /fast-link
        error_has 'is a symbolic link'
        run 1 cp-in "$work/1k.img" "$work/no-such-file" /x
        error_has 'No such file or directory'
        run 1 cp-in "$work/1k.img" "$work" /x
        error_has 'not a regular file'
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            cp "$ext/ext3.img" "$work/ext3.img"
            "$debugfs" -w -R "ssv journal_inum 0" "$work/ext3.img" >/dev/null 2>&1
            run 1 cp-in "$work/ext3.img" "$work/small.txt" /small.txt
            error_has 'the volume mounted read-only'
            error_has 'journal'
        fi
        run 2 cp-in "$work/1k.img" "$work/small.txt"
        check_clean "$work/1k.img"
        ;;
    ext2-rm)
        need_e2fsck
        cp "$ext/4k.img" "$work/4k.img"
        run 0 rm "$work/4k.img" /big.txt
        run 0 rm "$work/4k.img" /hello.txt
        run 0 rm "$work/4k.img" /slow-link
        run 0 rm "$work/4k.img" /sparse.bin
        run 0 rm "$work/4k.img" many/entry-with-a-longer-name-150.txt
        run 0 ls "$work/4k.img"
        has_line '-rw-r-----         17  hard.txt'
        if grep -q 'big.txt\|hello.txt\|slow-link\|sparse.bin' "$work/out"; then
            fail "a removed file is still listed"
        fi
        run 0 ls "$work/4k.img" many
        [ "$(wc -l <"$work/out")" -eq 299 ] || fail "many should hold 299 entries"
        run 1 rm "$work/4k.img" /a
        error_has 'is a directory'
        run 1 rm "$work/4k.img" /big.txt
        error_has 'not found'
        run 1 rm "$work/4k.img" /
        run 2 rm "$work/4k.img"
        check_clean "$work/4k.img"
        ;;
    ext2-mkdir)
        need_e2fsck
        cp "$ext/1k.img" "$work/1k.img"
        run 0 mkdir "$work/1k.img" /made
        run 0 mkdir "$work/1k.img" /made/inner
        run 0 mkdir "$work/1k.img" a/b/c/deeper
        printf 'inside\n' >"$work/inside.txt"
        run 0 cp-in "$work/1k.img" "$work/inside.txt" /made/inner
        run 0 ls "$work/1k.img"
        grep -Eq '^drwxr-xr-x +1024  made$' "$work/out" || fail "made is not listed as a directory"
        run 1 mkdir "$work/1k.img" /made
        error_has 'already exists'
        run 1 mkdir "$work/1k.img" /missing/x
        error_has 'not found'
        run 1 rmdir "$work/1k.img" /made
        error_has 'directory not empty'
        run 1 rmdir "$work/1k.img" /hello.txt
        error_has 'not a directory'
        run 1 rmdir "$work/1k.img" /
        error_has 'needs a name'
        check_clean "$work/1k.img"
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            "$debugfs" -R "ls /made" "$work/1k.img" 2>/dev/null | grep -q inner \
                || fail "debugfs does not list /made/inner"
        fi
        debugfs_matches "$work/1k.img" /made/inner/inside.txt "$work/inside.txt"
        run 0 rm "$work/1k.img" /made/inner/inside.txt
        run 0 rmdir "$work/1k.img" /made/inner
        run 0 rmdir "$work/1k.img" /made
        run 0 rmdir "$work/1k.img" a/b/c/deeper
        run 1 ls "$work/1k.img" /made
        check_clean "$work/1k.img"
        ;;
    ext2-mv)
        need_e2fsck
        cp "$ext/4k.img" "$work/4k.img"
        run 0 mv "$work/4k.img" /hello.txt /greeting.txt
        run 0 mv "$work/4k.img" /greeting.txt /a/b
        run 0 cat "$work/4k.img" /a/b/greeting.txt
        printf 'Hello from ext2.\n' | cmp -s - "$work/out" || fail "wrong file after moving it"
        # Over a file, then a directory into another and back out over an empty one.
        run 0 mv "$work/4k.img" /tool /a/b/greeting.txt
        run 0 cat "$work/4k.img" /a/b/greeting.txt
        printf '#!/bin/sh\n' | cmp -s - "$work/out" || fail "the replaced file still has its old bytes"
        run 0 mv "$work/4k.img" /many /a
        run 0 ls "$work/4k.img" /a/many
        [ "$(wc -l <"$work/out")" -eq 300 ] || fail "a/many should hold 300 entries"
        run 0 mkdir "$work/4k.img" /landing
        run 0 mv "$work/4k.img" /a/many /landing
        run 0 ls "$work/4k.img" /landing/many
        run 0 mv "$work/4k.img" /landing/many /many-again
        run 0 rmdir "$work/4k.img" /landing
        run 1 mv "$work/4k.img" /a /a/b/c
        error_has 'invalid argument'
        run 1 mv "$work/4k.img" /a/b/c /big.txt
        error_has 'not a directory'
        run 1 mv "$work/4k.img" /missing /x
        error_has 'not found'
        check_clean "$work/4k.img"
        printf '#!/bin/sh\n' >"$work/tool.txt"
        debugfs_matches "$work/4k.img" /a/b/greeting.txt "$work/tool.txt"
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            "$debugfs" -R "ls /many-again" "$work/4k.img" 2>/dev/null \
                | grep -q entry-with-a-longer-name-299.txt || fail "debugfs does not see the move"
        fi
        ;;
    ext2-ln)
        need_e2fsck
        cp "$ext/1k.img" "$work/1k.img"
        run 0 ln "$work/1k.img" /big.txt /a/b/big-again.txt
        run 0 ln -s "$work/1k.img" ../../hello.txt /a/b/fast
        long_target=$(awk 'BEGIN { while (length(t) < 300) t = t "a/long/target/"; print t }')
        run 0 ln -s "$work/1k.img" "$long_target" /slow
        run 0 ln -s "$work/1k.img" hello.txt /a
        run 0 readlink "$work/1k.img" /a/b/fast
        has_line '../../hello.txt'
        run 0 readlink "$work/1k.img" /slow
        has_line "$long_target"
        run 0 readlink "$work/1k.img" /a/hello.txt
        has_line 'hello.txt'
        run 0 rm "$work/1k.img" /big.txt
        run 0 cat "$work/1k.img" /a/b/big-again.txt
        awk 'BEGIN { for (line = 0; line < 12800; line++) printf "line %05d of a file with indirect\n", line }' \
            | cmp -s - "$work/out" || fail "the second name lost the data"
        run 1 ln "$work/1k.img" /a /a-again
        error_has 'operation not permitted'
        run 1 ln "$work/1k.img" /hello.txt /hard.txt
        error_has 'already exists'
        run 1 readlink "$work/1k.img" /hello.txt
        error_has 'not a symbolic link'
        run 2 ln -s "$work/1k.img" x
        check_clean "$work/1k.img"
        if [ -n "$debugfs" ] && [ -x "$debugfs" ]; then
            "$debugfs" -R "stat /slow" "$work/1k.img" 2>/dev/null | grep -q 'Type: symlink' \
                || fail "debugfs does not see a symbolic link"
        fi
        ;;
    *)
        fail "unknown case '$case'"
        ;;
esac
