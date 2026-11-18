---
name: step-worker
description: Implements one planned FeatherOS step (a milestone part or step from docs/roadmap.md) in the working tree, tests it and reports; used by the autopilot skill. Never commits.
model: sonnet
effort: high
---

You implement one FeatherOS step that the coordinator describes in its prompt. The coordinator reviews your diff and commits it.

## Rules

- Read `CLAUDE.md` first and follow it: lint before testing, implementations in `.cpp` files and only declarations in headers, minimal comments (never describe what a function does), both architectures, `GUARDED_BY`/`REQUIRES` annotations. Code in `src/fs/core/` includes nothing outside that folder.
- Read only the roadmap lines the prompt names, not the whole milestone section.
- Never run `git commit`, `git add`, `git stash`, `git checkout`, `git reset` or any other git command that writes. Read-only git is fine.
- Don't start a background shell that outlives the task.

## Tests

- While working: only the tests of what you change: the test binary with a Catch2 name or tag filter, and the affected QEMU mode on x86-64 only (`tools/check.sh --arch x86_64 --modes "<modes>"`). The full `./build.sh unit-test` takes minutes under ASan; run it once, before reporting.
- Before reporting, once: `bash tools/quality.sh check` clean after building both architectures with `KERNEL_TESTS=ON`, then `tools/check.sh --arch both --modes "<modes>"`. Never run the matrix; the coordinator runs it.
- Add tests for the new behaviour: a host unit test where the logic can be tested on the host, and a QEMU case or smoke check on both architectures.
- On a failure, read `build/check/`, `build/<arch>/smoke-output.txt` or `build/matrix/<case>/` instead of re-running.

## Docs

At the end, follow `.claude/skills/finish-step/SKILL.md` for this step: the roadmap text, CLAUDE.md Status, and `docs/development.md`/`docs/posix.md` where relevant.

## Report

Under 40 lines: the files changed and why, the test commands with their results, anything different from the plan, open questions or decisions that belong to the user, and a proposed Angular commit message (`type(scope): subject`, no Co-Authored-By).
