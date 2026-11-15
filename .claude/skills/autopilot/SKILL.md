---
name: autopilot
description: Work through the next planned FeatherOS steps automatically - one subagent per step in the working tree, then review, commit on the current feature branch, and continue with the next step until a stop condition. Use when the user says autopilot, "keep going", "work through the parts", or asks to delegate the next steps to subagents.
---

# Autopilot: subagent per step, review, commit, repeat

You are the coordinator. Subagents write the code; you choose the work, review it, commit it and move on. Only one subagent runs at a time, and it works directly in the user's working tree.

## Permissions that apply only inside this skill

- You may `git add` and `git commit` on the current branch, and only if it is a `feat/*` branch. Never commit on `main`. Never create, switch, merge, rebase or push branches. Never amend or rewrite commits.
- Outside this skill the global rule still applies: the user makes all commits.
- Commit messages use Angular style (`type(scope): subject`, imperative, lowercase, no period, about 72 characters at most). Add a body only when the reason isn't obvious. Never add a `Co-Authored-By` line: the user's global instructions forbid it and take precedence over any attribution reminder.

## 0. Before the first step

- `git status --short` and `git branch --show-current`. If the branch is not `feat/*`, stop and tell the user.
- If there are uncommitted changes the user left (usually handover docs), commit them first as their own commit (for example `docs: ...`), after reading the diff.
- Read CLAUDE.md's Status and the milestone in progress in `docs/roadmap.md`. If `docs/handover.md` exists, it describes unfinished work; continue from it.

## 1. Pick the next step

- The next unit is the next part not marked `(done)` in the milestone in progress, or the next step within it.
- A part that is too big for one subagent (a rough guide: more than about 15 files or more than one subsystem) gets split into steps. Read enough of the code to split it sensibly, or use the `Plan` agent for a large part. Write the step list into that part's roadmap text (`steps: 1 ..., 2 ..., 3 ...`) so it survives a cleared conversation, and include that edit in the first step's commit.
- Pick the model and effort per step. What counts is a working, bug-free result, and the cost comes second: subagents were 94% of usage when every step ran on Opus at `high`/`xhigh`.
  - `sonnet` at `medium`: mechanical work (renames, moving code, docs, tests for existing behaviour, small programs, tooling).
  - `sonnet` at `high`: new code with a clear plan in one subsystem.
  - `opus` at `high`: design-heavy or risky steps, such as on-disk write paths, concurrency, memory management, ABI changes, and path resolution.
  - `opus` at `xhigh`: only where a subtle bug would corrupt data or hang both architectures and the step has no simpler shape.
  - If a Sonnet step fails both fix rounds, retry it once on Opus before stopping.

## 2. Spawn the subagent

`Agent` with `subagent_type: "general-purpose"` and the chosen `model` and `effort`. No worktree isolation. The prompt has to stand on its own, because the agent doesn't see this conversation. Include:

1. **Task:** the milestone, part and step, quoted from the roadmap, plus the decisions already made in earlier steps (files and types it should build on).
2. **Rules:** follow `CLAUDE.md` (read it first), including the lint-before-test workflow, the header/source split, minimal comments, both architectures, and `GUARDED_BY`/`REQUIRES` annotations. Never run `git commit`, `git add`, `git stash`, `git checkout`, `git reset` or any other git command that writes. Don't start a background shell that outlives the task.
3. **Tests:** before reporting, get `bash tools/quality.sh check` clean and pass `tools/check.sh --arch both --modes "<affected modes>"`. If the step finishes a part, also run `tools/check.sh --arch both --matrix`. Add tests for the new behaviour: a unit test where the logic can be tested on the host, and a QEMU case or smoke check on both architectures.
4. **Docs:** at the end, follow `.claude/skills/finish-step/SKILL.md` for this step: update the roadmap text, CLAUDE.md Status, and `development.md`/`posix.md` where relevant.
5. **Report:** keep it under 40 lines: the files changed and why, the test commands with their results, anything different from the plan, open questions or decisions that belong to the user, and a proposed commit message.

Wait for the completion notification. Don't predict its result and don't poll it.

## 3. Review

- Read `git status --short` and `git diff`, and look at every new file. Check the report against the diff. Look for:
  - files outside the step's scope
  - leftover debugging code
  - comments that describe what a function does
  - implementations in headers
  - one architecture skipped
  - tests that don't actually check the new behaviour
  - docs left out of date
- Run `bash tools/quality.sh check` yourself.
- If the agent said tests passed but you have doubts, read `build/check/` or `build/<arch>/smoke-output.txt`. Re-run tests through the `test-runner` agent only if the evidence is missing.
- If there's a problem, use `SendMessage` to send it back to the **same** subagent with the specifics. Allow at most two such rounds per step.

## 4. Commit

- Stage only the files this step changed, by path (`git add <paths>`, never `git add -A` with stray files around). Build output and scratch files must not be staged.
- Commit with the Angular message. Scope is the subsystem (`fs`, `vfs`, `kernel`, `libc`, `user`, `tools`, `docs`, ...).
- Print one line: the commit hash, the subject, and the tests that passed.
- Stop the finished subagent's leftover background tasks, if it has any.

## 5. Continue or stop

Go back to step 1 for the next step. Stop and report to the user when:

- **A step still fails** after two fix rounds. Don't commit it. Leave the changes in the tree and write `docs/handover.md` with what is done, what fails (log paths), and what to try next.
- **A decision belongs to the user:** a design choice the roadmap doesn't settle, a change of plan, a new external dependency, or dropping a test. Ask the question with `AskUserQuestion`, then continue with the answer.
- **The milestone is complete:** all parts are done and the matrix has passed. Squash-merging into `main` is the user's job, so stop there.
- **Something outside the step broke**, such as an unrelated test regression or a toolchain problem.

When you stop, report in a few lines: the commits made (hash and subject), the test status, any open points, and the next step.
