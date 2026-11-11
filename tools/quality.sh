#!/bin/bash
# Format and lint checks, one stage per tool.
#
#   tools/quality.sh check [stage...]   every stage, or only the named ones
#   tools/quality.sh check [stage...] -- file...
#                                       clang-tidy only on the sources those files affect
#   tools/quality.sh --fix              apply fixes, then run every stage
#   tools/quality.sh format             apply formatting only
#   tools/quality.sh stages             list the stage names
#
# Running more than one stage prints one line per stage with its elapsed time; a single stage
# (as each pre-commit hook runs) prints only its diagnostics.

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
    "$REPO_ROOT/tools/check.sh"
    "$REPO_ROOT/tools/quality.sh"
    "$REPO_ROOT/.claude/hooks/format-edited-file.sh"
)

ARCHES=(x86_64 aarch64)
STAGES=(clang-format clang-tidy-x86_64 clang-tidy-aarch64 ruff-check ruff-format shellcheck shfmt)

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

# macOS ships bash 3.2, which has no EPOCHREALTIME.
now() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        printf '%s\n' "${EPOCHREALTIME/,/.}"
    else
        python3 -c 'import time; print(time.time())'
    fi
}

elapsed_since() {
    awk -v start="$1" -v end="$(now)" 'BEGIN { printf "%.1fs", end - start }'
}

# The C and C++ sources, including the C++ standard library's headers, which have no extension.
cxx_sources() {
    find "$REPO_ROOT/src" -type f \
        \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
        -o -path "$REPO_ROOT/src/libcxx/include/*" \) -print0
}

format_sources() {
    cxx_sources | xargs -0 clang-format -i
    ruff format "$REPO_ROOT/tools" "$REPO_ROOT/tests"
    shfmt -w -i 4 -ci -bn "${SHELL_FILES[@]}"
}

check_clang_format() {
    cxx_sources | xargs -0 clang-format --dry-run --Werror
}

# Each clang-tidy run compiles its file, so files are checked in parallel.
job_count() {
    if command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}

# The sources clang-tidy checks for one architecture: the shared ones and that arch's own, as far
# as the build compiled them. The Swift runtimes' files are compiled by custom commands, so they
# have no entry in compile_commands.json; the kernel test hooks have none in a build without
# KERNEL_TESTS, and clang-tidy would only guess their flags.
tidy_sources() {
    local arch="$1"
    {
        find "$REPO_ROOT/src" -type f \
            \( -name '*.c' -o -name '*.cpp' \) \
            ! -path "$REPO_ROOT/src/kernel/arch/*" \
            ! -path "$REPO_ROOT/src/libc/arch/*" \
            ! -path "$REPO_ROOT/src/user/swift_concurrency/*" \
            ! -path "$REPO_ROOT/src/user/swift_runtime/*"
        find "$REPO_ROOT/src/kernel/arch/$arch" "$REPO_ROOT/src/libc/arch/$arch" -type f \
            \( -name '*.c' -o -name '*.cpp' \)
    } | grep -Fxf <(grep -o '"file": *"[^"]*"' "$REPO_ROOT/build/$arch/compile_commands.json" \
        | sed 's/.*"\([^"]*\)"$/\1/') | tr '\n' '\0'
}

# The sources among tidy_sources that the given files can change the diagnostics of, one per
# line: the files themselves and every source whose last build included one of them, from
# Ninja's dependency log. Any other file (.clang-tidy, CMake files), or no usable log, selects
# everything.
affected_tidy_sources() {
    local arch="$1"
    shift
    local file deps changed=""
    for file in "$@"; do
        case "$file" in
            src/*.c | src/*.cpp | src/*.h | src/*.hpp | src/libcxx/include/*)
                changed+="$REPO_ROOT/$file"$'\n'
                ;;
            *)
                tidy_sources "$arch" | tr '\0' '\n'
                return
                ;;
        esac
    done
    if ! deps="$(ninja -C "$REPO_ROOT/build/$arch" -t deps 2>/dev/null)" \
        || [[ "$deps" != *"$REPO_ROOT/src/"* ]]; then
        tidy_sources "$arch" | tr '\0' '\n'
        return
    fi
    # Each record is an unindented object line, then its source and headers indented.
    awk '
        FNR == 1 { file++ }
        file == 1 { changed[$0] = 1; next }
        file == 2 {
            if ($0 !~ /^ /) { source = ""; next }
            sub(/^ +/, "")
            if (source == "") source = $0
            if ($0 in changed) hit[source] = 1
            next
        }
        ($0 in changed) || ($0 in hit)
    ' <(printf '%s' "$changed") <(printf '%s\n' "$deps") <(tidy_sources "$arch" | tr '\0' '\n')
}

# With files, checks only the sources they affect and says how many on descriptor 3, which
# check_stages keeps on its standard output.
check_clang_tidy() {
    local arch="$1"
    shift
    local selected count total status=0
    if (($# == 0)); then
        tidy_sources "$arch" | xargs -0 -n 4 -P "$(job_count)" clang-tidy -p "$REPO_ROOT/build/$arch"
        return
    fi
    selected="$(mktemp)"
    affected_tidy_sources "$arch" "$@" >"$selected"
    count="$(wc -l <"$selected" | tr -d ' ')"
    total="$(tidy_sources "$arch" | tr -cd '\0' | wc -c | tr -d ' ')"
    printf 'clang-tidy %s: %s of %s sources affected\n' "$arch" "$count" "$total" >&3
    if ((count > 0)); then
        tr '\n' '\0' <"$selected" \
            | xargs -0 -n 4 -P "$(job_count)" clang-tidy -p "$REPO_ROOT/build/$arch" \
            || status=$?
    fi
    rm -f "$selected"
    return "$status"
}

# Fixing stays serial: parallel runs fixing the same header would overwrite each other's edits.
# run-clang-tidy avoids that, but it also applies the fixes clang-tidy only suggests in notes,
# such as casts for bugprone-implicit-widening-of-multiplication-result.
fix_clang_tidy() {
    tidy_sources "$1" | xargs -0 clang-tidy --fix -p "$REPO_ROOT/build/$1"
}

is_built() {
    [[ -f "$REPO_ROOT/build/$1/compile_commands.json" ]]
}

stage_label() {
    case "$1" in
        clang-tidy-*) printf 'clang-tidy %s' "${1#clang-tidy-}" ;;
        ruff-check) printf 'Ruff check' ;;
        ruff-format) printf 'Ruff format check' ;;
        shellcheck) printf 'ShellCheck' ;;
        shfmt) printf 'shfmt check' ;;
        *) printf '%s' "$1" ;;
    esac
}

stage_tools() {
    case "$1" in
        clang-format) echo clang-format ;;
        clang-tidy-*) echo clang-tidy ;;
        ruff-*) echo ruff ;;
        shellcheck) echo shellcheck ;;
        shfmt) echo shfmt ;;
    esac
}

run_stage() {
    local stage="$1"
    shift
    case "$stage" in
        clang-format) check_clang_format ;;
        clang-tidy-*) check_clang_tidy "${stage#clang-tidy-}" "$@" ;;
        ruff-check) ruff check "$REPO_ROOT/tools" "$REPO_ROOT/tests" ;;
        ruff-format) ruff format --check "$REPO_ROOT/tools" "$REPO_ROOT/tests" ;;
        shellcheck) shellcheck -x "${SHELL_FILES[@]}" ;;
        shfmt) shfmt -d -i 4 -ci -bn "${SHELL_FILES[@]}" ;;
    esac
}

is_stage() {
    local stage
    for stage in "${STAGES[@]}"; do
        [[ "$stage" == "$1" ]] && return 0
    done
    return 1
}

# Runs a fix step with its output hidden unless it fails.
run_fix() {
    local label="$1"
    local output_file
    shift

    output_file="$(mktemp)"
    if "$@" >"$output_file" 2>&1; then
        rm -f "$output_file"
        return 0
    fi
    printf 'ERROR: %s failed\n' "$label" >&2
    cat "$output_file" >&2
    rm -f "$output_file"
    return 1
}

apply_fixes() {
    local arch
    run_fix "format fixes" format_sources
    run_fix "Ruff fixes" ruff check --fix "$REPO_ROOT/tools" "$REPO_ROOT/tests"
    for arch in "${ARCHES[@]}"; do
        if is_built "$arch"; then
            run_fix "clang-tidy fixes $arch" fix_clang_tidy "$arch"
        fi
    done
}

# Arguments are stages, then optionally `--` and the files that changed, which narrow the
# clang-tidy stages. Runs every stage, even after a failure, so one run reports all of them.
check_stages() {
    local stages=()
    local files=()
    while (($# > 0)) && [[ "$1" != "--" ]]; do
        stages+=("$1")
        shift
    done
    if (($# > 0)); then
        shift
        files=("$@")
        ((${#files[@]} > 0)) || return 0
    fi
    ((${#stages[@]} > 0)) || stages=("${STAGES[@]}")
    local summary=false
    local failed=0
    local total_start stage label start output_file tools arch any_built=false

    ((${#stages[@]} > 1)) && summary=true
    for arch in "${ARCHES[@]}"; do
        is_built "$arch" && any_built=true
    done

    tools=()
    for stage in "${stages[@]}"; do
        is_stage "$stage" || fail "unknown stage '$stage' (stages: ${STAGES[*]})"
        tools+=("$(stage_tools "$stage")")
        if [[ "$stage" == clang-tidy-* && "$any_built" == false ]]; then
            fail "no compile database found; run './build.sh --arch x86_64 build' first"
        fi
    done
    require_tools "${tools[@]}"

    total_start="$(now)"
    for stage in "${stages[@]}"; do
        label="$(stage_label "$stage")"
        if [[ "$stage" == clang-tidy-* ]] && ! is_built "${stage#clang-tidy-}"; then
            printf 'skip  %-20s build/%s not built\n' "$label" "${stage#clang-tidy-}"
            continue
        fi
        start="$(now)"
        output_file="$(mktemp)"
        if run_stage "$stage" ${files[@]+"${files[@]}"} 3>&1 >"$output_file" 2>&1; then
            if [[ "$summary" == true ]]; then
                printf 'ok    %-20s %s\n' "$label" "$(elapsed_since "$start")"
            fi
        else
            failed=1
            if [[ "$summary" == true ]]; then
                printf 'FAIL  %-20s %s\n' "$label" "$(elapsed_since "$start")"
            fi
            printf 'ERROR: %s failed\n' "$label" >&2
            cat "$output_file" >&2
        fi
        rm -f "$output_file"
    done
    if [[ "$summary" == true ]]; then
        printf '%-26s %s\n' "total" "$(elapsed_since "$total_start")"
    fi
    return "$failed"
}

case "${1:-}" in
    format)
        require_tools clang-format ruff shfmt
        format_sources
        ;;
    check)
        shift
        check_stages "$@"
        ;;
    --fix)
        require_tools clang-format clang-tidy ruff shellcheck shfmt
        apply_fixes
        check_stages "${STAGES[@]}"
        ;;
    stages)
        printf '%s\n' "${STAGES[@]}"
        ;;
    *)
        fail "usage: $0 {check [stage...]|--fix|format|stages}"
        ;;
esac
