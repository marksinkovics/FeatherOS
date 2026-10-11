#!/bin/bash

set -e
# set -x

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")

_print() {
    printf '[DEBUG] %s\n' "$*"
}

_err() {
    printf '[ERROR] %s\n' "$*" >&2
}

# Verifies the external tools a given action depends on are on PATH, exiting
# with a single clear error instead of failing deep inside that action.
_require_tools() {
    local missing=()
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done

    if [ "${#missing[@]}" -gt 0 ]; then
        _err "Missing required tool(s): ${missing[*]}"
        _err "See docs/development.md for prerequisites."
        exit 1
    fi
}

build() {
    _require_tools git make cmake x86_64-elf-gcc x86_64-elf-g++ x86_64-elf-ld x86_64-elf-ar nasm
    if [ ! -d "$SCRIPT_DIR/deps/limine" ]; then
        _print "Fetching limine"
        git clone --branch=v8.x-binary --depth=1 https://github.com/limine-bootloader/limine.git deps/limine
        make -C deps/limine
    fi

    if [ ! -d "$SCRIPT_DIR/build" ]; then
        _print "Creating build folder"
        mkdir -p "$SCRIPT_DIR/build"
    fi

    cd "$SCRIPT_DIR/build"

    cmake --fresh .. -G "Unix Makefiles" -DARCH=x86_64 #--debug-output

    time make
}

clean() {
    if [ -d "$SCRIPT_DIR/build" ]; then
        _print "Removing build folder"
        rm -rf "$SCRIPT_DIR/build"
    fi

    if [ -d "$SCRIPT_DIR/iso_root" ]; then
        _print "Removing ISO folder"
        rm -rf "$SCRIPT_DIR/iso_root"
    fi

    if [ -f "$SCRIPT_DIR/barebones.iso" ]; then
        _print "Removing barebones.iso"
        rm -rf "$SCRIPT_DIR/barebones.iso"
    fi
}

clean_deps() {
    if [ -d "$SCRIPT_DIR/deps/stivale" ]; then
        _print "Cleaning stivale"
        rm -rf "$SCRIPT_DIR/deps/stivale"
    fi

    if [ -d "$SCRIPT_DIR/deps/limine" ]; then
        _print "Cleaning limine"
        rm -rf "$SCRIPT_DIR/deps/limine"
    fi
}

iso() {
    _require_tools make cc xorriso

    if [ ! -f "$SCRIPT_DIR/build/kernel.elf" ]; then
        _err "kernel.elf not found, run './build.sh build' first."
        exit 1
    fi

    _print "Creating ISO"
    make -B -C "$SCRIPT_DIR/deps/limine" limine
    rm -rf "$SCRIPT_DIR/iso_root"
    mkdir -p "$SCRIPT_DIR/iso_root"
    cp "$SCRIPT_DIR/build/kernel.elf" "$SCRIPT_DIR/limine.conf" "$SCRIPT_DIR/deps/limine/limine-bios.sys" "$SCRIPT_DIR/deps/limine/limine-bios-cd.bin" "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" "$SCRIPT_DIR/iso_root/"
    mkdir -p "$SCRIPT_DIR/iso_root/EFI/BOOT"
    cp -v "$SCRIPT_DIR/deps/limine/BOOTX64.EFI" "$SCRIPT_DIR/iso_root/EFI/BOOT/"
    cp -v "$SCRIPT_DIR/deps/limine/BOOTIA32.EFI" "$SCRIPT_DIR/iso_root/EFI/BOOT/"

    xorriso -as mkisofs -R -r -J -b limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "$SCRIPT_DIR/iso_root" -o "$SCRIPT_DIR/barebones.iso"
    "$SCRIPT_DIR/deps/limine/limine" bios-install "$SCRIPT_DIR/barebones.iso"
    rm -rf "$SCRIPT_DIR/iso_root"
}

run() {
    _require_tools qemu-system-x86_64

    if [ ! -f "$SCRIPT_DIR/barebones.iso" ]; then
        _err "barebones.iso not found, run './build.sh iso' first."
        exit 1
    fi

    _print "Run in QEMU"
    qemu-system-x86_64 -m 2G -serial stdio -cdrom "$SCRIPT_DIR/barebones.iso"
    # qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $SCRIPT_DIR/barebones.iso

}

smoke_test() {
    _require_tools python3 qemu-system-x86_64

    if [ ! -f "$SCRIPT_DIR/barebones.iso" ]; then
        _err "barebones.iso not found, run './build.sh test' to build and test."
        exit 1
    fi

    python3 "$SCRIPT_DIR/tools/smoke_test.py" "$SCRIPT_DIR/barebones.iso"
}

__exit() {
    cd "$SCRIPT_DIR" || return
}

trap __exit EXIT

if [ "$#" == 0 ]; then
    build
else
    while test $# -gt 0; do
        case "$1" in
            clean)
                clean
                ;;
            fullclean)
                clean
                clean_deps
                ;;
            build)
                build
                ;;
            iso)
                iso
                ;;
            run)
                run
                ;;
            test)
                build
                iso
                smoke_test
                ;;
            debug)
                build
                iso
                run
                ;;
            all)
                clean
                build
                iso
                run
                ;;
            *)
                _err "Unknown action: $1"
                exit 1
                ;;
        esac
        shift
    done
fi
