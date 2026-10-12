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
    ruff format "$REPO_ROOT/tools" "$REPO_ROOT/tests"
    shfmt -w -i 4 -ci -bn "${SHELL_FILES[@]}"
}

run_check() {
    local label="$1"
    local output_file
    shift

    output_file="$(mktemp)"
    if "$@" >"$output_file" 2>&1; then
        rm -f "$output_file"
        return 0
    else
        printf 'ERROR: %s failed\n' "$label" >&2
        cat "$output_file" >&2
        rm -f "$output_file"
        return 1
    fi
}

check_clang_format() {
    find "$REPO_ROOT/src" -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -print0 | xargs -0 clang-format --dry-run --Werror
}

check_clang_tidy() {
    local arch="$1"
    local clang_tidy_args=(-p "$REPO_ROOT/build/$arch")
    if [[ "${2:-}" == "--fix" ]]; then
        clang_tidy_args=(--fix "${clang_tidy_args[@]}")
    fi
    if [[ ! -d "$REPO_ROOT/src/kernel/arch/$arch" ]]; then
        fail "unsupported architecture for clang-tidy: $arch"
    fi

    {
        find "$REPO_ROOT/src" -type f \
            \( -name '*.c' -o -name '*.cpp' \) \
            ! -path "$REPO_ROOT/src/kernel/arch/*" \
            -print0
        find "$REPO_ROOT/src/kernel/arch/$arch" -type f \
            \( -name '*.c' -o -name '*.cpp' \) \
            -print0
    } | xargs -0 clang-tidy "${clang_tidy_args[@]}"
}

check_sources() {
    local fix="$1"
    local built_arches=()
    local arch
    for arch in x86_64 aarch64; do
        if [[ -f "$REPO_ROOT/build/$arch/compile_commands.json" ]]; then
            built_arches+=("$arch")
        fi
    done
    if ((${#built_arches[@]} == 0)); then
        fail "no compile database found; run './build.sh --arch x86_64 build' first"
    fi

    if [[ "$fix" == true ]]; then
        run_check "format fixes" format_sources
        run_check "Ruff fixes" ruff check --fix "$REPO_ROOT/tools" "$REPO_ROOT/tests"
        for arch in "${built_arches[@]}"; do
            run_check "clang-tidy fixes $arch" check_clang_tidy "$arch" --fix
        done
    fi

    run_check "clang-format check" check_clang_format
    for arch in "${built_arches[@]}"; do
        run_check "clang-tidy $arch" check_clang_tidy "$arch"
    done
    run_check "Ruff check" ruff check "$REPO_ROOT/tools" "$REPO_ROOT/tests"
    run_check "Ruff format check" ruff format --check "$REPO_ROOT/tools" "$REPO_ROOT/tests"
    run_check "ShellCheck" shellcheck -x "${SHELL_FILES[@]}"
    run_check "shfmt check" shfmt -d -i 4 -ci -bn "${SHELL_FILES[@]}"
}

case "${1:-}" in
    format)
        require_tools clang-format ruff shfmt
        format_sources
        ;;
    check)
        require_tools clang-format clang-tidy ruff shellcheck shfmt
        check_sources false
        ;;
    --fix)
        require_tools clang-format clang-tidy ruff shellcheck shfmt
        check_sources true
        ;;
    *)
        fail "usage: $0 {format|check|--fix}"
        ;;
esac
