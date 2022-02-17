#!/bin/bash

PREFIX_BASE="/usr/local/cross"
BIN_PATH="/usr/local/bin"

BINUTILS_VERSION="2.37"
GCC_VERSION="11.2.0"
GRUB_VERSION="2.06"

# TARGETS=(i686-elf x86_64-elf aarch64-elf i386-elf)
TARGETS=(x86_64-elf)
# GRUB_TARGETS=(x86_64)

ORIGIN="$(pwd)"
TMP="$(mktemp -d /tmp/cross-compiler.XXXXXX)"

#
#   Preparation & Cleanup
#

__prepare()
{
    echo "Validation prefix folder ($PREFIX_BASE)"
    if [ ! -d "$PREFIX_BASE" ]; then
        echo "Folder doesn't exist. Creating ($PREFIX_BASE)"
        sudo mkdir -p "$PREFIX_BASE"
        sudo chown -R "$(whoami)" "$PREFIX_BASE"
        sudo chmod 775 "$PREFIX_BASE"
    fi

    echo "Prepare $TMP"
    mkdir -p "$TMP"

    echo "Navigate to tmp: $TMP"
    mkdir -p "$TMP/src"
    cd "$TMP" || return
}

__cleanup ()
{
    echo "Cleaning up... $TMP"
    [[ -d "$TMP" ]] && rm -rf "$TMP"
    echo "Navigate back to $ORIGIN"
    cd "$ORIGIN" || return
}

_print() {
    echo "[DEBUG] $@"
}

trap __cleanup EXIT

#
#   Installation steps
#

validate-binutils()
{
    TARGET="$1"
    test -L "$BIN_PATH/$TARGET-as" && test -L "$BIN_PATH/$TARGET-ld"
}

install-binutils()
{
    TARGET="$1"
    PREFIX="$PREFIX_BASE/binutils"
    mkdir -p "$TMP/$TARGET"
    cd "$TMP/$TARGET"
    wget https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz
    tar Jxf binutils-$BINUTILS_VERSION.tar.xz
    mkdir build-binutils
    cd build-binutils
    ../binutils-$BINUTILS_VERSION/configure --target="$TARGET" --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
    make
    make install

    # link to /usr/local/bin
    cd /usr/local/bin
    ln -sf ../cross/binutils/bin/* ./
}

validate-gcc()
{
    TARGET="$1"
    test -L "$BIN_PATH/$TARGET-gcc"
}

install-gcc()
{
    TARGET="$1"

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

    which -- "$TARGET-as" || exit 1

    mkdir build-gcc
    cd build-gcc || exit 1
    ../gcc-$GCC_VERSION/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --without-isl \
        --without-header \
        --with-as="$BIN_PATH/$TARGET-as" \
        --with-ld="$BIN_PATH/$TARGET-ld" \
        --enable-languages=c,c++

    make all-gcc
    make install-gcc
    make all-target-libgcc
    make install-target-libgcc

    _print "Linking gcc"
    cd "$BIN_PATH" || exit 1
    ln -sf ../cross/gcc/bin/* ./
}

#
#   Command line argument handlers
#

MODES=()
ARCHS=()
FORCE=0
VERBOSE=0
GCC=0
BINUTILS=0

usage()
{
    echo "$(basename "$0") [-h] [--gcc] [--binutils] [-a|--arch arch] [-v*] [-f|--force] -- program to build and installcross-platform developer tools from scratch"
    echo "where:"
    echo "    -h,help      show this help text"
    echo "    --gcc         build and install version $GCC_VERSION"
    echo "    --binutils    build and install version $BINUTILS_VERSION"
    echo "    -f,-force     force the installation even if the program with the given version is installed"
    echo "    -a,--arch     architecture"
    echo "          OPTIONS:"
    echo "              - i686-elf"
    echo "              - x86_64-elf"
    echo "              - aarch64-elf"
    echo "              - i386-elf"
    echo "    -v,--verbose"
    echo ""
}

__prepare

while [[ "$1" == -* ]]; do
    case "$1" in
        -f|--force)
            FORCE=1
            ;;
        -v*)
            (( VERBOSE += ${#1} - 1 ))
            ;;
        --verbose)
            (( VERBOSE++ ))
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --gcc)
            GCC=1
            ;;
        --binutils)
            BINUTILS=1
            ;;
        -a|--arch)
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
            exit 1;
    esac
    shift
done

if [ "$BINUTILS" == "1" ]; then
    for arch in "${ARCHS[@]}"; do
        if ! validate-binutils "$arch"; then
            _print "Installing binutils for $arch"
            install-binutils "$arch"
        else
            _print "binutils for $arch is already installed. Skip"
        fi
    done
fi

if [ "$GCC" == "1" ]; then
    for arch in "${ARCHS[@]}"; do
        if ! validate-gcc "$arch"; then
            _print "Installing gcc for $arch"
            install-gcc "$arch"
        else
            _print "gcc for $arch is already installed. Skip"
        fi
    done
fi