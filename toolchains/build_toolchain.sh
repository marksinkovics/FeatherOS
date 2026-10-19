#!/bin/bash

set -e

PREFIX_BASE="${PREFIX_BASE:-$HOME/.local/cross}"
BIN_PATH="${BIN_PATH:-$HOME/.local/bin}"

BINUTILS_VERSION="2.37"
GCC_VERSION="14.1.0"
SUPPORTED_ARCHS=(i686-elf x86_64-elf aarch64-elf i386-elf)
HOST_CC="${HOST_CC:-${CC:-}}"
HOST_CFLAGS="${HOST_CFLAGS:-${CFLAGS:-}}"
if [[ "$(uname -s)" == Darwin ]]; then
    HOST_CC="${HOST_CC:-clang}"
    if [[ -z "${SDKROOT:-}" ]]; then
        if ! command -v xcrun >/dev/null 2>&1; then
            echo "ERROR: xcrun is required to locate the macOS SDK." >&2
            exit 1
        fi
        SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
    fi
    export SDKROOT
    if [[ -z "$HOST_CFLAGS" && ("$HOST_CC" == cc || "$HOST_CC" == clang) ]]; then
        HOST_CFLAGS="-O2 -std=gnu89"
    fi
else
    HOST_CC="${HOST_CC:-cc}"
fi

ORIGIN="$(pwd)"
TMP="$(mktemp -d /tmp/cross-compiler.XXXXXX)"

#
#   Preparation & Cleanup
#

__prepare() {
    echo "Validation prefix folder ($PREFIX_BASE)"
    mkdir -p "$PREFIX_BASE" "$BIN_PATH"

    echo "Prepare $TMP"
    mkdir -p "$TMP"

    echo "Navigate to tmp: $TMP"
    mkdir -p "$TMP/src"
    cd "$TMP" || return
}

__cleanup() {
    local exit_code=$?
    if [[ "$exit_code" -ne 0 && "${KEEP_TMP_ON_FAILURE:-0}" == 1 ]]; then
        echo "Preserving failed build files at $TMP"
    else
        echo "Cleaning up... $TMP"
        [[ -d "$TMP" ]] && rm -rf "$TMP"
    fi
    echo "Navigate back to $ORIGIN"
    cd "$ORIGIN" || true
    return "$exit_code"
}

_print() {
    printf '[DEBUG] %s\n' "$*"
}

trap __cleanup EXIT

#
#   Installation steps
#

validate-binutils() {
    local target="$1"
    test -x "$BIN_PATH/$target-as" && test -x "$BIN_PATH/$target-ld"
}

install-binutils() {
    TARGET="$1"
    PREFIX="$PREFIX_BASE/binutils"

    local configure_args=(--target="$TARGET" --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror)
    local compiler_env=("CC=$HOST_CC")
    if [[ "$(uname -s)" == Darwin ]]; then
        configure_args+=(--with-system-zlib)
    fi
    if [[ -n "$HOST_CFLAGS" ]]; then
        compiler_env+=("CFLAGS=$HOST_CFLAGS")
    fi

    _print "Using host C compiler: $HOST_CC"
    mkdir -p "$TMP/$TARGET"
    cd "$TMP/$TARGET" || exit 1
    wget https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz
    tar Jxf binutils-$BINUTILS_VERSION.tar.xz
    mkdir build-binutils
    cd build-binutils || exit 1
    env "${compiler_env[@]}" ../binutils-$BINUTILS_VERSION/configure "${configure_args[@]}"
    make
    make install

    ln -sf "$PREFIX_BASE/binutils/bin/"* "$BIN_PATH/"
}

validate-gcc() {
    local target="$1"
    test -x "$BIN_PATH/$target-gcc" && test -x "$BIN_PATH/$target-g++"
}

install-gcc() {
    TARGET="$1"
    _print "Using host C compiler: $HOST_CC"

    REQUIREMENTS=(
        "$BIN_PATH/$TARGET-as"
        "$BIN_PATH/$TARGET-ld"
    )

    # Validate the existence of as and ld command
    for r in "${REQUIREMENTS[@]}"; do
        if [ ! -f "$r" ]; then
            echo "Aborted. Missing requirement: $r"
            exit 1
        fi
    done

    PREFIX="$PREFIX_BASE/gcc"
    mkdir -p "$TMP/$TARGET"
    cd "$TMP/$TARGET" || exit 1
    wget https://ftp.lip6.fr/pub/gcc/releases/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz
    tar Jxf gcc-$GCC_VERSION.tar.xz

    if [ ! -x "$BIN_PATH/$TARGET-as" ]; then
        echo "Aborted. Missing assembler: $BIN_PATH/$TARGET-as" >&2
        exit 1
    fi

    _print "Download prerequisites"
    _print "$(pwd)"
    cd "$TMP/$TARGET"/gcc-$GCC_VERSION || exit 1
    _print "$(pwd)"
    ./contrib/download_prerequisites
    cd "$TMP/$TARGET" || exit 1
    _print "$(pwd)"

    mkdir build-gcc
    cd build-gcc || exit 1
    _print "Compile GCC"
    local compiler_env=("CC=$HOST_CC")
    if [[ -n "$HOST_CFLAGS" ]]; then
        compiler_env+=("CFLAGS=$HOST_CFLAGS")
    fi
    local languages="c,c++"
    local configure_args=(
        --target="$TARGET"
        --prefix="$PREFIX"
        --disable-nls
        --without-isl
        --without-header
        --with-as="$BIN_PATH/$TARGET-as"
        --with-ld="$BIN_PATH/$TARGET-ld"
        "--enable-languages=$languages"
    )
    if [[ "$(uname -s)" == Darwin ]]; then
        configure_args+=(--with-system-zlib)
    fi
    env "${compiler_env[@]}" ../gcc-$GCC_VERSION/configure "${configure_args[@]}"

    make all-gcc
    make install-gcc
    make all-target-libgcc
    make install-target-libgcc

    _print "Linking gcc"
    ln -sf "$PREFIX_BASE/gcc/bin/"* "$BIN_PATH/"
}

#
#   Command line argument handlers
#

ARCHS=()
FORCE=0
VERBOSE=0
GCC=0
BINUTILS=0

usage() {
    echo "$(basename "$0") [-h] [--gcc] [--binutils] [-a|--arch arch] [-v*] [-f|--force]"
    echo "Build and install a cross-toolchain for the selected target."
    echo "where:"
    echo "    -h,help      show this help text"
    echo "    --gcc         build and install version $GCC_VERSION"
    echo "    --binutils    build and install version $BINUTILS_VERSION"
    echo "    -f,-force     force the installation even if the program with the given version is installed"
    echo "    -a,--arch     architecture"
    echo "          OPTIONS:"
    printf '              - %s\n' "${SUPPORTED_ARCHS[@]}"
    echo "    Example: $0 --binutils --gcc --arch aarch64-elf"
    echo "    -v,--verbose"
    echo ""
}

while [[ "$1" == -* ]]; do
    case "$1" in
        -f | --force)
            FORCE=1
            ;;
        -v*)
            ((VERBOSE += ${#1} - 1))
            ;;
        --verbose)
            ((VERBOSE += 1))
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --gcc)
            GCC=1
            ;;
        --binutils)
            BINUTILS=1
            ;;
        -a | --arch)
            shift
            ARCHS+=("$1")
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Unrecognized option $1."
            usage
            exit 1
            ;;
    esac
    shift
done

if ((${#ARCHS[@]} == 0)); then
    echo "ERROR: select at least one target with --arch." >&2
    usage >&2
    exit 1
fi

if [[ "$BINUTILS" == 0 && "$GCC" == 0 ]]; then
    echo "ERROR: select --binutils, --gcc, or both." >&2
    usage >&2
    exit 1
fi

for arch in "${ARCHS[@]}"; do
    supported=false
    for supported_arch in "${SUPPORTED_ARCHS[@]}"; do
        if [[ "$arch" == "$supported_arch" ]]; then
            supported=true
            break
        fi
    done
    if [[ "$supported" == false ]]; then
        echo "ERROR: unsupported target: $arch" >&2
        usage >&2
        exit 1
    fi
done

__prepare

if [ "$BINUTILS" == "1" ]; then
    for arch in "${ARCHS[@]}"; do
        if [[ "$FORCE" == 1 ]] || ! validate-binutils "$arch"; then
            _print "Installing binutils for $arch"
            install-binutils "$arch"
        else
            _print "binutils for $arch is already installed. Skip"
        fi
    done
fi

if [ "$GCC" == "1" ]; then
    for arch in "${ARCHS[@]}"; do
        if [[ "$FORCE" == 1 ]] || ! validate-gcc "$arch"; then
            _print "Installing gcc for $arch"
            install-gcc "$arch"
        else
            _print "gcc for $arch is already installed. Skip"
        fi
    done
fi
