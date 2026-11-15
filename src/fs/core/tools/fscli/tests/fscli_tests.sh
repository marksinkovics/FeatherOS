# SPDX-License-Identifier: MIT OR Apache-2.0
# shellcheck shell=sh
#
# Runs fscli against the images tests/make_fat16_image.sh made and checks what it prints.
#
#   sh fscli_tests.sh <fscli> <bare image> <mbr image> <work directory> <case>
#
# Exits 77, which CTest reports as skipped, when the images were not made (no mtools).

set -eu

fscli="$1"
bare="$2"
mbr="$3"
work="$4"
case="$5"

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
        has_line '-         29  Read Me First.txt'
        has_line '-      11400  big.txt'
        grep -Eq '^d +[0-9]+  docs$' "$work/out" || fail "docs is not listed as a directory"
        grep -Eq '^d +[0-9]+  many$' "$work/out" || fail "many is not listed as a directory"
        [ "$(wc -l <"$work/out")" -eq 4 ] || fail "the root should hold 4 entries"
        run 0 ls "$bare" /docs/archive
        has_line '-         10  Old Plans.txt'
        run 0 ls "$bare" many
        [ "$(wc -l <"$work/out")" -eq 40 ] || fail "many should hold 40 entries"
        has_line '-          7  f7.txt'
        has_line '-          8  f39.txt'
        run 0 ls "$bare" /docs/notes.txt
        has_line '-          6  notes.txt'
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
        has_line '-          6  notes.txt'
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
    *)
        fail "unknown case '$case'"
        ;;
esac
