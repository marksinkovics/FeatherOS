#!/bin/bash

set -e
# set -x

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
ARCH="${ARCH:-}"
LIMINE_TAG="v8.7.0-binary"
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
    printf 'Actions: build, iso, run, test, test-all, unit-test, debug, all, clean, fullclean\n'
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

_build_dir() {
    printf '%s/build/%s\n' "$SCRIPT_DIR" "$ARCH"
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
    _require_tools git make cmake ninja
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
        git clone --branch="$LIMINE_TAG" --depth=1 https://github.com/limine-bootloader/limine.git deps/limine
        make -C deps/limine
    fi

    local build_dir
    build_dir=$(_build_dir)
    mkdir -p "$build_dir"
    cd "$build_dir"

    cmake --fresh "$SCRIPT_DIR" -G Ninja -DARCH="$ARCH" \
        -DFEATHEROS_PANIC_TEST="${PANIC_TEST:-OFF}" \
        -DFEATHEROS_EXCEPTION_TEST="${EXCEPTION_TEST:-OFF}" \
        -DFEATHEROS_INTERRUPT_TEST="${INTERRUPT_TEST:-OFF}" \
        -DFEATHEROS_MEMORY_TEST="${MEMORY_TEST:-OFF}" \
        -DFEATHEROS_TIMER_TEST="${TIMER_TEST:-OFF}" \
        -DFEATHEROS_SCHEDULER_TEST="${SCHEDULER_TEST:-OFF}" \
        -DFEATHEROS_USER_PREEMPTION_TEST="${USER_PREEMPTION_TEST:-OFF}" \
        -DFEATHEROS_SMP_TEST="${SMP_TEST:-OFF}"

    time cmake --build . --parallel
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
    _require_tools make cc mcopy mformat python3 tar xorriso

    local build_dir
    build_dir=$(_build_dir)
    if [ ! -f "$build_dir/kernel.elf" ]; then
        _err "kernel.elf not found, run './build.sh build' first."
        exit 1
    fi
    local initramfs_path="$build_dir/initramfs.tar"
    local block_image="$build_dir/virtio-disk.img"
    local user_compiler
    local user_linker_script
    local -a user_arch_flags=()
    if [ "$ARCH" = "x86_64" ]; then
        _require_tools x86_64-elf-g++
        user_compiler=x86_64-elf-g++
        user_linker_script="$SCRIPT_DIR/src/user/linker_x86_64.ld"
        # Context switches do not save SSE state, so user code must not use it.
        user_arch_flags=(-mno-red-zone -mcmodel=large -mgeneral-regs-only)
    else
        _require_tools aarch64-elf-g++
        user_compiler=aarch64-elf-g++
        user_linker_script="$SCRIPT_DIR/src/user/linker_aarch64.ld"
        user_arch_flags=(-mcpu=generic -march=armv8-a+nofp+nosimd -mcmodel=small)
    fi
    tar -cf "$initramfs_path" -C "$SCRIPT_DIR/initramfs" hello.txt
    local user_program
    for user_program in test:test_program shell:shell calc:calc echo:echo upper:upper date:date fanout:fanout memtest:memtest sleep:sleep hardening:hardening aslr:aslr; do
        local user_elf="${user_program%%:*}-$ARCH.elf"
        "$user_compiler" \
            -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector \
            -fno-pic -fno-pie -nostdlib -nodefaultlibs -static \
            -Wl,-T,"$user_linker_script" -Wl,-e,_start -Wl,--build-id=none \
            -Wl,-z,max-page-size=0x1000 "${user_arch_flags[@]}" \
            -I"$SCRIPT_DIR/src/user" "$SCRIPT_DIR/src/user/${user_program#*:}.cpp" \
            -o "$build_dir/$user_elf"
        tar -rf "$initramfs_path" -C "$build_dir" "$user_elf"
    done
    python3 "$SCRIPT_DIR/tools/create_bad_elf.py" --arch "$ARCH" "$build_dir/badelf-$ARCH.elf"
    tar -rf "$initramfs_path" -C "$build_dir" "badelf-$ARCH.elf"
    python3 "$SCRIPT_DIR/tools/create_test_disk.py" "$block_image" \
        "$SCRIPT_DIR/initramfs/block.txt"

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools mcopy mdel qemu-system-aarch64
        local iso_path
        local dtb_path
        iso_path=$(_kernel_iso)
        dtb_path="$(_build_dir)/virt.dtb"
        local arm_uefi_image="$SCRIPT_DIR/iso_root/limine-uefi-aarch64.bin"
        _print "Creating AArch64 UEFI ISO"
        qemu-system-aarch64 \
            -machine "virt,dumpdtb=$dtb_path" \
            -cpu cortex-a72 \
            -m 2G \
            -global virtio-mmio.force-legacy=false \
            -device virtio-keyboard-device \
            -drive "if=none,id=featheros_disk,file=$block_image,format=raw" \
            -device virtio-blk-device,drive=featheros_disk
        if [ ! -s "$dtb_path" ]; then
            _err "QEMU did not generate the AArch64 device tree."
            exit 1
        fi
        make -B -C "$SCRIPT_DIR/deps/limine" limine
        rm -rf "$SCRIPT_DIR/iso_root"
        mkdir -p "$SCRIPT_DIR/iso_root/EFI/BOOT"
        cp "$build_dir/kernel.elf" "$initramfs_path" \
            "$SCRIPT_DIR/configs/aarch64/limine.conf" \
            "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" "$SCRIPT_DIR/iso_root/"
        cp "$dtb_path" "$SCRIPT_DIR/iso_root/virt.dtb"
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
        cp "$build_dir/kernel.elf" "$initramfs_path" "$SCRIPT_DIR/configs/x86_64/limine.conf" "$SCRIPT_DIR/deps/limine/limine-bios.sys" "$SCRIPT_DIR/deps/limine/limine-bios-cd.bin" "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" "$SCRIPT_DIR/iso_root/"
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
        local build_dir
        build_dir=$(_build_dir)
        _print "Run AArch64 in QEMU (serial log: build/aarch64/serial.log)"
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch aarch64 \
            --firmware "$firmware" \
            --disk "$build_dir/virtio-disk.img" \
            --log-file "$build_dir/serial.log" \
            "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64 python3
        local build_dir
        build_dir=$(_build_dir)
        _print "Run x86_64 in QEMU (serial log: build/x86_64/serial.log)"
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch x86_64 \
            --disk "$build_dir/virtio-disk.img" \
            --log-file "$build_dir/serial.log" \
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
    local -a smoke_args=(--arch "$ARCH")
    if [[ "${PANIC_TEST:-OFF}" == ON || "${PANIC_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-panic)
    fi
    if [[ "${EXCEPTION_TEST:-OFF}" == ON || "${EXCEPTION_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-exception)
    fi
    if [[ "${INTERRUPT_TEST:-OFF}" == ON || "${INTERRUPT_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-interrupt)
    fi
    if [[ "${MEMORY_TEST:-OFF}" == ON || "${MEMORY_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-memory)
    fi
    if [[ "${TIMER_TEST:-OFF}" == ON || "${TIMER_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-timer)
    fi
    if [[ "${SCHEDULER_TEST:-OFF}" == ON || "${SCHEDULER_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-scheduler)
    fi
    if [[ "${SMP_TEST:-OFF}" == ON || "${SMP_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-smp)
    fi
    if [[ "${USER_PREEMPTION_TEST:-OFF}" == ON || "${USER_PREEMPTION_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-user-preemption)
    fi
    if [[ "${CALC_TEST:-OFF}" == ON || "${CALC_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-calc)
    fi
    if [[ "${SHELL_TEST:-OFF}" == ON || "${SHELL_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-shell)
    fi
    if [[ "${INPUT_TEST:-OFF}" == ON || "${INPUT_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-input)
    fi

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
        smoke_args+=(--firmware "$firmware")
        local build_dir
        build_dir=$(_build_dir)
        smoke_args+=(--disk "$build_dir/virtio-disk.img")
        python3 "$SCRIPT_DIR/tools/smoke_test.py" "${smoke_args[@]}" "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64
        local build_dir
        build_dir=$(_build_dir)
        smoke_args+=(--disk "$build_dir/virtio-disk.img")
        python3 "$SCRIPT_DIR/tools/smoke_test.py" "${smoke_args[@]}" "$iso_path"
    else
        _err "Smoke test is not supported for ARCH='$ARCH'."
        exit 1
    fi
}

unit_test() {
    _require_tools cmake git ninja

    local unit_build_dir="$SCRIPT_DIR/build/unit-tests-ninja"
    cmake --fresh -S "$SCRIPT_DIR/tests/unit" -B "$unit_build_dir" -G Ninja
    cmake --build "$unit_build_dir" --parallel
    ctest --test-dir "$unit_build_dir" --output-on-failure
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
        test-all)
            _require_tools python3
            cd "$SCRIPT_DIR"
            python3 -m unittest discover -s tests -p 'test_*.py' -v
            ;;
        unit-test)
            unit_test
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
