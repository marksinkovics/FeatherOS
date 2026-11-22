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
- **Usage budget.** The user keeps hitting the 5-hour limit. Most of the cost is subagents re-reading code from scratch and running the full test suite again and again (4b: ~566k tokens; 4c with these rules: ~416k). Follow the rules marked *budget* below.
- Pick the model and effort per step. Sonnet is the default.
  - `sonnet` at `medium`: mechanical work (renames, moving code, docs, tests for existing behaviour, small programs, tooling, test-disk and harness changes).
  - `sonnet` at `high` (the agent's default): everything else, including read-only filesystem code, parsers, and wiring an existing core feature into the kernel.
  - Risky steps (code that writes on-disk structures, crash ordering, concurrency, memory management or ABI changes): designed on Opus (section 1b), implemented on `sonnet` at `high`.
  - Never `xhigh`. If a Sonnet step fails both fix rounds, retry it once on Opus before stopping.

## 1b. Design (risky steps only)

- *Budget:* if a design file already covers the step (a sub-step of a designed step), reuse it and skip this pass. Fix mistakes found later in the design file itself.
- Run the `Plan` agent with `model: "opus"`. It reads the code and writes the design to `docs/design/<milestone>-<part>-<step>.md`; give it the roadmap line range, the earlier steps' decisions, and this list of what the design must contain:
  - the files, types and function signatures, and which existing code they build on
  - the on-disk or in-memory layout, and every invariant the code must keep, numbered (`I1`, `I2`, ...)
  - for write paths, the exact order of writes and what a crash at each point leaves
  - the edge cases, and the errors returned for each
  - the acceptance tests, numbered (`T1`, ...), each checked by an outside oracle where one exists (`e2fsck -fn`, `debugfs`, mtools, a model, a byte comparison), and for each invariant the test that fails when it is broken
- Review the design before implementing: every roadmap requirement covered, every invariant has a test, nothing settles a question that belongs to the user. Fix gaps by sending them back to the same Plan agent.
- The design file is committed with the step.

## 2. Spawn the subagent

`Agent` with `subagent_type: "step-worker"` (`.claude/agents/step-worker.md`: Sonnet at `high`, with the standing rules, test cadence, docs and report format). Pass `model`/`effort` only to move off that default. No worktree isolation. The prompt has to stand on its own, because the agent doesn't see this conversation, but leave out what the agent file already says. Include:

1. **Task:** the milestone, part and step, quoted from the roadmap, plus the decisions already made in earlier steps (files and types it should build on).
2. **Where to read:** the roadmap line range for this part and the earlier summaries it builds on (`grep -n` for them), so the agent doesn't read the whole milestone section.
3. **Tests:** the affected QEMU modes. The agent never runs the matrix.
4. **For risky steps:** the design file path. Tell the agent to implement it as written, to implement every test `T1`... it names, to report any departure from it with the reason, and to run a mutation check on the 2–3 riskiest invariants you name (not all of them): break each on purpose, run the one test that should catch it (filtered), record which test failed, and undo the change. The report lists `I<n>: caught by T<m>` or `I<n>: not caught`.
5. *Budget:* read only the design sections and code the step needs (name them); while iterating, run only the filtered tests of what changes; the full `./build.sh unit-test` once at the end; stop every background process before reporting.

Wait for the completion notification. Don't predict its result and don't poll it.

## 3. Review

- Read `git status --short`, `git diff --stat` and the diff of changed existing files and tests. Of large new files, check the parts the design calls risky and scan for leftover debugging code (*budget:* don't read them whole). Check the report against the diff. Look for:
  - files outside the step's scope
  - leftover debugging code
  - comments that describe what a function does
  - implementations in headers
  - one architecture skipped
  - tests that don't actually check the new behaviour
  - docs left out of date
- For a risky step, also check the diff against the design: every invariant visible in the code, every `T<n>` present and asserting what the design says, the departures justified, and the named invariants caught in the mutation check. An invariant that is not caught is a missing test; send it back.
- Run `bash tools/quality.sh check` yourself.
- If the agent said tests passed but you have doubts, read `build/check/` or `build/<arch>/smoke-output.txt`. Re-run tests through the `test-runner` agent only if the evidence is missing.
- If the step finishes a part that changes shared code (kernel, libc, the build, the disk image or the test harness), or finishes the milestone, run the matrix through `test-runner` (Haiku): `tools/check.sh --arch both --matrix`. A part limited to one program or to docs relies on its affected smoke modes on both architectures.
- If a test fails, give `test-diagnoser` (Sonnet) the failing cases and log paths from `test-runner`. If its confidence is low, or its fix doesn't work, run it once more with `model: "opus"`.
- If there's a problem, use `SendMessage` to send it back to the **same** subagent with the specifics (for a test failure, the diagnoser's cause, evidence and fix). Allow at most two such rounds per step, and re-run only the failing cases afterwards through `test-runner`.

## 4. Commit

- Stage only the files this step changed, by path (`git add <paths>`, never `git add -A` with stray files around). Build output and scratch files must not be staged.
- Commit with the Angular message. Scope is the subsystem (`fs`, `vfs`, `kernel`, `libc`, `user`, `tools`, `docs`, ...).
- Add a row to `.claude/skills/autopilot/quality-log.md` and commit it with the step.
- When a review, a test failure or a later step finds a bug that an earlier committed step introduced, note it in that step's row under "Later bugs". This column is what tells whether the cheaper models cost quality.
- Print one line: the commit hash, the subject, and the tests that passed.
- Stop the finished subagent with `TaskStop` as soon as its report is in, so leftover background work doesn't keep using tokens.

## 5. Continue or stop

Go back to step 1 for the next step. Stop and report to the user when:

- **A step still fails** after two fix rounds. Don't commit it. Leave the changes in the tree and write `docs/handover.md` with what is done, what fails (log paths), and what to try next.
- **A decision belongs to the user:** a design choice the roadmap doesn't settle, a change of plan, a new external dependency, or dropping a test. Ask the question with `AskUserQuestion`, then continue with the answer.
- **The milestone is complete:** all parts are done and the matrix has passed. Squash-merging into `main` is the user's job, so stop there.
- **A new part or the matrix is next** (*budget*): finishing a step within the current part continues automatically; before starting the next part, or before running the matrix, stop and ask.
- **Something outside the step broke**, such as an unrelated test regression or a toolchain problem.

When you stop, report in a few lines: the commits made (hash and subject), the test status, any open points, and the next step.
