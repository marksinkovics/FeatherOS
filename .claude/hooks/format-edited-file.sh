#!/bin/bash
# Claude Code PostToolUse hook: formats the file an Edit or Write just changed with the same
# formatter and settings as tools/quality.sh, so formatting never shows up as a lint failure.
# It never blocks the edit.

set -uo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

file="$(python3 -c 'import json, sys; print(json.load(sys.stdin).get("tool_input", {}).get("file_path", ""))' 2>/dev/null)"
[[ -n "$file" && -f "$file" ]] || exit 0
case "$file" in
    "$REPO_ROOT"/*) ;;
    *) exit 0 ;;
esac

if [[ -d /opt/homebrew/opt/llvm/bin ]]; then
    PATH="/opt/homebrew/opt/llvm/bin:$PATH"
elif [[ -d /usr/local/opt/llvm/bin ]]; then
    PATH="/usr/local/opt/llvm/bin:$PATH"
fi

relative="${file#"$REPO_ROOT"/}"
case "$relative" in
    src/*.c | src/*.cpp | src/*.h | src/*.hpp | src/libcxx/include/*)
        clang-format -i "$file" 2>/dev/null
        ;;
    tools/*.py | tests/*.py)
        ruff format --quiet "$file" 2>/dev/null
        ;;
    *.sh)
        shfmt -w -i 4 -ci -bn "$file" 2>/dev/null
        ;;
esac
exit 0
