# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Runs fscli against the images tests/make_fat16_image.sh and tests/make_ext2_image.sh made and
# checks what it prints.
#
#   sh fscli_tests.sh <fscli> <bare image> <mbr image> <work directory> <case> [<ext2 images>]
#
# Exits 77, which CTest reports as skipped, when the images a case needs were not made (no mtools
# or no e2fsprogs).

set -eu

fscli="$1"
bare="$2"
mbr="$3"
work="$4"
case="$5"
ext="${6:-}"

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
        run 1 ls "$ext/ext4.img"
        error_has 'cannot mount ext2: not supported by this filesystem'
        error_has 'unsupported incompatible features'
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
    *)
        fail "unknown case '$case'"
        ;;
esac
