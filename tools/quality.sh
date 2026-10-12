#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
if command -v brew >/dev/null 2>&1; then
    LLVM_BIN="$(brew --prefix llvm 2>/dev/null)/bin"
    if [[ -x "$LLVM_BIN/clang-tidy" ]]; then
        PATH="$LLVM_BIN:$PATH"
        export PATH
    fi
fi

SHELL_FILES=(
    "$REPO_ROOT/build.sh"
    "$REPO_ROOT/toolchains/build_toolchain.sh"
    "$REPO_ROOT/tools/gen_font.sh"
    "$REPO_ROOT/tools/quality.sh"
)

fail() {
    printf 'ERROR: %s\n' "$1" >&2
    exit 1
}

require_tools() {
    local missing=()
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done

    if ((${#missing[@]} > 0)); then
        fail "missing tools: ${missing[*]}"
    fi
}

format_sources() {
    find "$REPO_ROOT/src" -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -print0 | xargs -0 clang-format -i
    ruff format "$REPO_ROOT/tools"
    shfmt -w -i 4 -ci -bn "${SHELL_FILES[@]}"
}

run_check() {
    local label="$1"
    shift

    printf '%-24s' "$label"
    if "$@" >"$CHECK_OUTPUT" 2>&1; then
        printf 'PASS\n'
    else
        printf 'FAIL\n'
        cat "$CHECK_OUTPUT" >&2
        return 1
    fi
}

check_clang_format() {
    find "$REPO_ROOT/src" -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -print0 | xargs -0 clang-format --dry-run --Werror
}

check_clang_tidy() {
    local arch
    arch="$(sed -n 's/^ARCH:[^=]*=//p' "$REPO_ROOT/build/CMakeCache.txt" | tail -n 1)"
    if [[ -z "$arch" || ! -d "$REPO_ROOT/src/kernel/arch/$arch" ]]; then
        fail "could not determine active ARCH from build/CMakeCache.txt"
    fi

    {
        find "$REPO_ROOT/src" -type f \
            \( -name '*.c' -o -name '*.cpp' \) \
            ! -path "$REPO_ROOT/src/kernel/arch/*" \
            -print0
        find "$REPO_ROOT/src/kernel/arch/$arch" -type f \
            \( -name '*.c' -o -name '*.cpp' \) \
            -print0
    } | xargs -0 clang-tidy -p "$REPO_ROOT/build"
}

check_sources() {
    if [[ ! -f "$REPO_ROOT/build/compile_commands.json" ]]; then
        fail "compile_commands.json not found; run './build.sh build' first"
    fi

    CHECK_OUTPUT="$(mktemp)"
    trap 'rm -f "$CHECK_OUTPUT"' EXIT

    run_check "clang-format check" check_clang_format
    run_check "clang-tidy check" check_clang_tidy
    run_check "ruff check" ruff check "$REPO_ROOT/tools"
    run_check "ruff format check" ruff format --check "$REPO_ROOT/tools"
    run_check "shellcheck" shellcheck -x "${SHELL_FILES[@]}"
    run_check "shfmt check" shfmt -d -i 4 -ci -bn "${SHELL_FILES[@]}"
}

case "${1:-}" in
    format)
        require_tools clang-format ruff shfmt
        format_sources
        ;;
    check)
        require_tools clang-format clang-tidy ruff shellcheck shfmt
        check_sources
        ;;
    *)
        fail "usage: $0 {format|check}"
        ;;
esac
