#!/bin/bash

set -e
# set -x

SCRIPT_PATH=`realpath "$0"`
SCRIPT_DIR=`dirname "$SCRIPT_PATH"`

_print() {
    echo "[DEBUG] $@"
}

build() {
    if [ ! -d "$SCRIPT_DIR/deps/stivale" ]; then
        _print "Fetching stivale"
        git clone -b master https://github.com/stivale/stivale.git deps/stivale
    fi

    if [ ! -d "$SCRIPT_DIR/deps/limine" ]; then
    _print "Fetching limine"
        git clone -b v3.0-branch-binary https://github.com/limine-bootloader/limine.git deps/limine
        make -C deps/limine
    fi

    if [ ! -d "$SCRIPT_DIR/build" ]; then
        _print "Creating build folder"
        mkdir -p "$SCRIPT_DIR/build"
    fi

    cd "$SCRIPT_DIR/build"

    cmake .. -G "Unix Makefiles" -DARCH=x86_64 #--debug-output

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
    _print "Creating ISO"
	rm -rf "$SCRIPT_DIR/iso_root"
	mkdir -p "$SCRIPT_DIR/iso_root"
	cp 	"$SCRIPT_DIR/build/kernel.elf" "$SCRIPT_DIR/limine.cfg" "$SCRIPT_DIR/deps/limine/limine.sys" "$SCRIPT_DIR/deps/limine/limine-cd.bin" "$SCRIPT_DIR/deps/limine/limine-cd-efi.bin" "$SCRIPT_DIR/iso_root/"

    xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-cd-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		"$SCRIPT_DIR/iso_root" -o "$SCRIPT_DIR/barebones.iso"
	"$SCRIPT_DIR/deps/limine/limine-deploy" "$SCRIPT_DIR/barebones.iso"
	rm -rf "$SCRIPT_DIR/iso_root"
}

run() {
    _print "Run in QEMU"
	qemu-system-x86_64 -m 2G -cdrom "$SCRIPT_DIR/barebones.iso"
}

__exit()
{
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
            all)
                clean
                build
                iso
                run
                ;;
        esac
        shift
    done
fi