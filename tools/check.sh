#!/bin/bash
# Builds both architectures, then runs lint, the unit tests and the chosen QEMU modes, printing
# one line per stage and only the lines that matter when one fails.
#
#   tools/check.sh [--modes "SHELL INPUT"] [--arch x86_64|aarch64|both] [--matrix] [--no-lint]
#
# Modes are build.sh test toggles without _TEST (SHELL for SHELL_TEST=ON); none runs no QEMU
# test. --matrix runs ./build.sh test-all after everything else passed. Full logs are under
# build/check/, and each smoke test's whole session is in build/<arch>/smoke-output.txt.

set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$REPO_ROOT/build/check"
modes=""
arches="x86_64 aarch64"
matrix=false
lint=true

while (($# > 0)); do
    case "$1" in
        --modes)
            modes="$2"
            shift 2
            ;;
        --arch)
            if [[ "$2" == both ]]; then
                arches="x86_64 aarch64"
            else
                arches="$2"
            fi
            shift 2
            ;;
        --matrix)
            matrix=true
            shift
            ;;
        --no-lint)
            lint=false
            shift
            ;;
        *)
            printf 'usage: %s [--modes "SHELL INPUT"] [--arch x86_64|aarch64|both] [--matrix] [--no-lint]\n' "$0" >&2
            exit 2
            ;;
    esac
done

mkdir -p "$LOG_DIR"
cd "$REPO_ROOT" || exit 1

# Runs a stage with its output in a log; on failure prints the lines worth reading.
stage() {
    local name="$1"
    local pattern="$2"
    shift 2
    local log="$LOG_DIR/${name//[^A-Za-z0-9_-]/_}.log"
    if "$@" >"$log" 2>&1; then
        printf 'ok    %s\n' "$name"
        return 0
    fi
    printf 'FAIL  %s (log: %s)\n' "$name" "${log#"$REPO_ROOT"/}"
    grep -E "$pattern" "$log" | head -n 30
    return 1
}

for arch in x86_64 aarch64; do
    stage "build $arch" "error|undefined reference|FAILED" ./build.sh --arch "$arch" build || exit 1
done
if [[ "$lint" == true ]]; then
    bash tools/quality.sh --fix >/dev/null 2>&1
    # Fixes may have changed sources, so the check needs fresh builds.
    for arch in x86_64 aarch64; do
        stage "rebuild $arch" "error|undefined reference|FAILED" ./build.sh --arch "$arch" build || exit 1
    done
    stage "lint" "warning:|error:|ERROR|^[A-Z]+[0-9]+ |-->|^[-+][^-+]" \
        bash tools/quality.sh check || exit 1
fi
stage "unit tests" "FAILED|Failed|error:|with expansion|REQUIRE" ./build.sh unit-test || exit 1
for arch in $arches; do
    for mode in $modes; do
        stage "$mode $arch" "^FAIL|^PASS|PANIC|libctest: failed|hardening: failed" \
            env "${mode}_TEST=ON" ./build.sh --arch "$arch" test || exit 1
    done
done
if [[ "$matrix" == true ]]; then
    stage "matrix" "^FAIL|^ERROR|^Ran |^OK|FAILED" ./build.sh test-all || exit 1
fi
