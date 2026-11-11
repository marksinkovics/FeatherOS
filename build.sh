#!/bin/bash

set -e
# set -x

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
ARCH="${ARCH:-}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
LIMINE_TAG="v8.7.0-binary"
# The Swift runtimes are built from the sources of the Swift release the pinned toolchain came
# from (src/user/swift_concurrency, src/user/swift_runtime).
SWIFT_SOURCE_TAG="swift-$(cat "$SCRIPT_DIR/.swift-version")-RELEASE"
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
    printf 'BUILD_TYPE=Release builds at -O2 (Swift -O) in build/<arch>-release; the default, Debug, at -O0 -g\n'
    printf '(Swift -Onone) in build/<arch>. OUT_DIR puts the ISO, disk image and logs elsewhere than the build folder.\n'
    printf 'KERNEL_TESTS=ON builds the kernel test hooks (test, smoke, test-all and any *_TEST kernel toggle turn it on).\n'
    printf 'Actions: build, iso, run, test, smoke (iso and test without building), test-all, unit-test, benchmark,\n'
    printf '         debug, all, clean, fullclean\n'
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

_build_dir() {
    if [ "$BUILD_TYPE" = Release ]; then
        printf '%s/build/%s-release\n' "$SCRIPT_DIR" "$ARCH"
    else
        printf '%s/build/%s\n' "$SCRIPT_DIR" "$ARCH"
    fi
}

# Where an image and what runs it go, so runs of one build can happen side by side.
_out_dir() {
    printf '%s\n' "${OUT_DIR:-$(_build_dir)}"
}

_kernel_iso() {
    printf '%s/featheros.iso\n' "$(_out_dir)"
}

_is_on() {
    [[ "${1:-OFF}" == ON || "${1:-OFF}" == 1 ]]
}

# The kernel test hook a run selects, from the *_TEST toggles, as the kernel's test= option.
_kernel_test_mode() {
    local -a modes=()
    local toggle variable
    for toggle in PANIC EXCEPTION INTERRUPT MEMORY TIMER SCHEDULER USER_PREEMPTION SMP STORAGE \
        NETWORK INPUT; do
        variable="${toggle}_TEST"
        if _is_on "${!variable:-OFF}"; then
            modes+=("$(printf '%s' "$toggle" | tr '[:upper:]_' '[:lower:]-')")
        fi
    done
    if ((${#modes[@]} > 1)); then
        _err "Choose one kernel test toggle, not ${modes[*]}."
        return 1
    fi
    if ((${#modes[@]} == 1)); then
        printf '%s\n' "${modes[0]}"
    fi
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

# swiftc from a swift.org toolchain: SWIFTC if set, otherwise the toolchain swiftly selects for
# this checkout (.swift-version). Xcode's swiftc does not reliably target ELF, so it is not used.
_swift_compiler() {
    if [ -n "${SWIFTC:-}" ]; then
        printf '%s\n' "$SWIFTC"
        return
    fi
    command -v swiftly >/dev/null 2>&1 || return 1
    local toolchain
    toolchain=$(cd "$SCRIPT_DIR" && swiftly use --print-location 2>/dev/null) || return 1
    [ -x "$toolchain/usr/bin/swiftc" ] || return 1
    printf '%s\n' "$toolchain/usr/bin/swiftc"
}

# Shallow, sparse clones of what the runtime build reads. The Swift sources get
# src/user/swift_concurrency/swift.patch, applied again from a clean tree whenever it changes.
_fetch_swift_runtime_sources() {
    local swift="$SCRIPT_DIR/deps/swift"
    local string_processing="$SCRIPT_DIR/deps/swift-experimental-string-processing"
    local patch="$SCRIPT_DIR/src/user/swift_concurrency/swift.patch"
    local applied="$SCRIPT_DIR/deps/swift.patch.applied"
    if [ ! -d "$swift/.git" ]; then
        _print "Fetching the Swift sources ($SWIFT_SOURCE_TAG)"
        rm -f "$applied"
        git clone --branch="$SWIFT_SOURCE_TAG" --depth=1 --filter=blob:none --no-checkout \
            https://github.com/swiftlang/swift.git "$swift"
    fi
    # Set every time, so a clone made with an older list gets what is added to it.
    git -C "$swift" sparse-checkout set --no-cone /include/swift/ /lib/Demangling/ \
        /lib/Threading/ /stdlib/include/ /stdlib/public/Concurrency/ \
        /stdlib/public/CommandLineSupport/ /stdlib/public/CompatibilityOverride/ \
        /stdlib/public/Cxx/ /stdlib/public/LLVMSupport/ \
        /stdlib/public/SwiftOnoneSupport/ /stdlib/public/SwiftShims/ /stdlib/public/core/ \
        /stdlib/public/runtime/ /stdlib/public/stubs/ /utils/gyb.py /utils/SwiftIntTypes.py \
        /utils/SwiftFloatingPointTypes.py /utils/availability-macros.def
    git -C "$swift" checkout --quiet
    if ! cmp -s "$patch" "$applied"; then
        _print "Patching the Swift sources"
        git -C "$swift" checkout --quiet -- .
        git -C "$swift" apply "$patch"
        cp "$patch" "$applied"
    fi
    if [ ! -d "$string_processing/.git" ]; then
        _print "Fetching swift-experimental-string-processing ($SWIFT_SOURCE_TAG)"
        git clone --branch="$SWIFT_SOURCE_TAG" --depth=1 --filter=blob:none --no-checkout \
            https://github.com/swiftlang/swift-experimental-string-processing.git \
            "$string_processing"
        git -C "$string_processing" sparse-checkout set --no-cone /Sources/_CUnicode/ \
            /Sources/_RegexParser/ /Sources/_StringProcessing/ /Sources/RegexBuilder/
        git -C "$string_processing" checkout --quiet
    fi
}

build() {
    _require_arch
    _require_tools git make cmake ninja
    case "$BUILD_TYPE" in
        Debug | Release) ;;
        *)
            _err "Unsupported BUILD_TYPE='$BUILD_TYPE'. Choose Debug or Release."
            exit 1
            ;;
    esac
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

    local swiftc
    if ! swiftc=$(_swift_compiler); then
        swiftc=""
        _print "No Swift toolchain (swiftly or SWIFTC), so the Swift programs are skipped"
    else
        _fetch_swift_runtime_sources
    fi

    local build_dir
    build_dir=$(_build_dir)
    mkdir -p "$build_dir"
    cd "$build_dir"

    local kernel_tests=OFF test_mode
    test_mode=$(_kernel_test_mode) || exit 1
    if _is_on "${KERNEL_TESTS:-OFF}" || [ -n "$test_mode" ]; then
        kernel_tests=ON
    fi
    _print "Build $ARCH ($BUILD_TYPE, kernel test hooks $kernel_tests)"
    cmake --fresh "$SCRIPT_DIR" -G Ninja -DARCH="$ARCH" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DFEATHEROS_SWIFTC="$swiftc" -DFEATHEROS_KERNEL_TESTS="$kernel_tests"

    time cmake --build . --parallel
    _image_inputs
}

# What every image of a build shares: the initramfs, Limine's tool and, on AArch64, QEMU's device
# tree.
_image_inputs() {
    _require_tools make cc python3 tar "$ARCH-elf-strip"

    local build_dir
    build_dir=$(_build_dir)
    local initramfs_path="$build_dir/initramfs.tar"
    # The CMake build compiles the programs (src/user/CMakeLists.txt) into this directory.
    local programs_dir="$build_dir/programs"
    local -a programs=("$programs_dir"/*-"$ARCH".elf)
    if [ ! -f "${programs[0]}" ]; then
        _err "No user programs in $programs_dir."
        exit 1
    fi
    # The initramfs holds /system, where each program is a file named after it, as "ls", and /etc.
    local root_dir="$build_dir/initramfs"
    local system_dir="$root_dir/system"
    rm -rf "$root_dir" "$build_dir/system"
    mkdir -p "$system_dir"
    cp "$SCRIPT_DIR/initramfs/hello.txt" "$SCRIPT_DIR/initramfs/init.conf" \
        "$SCRIPT_DIR/initramfs/sample.json" "$SCRIPT_DIR/initramfs/http-request.txt" \
        "$SCRIPT_DIR/initramfs/http-response.txt" "$system_dir/"
    cp -R "$SCRIPT_DIR/initramfs/etc" "$root_dir/"
    # Debug builds keep their debug information and symbols in build/<arch>/programs for gdb; the
    # copies the system runs leave both out, so they stay small (Embedded Swift's mangled names
    # alone take a quarter of a Swift program, and debug information three quarters of a
    # full-Swift one).
    local program
    for program in "${programs[@]}"; do
        local name="${program##*/}"
        "$ARCH-elf-strip" --strip-all -o "$system_dir/${name%-"$ARCH".elf}" "$program"
    done
    python3 "$SCRIPT_DIR/tools/create_bad_elf.py" --arch "$ARCH" "$system_dir/badelf"
    # macOS tar would otherwise add a "._name" AppleDouble entry for each file's metadata.
    export COPYFILE_DISABLE=1
    (cd "$root_dir" && tar -cf "$initramfs_path" -- system etc)
    make -B -C "$SCRIPT_DIR/deps/limine" limine

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools qemu-system-aarch64
        local dtb_path="$build_dir/virt.dtb"
        local dtb_disk="$build_dir/dtb-disk.img"
        python3 "$SCRIPT_DIR/tools/create_test_disk.py" "$dtb_disk" "$SCRIPT_DIR/initramfs/block.txt"
        qemu-system-aarch64 \
            -machine "virt,dumpdtb=$dtb_path" \
            -cpu cortex-a72 \
            -m 2G \
            -global virtio-mmio.force-legacy=false \
            -device virtio-keyboard-device \
            -drive "if=none,id=featheros_disk,file=$dtb_disk,format=raw" \
            -device virtio-blk-device,drive=featheros_disk \
            -netdev user,id=featheros_net \
            -device virtio-net-device,netdev=featheros_net
        rm -f "$dtb_disk"
        if [ ! -s "$dtb_path" ]; then
            _err "QEMU did not generate the AArch64 device tree."
            exit 1
        fi
    fi
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

    if [ -d "$SCRIPT_DIR/deps/swift" ] || [ -d "$SCRIPT_DIR/deps/llvm-project" ] \
        || [ -d "$SCRIPT_DIR/deps/swift-experimental-string-processing" ]; then
        _print "Cleaning the Swift and string-processing sources"
        rm -rf "$SCRIPT_DIR/deps/swift" "$SCRIPT_DIR/deps/llvm-project" \
            "$SCRIPT_DIR/deps/swift-experimental-string-processing"
        rm -f "$SCRIPT_DIR/deps/swift.patch.applied"
    fi
}

iso() {
    _require_arch
    _require_tools mcopy mformat python3 xorriso

    local build_dir out_dir test_mode
    build_dir=$(_build_dir)
    out_dir=$(_out_dir)
    if [ ! -f "$build_dir/kernel.elf" ] || [ ! -f "$build_dir/initramfs.tar" ]; then
        _err "No kernel or initramfs in $build_dir; run './build.sh build' first."
        exit 1
    fi
    test_mode=$(_kernel_test_mode) || exit 1
    if [ -n "$test_mode" ] && ! grep -q '^FEATHEROS_KERNEL_TESTS:BOOL=ON$' "$build_dir/CMakeCache.txt"; then
        _err "$build_dir has no kernel test hooks for test=$test_mode; build with KERNEL_TESTS=ON."
        exit 1
    fi
    mkdir -p "$out_dir"
    python3 "$SCRIPT_DIR/tools/create_test_disk.py" "$out_dir/virtio-disk.img" \
        "$SCRIPT_DIR/initramfs/block.txt" --layout "${DISK_LAYOUT:-mbr}"

    local iso_root="$out_dir/iso_root"
    local iso_path
    iso_path=$(_kernel_iso)
    rm -rf "$iso_root"
    mkdir -p "$iso_root/EFI/BOOT"
    cp "$SCRIPT_DIR/configs/$ARCH/limine.conf" "$iso_root/limine.conf"
    if [ -n "$test_mode" ]; then
        printf '\n    cmdline: test=%s\n' "$test_mode" >>"$iso_root/limine.conf"
    fi
    cp "$build_dir/kernel.elf" "$build_dir/initramfs.tar" "$SCRIPT_DIR/deps/limine/limine-uefi-cd.bin" \
        "$iso_root/"

    if [ "$ARCH" = "aarch64" ]; then
        _require_tools mdel
        local arm_uefi_image="$iso_root/limine-uefi-aarch64.bin"
        _print "Creating AArch64 UEFI ISO${test_mode:+ (test=$test_mode)}"
        cp "$build_dir/virt.dtb" "$iso_root/virt.dtb"
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
            "$iso_root" -o "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _print "Creating ISO${test_mode:+ (test=$test_mode)}"
        cp "$SCRIPT_DIR/deps/limine/limine-bios.sys" "$SCRIPT_DIR/deps/limine/limine-bios-cd.bin" \
            "$iso_root/"
        cp "$SCRIPT_DIR/deps/limine/BOOTX64.EFI" "$SCRIPT_DIR/deps/limine/BOOTIA32.EFI" \
            "$iso_root/EFI/BOOT/"
        xorriso -as mkisofs -R -r -J -b limine-bios-cd.bin \
            -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
            -apm-block-size 2048 --efi-boot limine-uefi-cd.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            "$iso_root" -o "$iso_path"
        "$SCRIPT_DIR/deps/limine/limine" bios-install "$iso_path"
    else
        _err "ISO creation is not supported for ARCH='$ARCH'."
        exit 1
    fi
    rm -rf "$iso_root"
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
        local out_dir
        out_dir=$(_out_dir)
        _print "Run AArch64 in QEMU (serial log: $out_dir/serial.log)"
        local run_args=()
        if [[ "${QEMU_NO_REBOOT:-OFF}" == ON || "${QEMU_NO_REBOOT:-OFF}" == 1 ]]; then
            run_args+=(--no-reboot)
        fi
        if [[ "${QEMU_ZOOM:-ON}" != OFF && "${QEMU_ZOOM:-ON}" != 0 ]]; then
            run_args+=(--zoom)
        fi
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch aarch64 \
            --firmware "$firmware" \
            --disk "$out_dir/virtio-disk.img" \
            --log-file "$out_dir/serial.log" \
            "${run_args[@]}" "$iso_path"
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64 python3
        local out_dir
        out_dir=$(_out_dir)
        _print "Run x86_64 in QEMU (serial log: $out_dir/serial.log)"
        local run_args=()
        if [[ "${QEMU_NO_REBOOT:-OFF}" == ON || "${QEMU_NO_REBOOT:-OFF}" == 1 ]]; then
            run_args+=(--no-reboot)
        fi
        if [[ "${QEMU_ZOOM:-ON}" != OFF && "${QEMU_ZOOM:-ON}" != 0 ]]; then
            run_args+=(--zoom)
        fi
        python3 "$SCRIPT_DIR/tools/qemu_run.py" \
            --arch x86_64 \
            --disk "$out_dir/virtio-disk.img" \
            --log-file "$out_dir/serial.log" \
            "${run_args[@]}" "$iso_path"
    else
        _err "QEMU launch is not supported for ARCH='$ARCH'."
        exit 1
    fi
}

smoke_test() {
    _require_arch
    _require_tools python3

    local iso_path build_dir out_dir
    build_dir=$(_build_dir)
    out_dir=$(_out_dir)
    local -a smoke_args=(--arch "$ARCH" --build-dir "$build_dir" --disk "$out_dir/virtio-disk.img"
        --session-output "$out_dir/smoke-output.txt")
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
    if [[ "${NETWORK_TEST:-OFF}" == ON || "${NETWORK_TEST:-OFF}" == 1 ]]; then
        smoke_args+=(--expect-network)
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
    if [[ -n "${QEMU_CPU:-}" ]]; then
        smoke_args+=(--cpu "$QEMU_CPU")
    fi
    if [[ -n "${POWER_TEST:-}" ]]; then
        smoke_args+=(--expect-power "$POWER_TEST")
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
    elif [ "$ARCH" = "x86_64" ]; then
        _require_tools qemu-system-x86_64
    else
        _err "Smoke test is not supported for ARCH='$ARCH'."
        exit 1
    fi

    if [[ "${STORAGE_TEST:-OFF}" == ON || "${STORAGE_TEST:-OFF}" == 1 ]]; then
        # Booting the same disk image twice shows that writes persist across a restart.
        local boot
        for boot in 1 2; do
            python3 "$SCRIPT_DIR/tools/smoke_test.py" "${smoke_args[@]}" \
                --expect-storage-boot "$boot" "$iso_path" || exit 1
        done
    else
        python3 "$SCRIPT_DIR/tools/smoke_test.py" "${smoke_args[@]}" "$iso_path"
    fi
}

benchmark() {
    _require_arch
    _require_tools python3 "qemu-system-$ARCH"

    local iso_path
    iso_path=$(_kernel_iso)
    local -a benchmark_args=(--arch "$ARCH" --disk "$(_out_dir)/virtio-disk.img"
    --programs "$(_build_dir)/programs" --seconds "${BENCHMARK_SECONDS:-1}" --rounds "${BENCHMARK_ROUNDS:-2}")
    if [ "$ARCH" = "aarch64" ]; then
        local firmware
        firmware=$(_aarch64_firmware) || {
            _err "Set QEMU_EFI to an AArch64 UEFI firmware image."
            exit 1
        }
        benchmark_args+=(--firmware "$firmware")
    fi
    python3 "$SCRIPT_DIR/tools/benchmark.py" "${benchmark_args[@]}" "$iso_path"
}

unit_test() {
    _require_tools cmake git ninja

    local unit_build_dir="$SCRIPT_DIR/build/unit-tests-ninja"
    local sanitize="${SANITIZE:-ON}"
    local cmake_args=(-DFEATHEROS_SANITIZERS="$sanitize")
    # Apple clang 17's AddressSanitizer runtime deadlocks before main on macOS 26, so sanitized
    # builds use Homebrew LLVM, which quality.sh already needs for clang-tidy.
    if [ "$sanitize" = ON ] && [ "$(uname -s)" = Darwin ]; then
        local llvm_bin
        llvm_bin="$(brew --prefix llvm 2>/dev/null)/bin"
        if [ ! -x "$llvm_bin/clang++" ]; then
            _err "Sanitized unit tests need Homebrew LLVM (brew install llvm), or set SANITIZE=OFF."
            exit 1
        fi
        cmake_args+=(-DCMAKE_C_COMPILER="$llvm_bin/clang" -DCMAKE_CXX_COMPILER="$llvm_bin/clang++")
    fi
    local swiftc
    if swiftc=$(_swift_compiler); then
        cmake_args+=(-DFEATHEROS_SWIFTC="$swiftc")
        _fetch_swift_runtime_sources
    else
        _print "No Swift toolchain (swiftly or SWIFTC), so the Swift tests are skipped"
    fi
    cmake --fresh -S "$SCRIPT_DIR/tests/unit" -B "$unit_build_dir" -G Ninja "${cmake_args[@]}"
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
            KERNEL_TESTS=ON
            build
            iso
            smoke_test
            ;;
        smoke)
            iso
            smoke_test
            ;;
        test-all)
            _require_tools python3
            python3 "$SCRIPT_DIR/tools/matrix.py"
            ;;
        unit-test)
            unit_test
            ;;
        benchmark)
            BUILD_TYPE=Release
            build
            iso
            benchmark
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
