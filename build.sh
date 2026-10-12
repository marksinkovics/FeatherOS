#!/bin/bash

set -e
# set -x

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
ARCH="${ARCH:-}"
PATH="$HOME/.local/bin:$PATH"
export PATH

_print() {
    printf '[DEBUG] %s\n' "$*"
}

_err() {
    printf '[ERROR] %s\n' "$*" >&2
}

_usage() {
    printf 'Usage: %s [--arch <x86_64|aarch64>] <action> [actions...]\n' "$0"
    printf '   or: ARCH=<x86_64|aarch64> %s <action> [actions...]\n' "$0"
    printf 'Actions: build, iso, run, test, debug, all, clean, fullclean\n'
}

_require_arch() {
    case "$ARCH" in
        x86_64 | aarch64)
            ;;
        "")
            _err "Architecture is required; use --arch <x86_64|aarch64> or set ARCH."
            exit 1
            ;;
        *)
            _err "Unsupported ARCH='$ARCH'. Choose x86_64 or aarch64."
            exit 1
            ;;
    esac
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

_kernel_iso() {
    case "$ARCH" in
        x86_64)
            printf '%s/barebones.iso\n' "$SCRIPT_DIR"
            ;;
        aarch64)
            printf '%s/barebones-aarch64.iso\n' "$SCRIPT_DIR"
            ;;
        *)
            _err "Unsupported ARCH='$ARCH'. Choose x86_64 or aarch64."
            return 1
            ;;
    esac
}

_aarch64_firmware() {
    if [ -n "${QEMU_EFI:-}" ] && [ -f "$QEMU_EFI" ]; then
        printf '%s\n' "$QEMU_EFI"
        return
    fi

    local firmware
    for firmware in \
        /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
        /usr/share/AAVMF/AAVMF_CODE.fd \
        /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
        if [ -f "$firmware" ]; then
            printf '%s\n' "$firmware"
            return
        fi
    done

    return 1
}

build() {
    _require_arch
    _require_tools git make cmake
    case "$ARCH" in
        x86_64)
            _require_tools x86_64-elf-gcc x86_64-elf-g++ x86_64-elf-ld x86_64-elf-ar nasm
            ;;
        aarch64)
            _require_tools aarch64-elf-gcc aarch64-elf-g++ aarch64-elf-ld aarch64-elf-ar
            ;;
        *)
            _err "Unsupported ARCH='$ARCH'. Choose x86_64 or aarch64."
            exit 1
            ;;
    esac
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

    cmake --fresh .. -G "Unix Makefiles" -DARCH="$ARCH"

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

    rm -f "$SCRIPT_DIR"/barebones*.iso
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
    _require_arch
    _require_tools make cc xorriso

    if [ ! -f "$SCRIPT_DIR/build/kernel.elf" ]; then
        _err "kernel.elf not found, run './build.sh build' first."
        exit 1
    fi

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools mcopy mdel
        local iso_path
        iso_path=$(_kernel_iso)
        local arm_uefi_image="$SCRIPT_DIR/iso_root/limine-uefi-aarch64.bin"
        _print "Creating AArch64 UEFI ISO"
        make -B -C "$SCRIPT_DIR/deps/limine" limine
        rm -rf "$SCRIPT_DIR/iso_root"
        mkdir -p "$SCRIPT_DIR/iso_root/EFI/BOOT"
        cp "$SCRIPT_DIR/build/kernel.elf" "$SCRIPT_DIR/limine.conf" \
            "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" "$SCRIPT_DIR/iso_root/"
        cp "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" "$arm_uefi_image"
        mcopy -o -i "$arm_uefi_image" "$SCRIPT_DIR/deps/limine/BOOTAA64.EFI" \
            ::/EFI/BOOT/BOOTAA64.EFI
        mdel -i "$arm_uefi_image" \
            ::/EFI/BOOT/BOOTIA32.EFI \
            ::/EFI/BOOT/BOOTX64.EFI \
            ::/EFI/BOOT/BOOTRISCV64.EFI \
            ::/EFI/BOOT/BOOTLOONGARCH64.EFI
        xorriso -as mkisofs -R -r -J --efi-boot limine-uefi-aarch64.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            "$SCRIPT_DIR/iso_root" -o "$iso_path"
        rm -rf "$SCRIPT_DIR/iso_root"
        return
    elif [ "$ARCH" = "x86_64" ]; then
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
    else
        _err "ISO creation is not supported for ARCH='$ARCH'."
        exit 1
    fi
}

run() {
    _require_arch
    local iso_path
    iso_path=$(_kernel_iso)

    if [ ! -f "$iso_path" ]; then
        _err "$(basename "$iso_path") not found, run './build.sh iso' first."
        exit 1
    fi

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools qemu-system-aarch64
        local firmware
        firmware=$(_aarch64_firmware) || {
            _err "Set QEMU_EFI to an AArch64 UEFI firmware image."
            exit 1
        }
        _require_tools python3
        _print "Run AArch64 in QEMU (serial log: build/aarch64-serial.log)"
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch aarch64 \
            --firmware "$firmware" \
            --log-file "$SCRIPT_DIR/build/aarch64-serial.log" \
            "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64 python3
        _print "Run x86_64 in QEMU (serial log: build/x86_64-serial.log)"
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch x86_64 \
            --log-file "$SCRIPT_DIR/build/x86_64-serial.log" \
            "$iso_path"
    else
        _err "QEMU launch is not supported for ARCH='$ARCH'."
        exit 1
    fi
}

smoke_test() {
    _require_arch
    _require_tools python3

    local iso_path
    iso_path=$(_kernel_iso)
    if [ ! -f "$iso_path" ]; then
        _err "$(basename "$iso_path") not found, run './build.sh test' to build and test."
        exit 1
    fi

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools qemu-system-aarch64
        local firmware
        firmware=$(_aarch64_firmware) || {
            _err "Set QEMU_EFI to an AArch64 UEFI firmware image."
            exit 1
        }
        python3 "$SCRIPT_DIR/tools/smoke_test.py" --arch aarch64 \
            --firmware "$firmware" "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64
        python3 "$SCRIPT_DIR/tools/smoke_test.py" --arch x86_64 "$iso_path"
    else
        _err "Smoke test is not supported for ARCH='$ARCH'."
        exit 1
    fi
}

__exit() {
    cd "$SCRIPT_DIR" || return
}

trap __exit EXIT

declare -a ACTIONS=()
while (($# > 0)); do
    case "$1" in
        --arch | -a)
            if (($# < 2)); then
                _err "Missing value for $1."
                _usage >&2
                exit 1
            fi
            ARCH="$2"
            shift 2
            ;;
        --arch=*)
            ARCH="${1#*=}"
            shift
            ;;
        --help | -h)
            _usage
            exit 0
            ;;
        *)
            ACTIONS+=("$1")
            shift
            ;;
    esac
done

if ((${#ACTIONS[@]} == 0)); then
    _err "An action and architecture must be specified."
    _usage >&2
    exit 1
fi

for action in "${ACTIONS[@]}"; do
    case "$action" in
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
            _err "Unknown action: $action"
            _usage >&2
            exit 1
            ;;
    esac
done
