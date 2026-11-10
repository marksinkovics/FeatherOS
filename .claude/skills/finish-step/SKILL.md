---
name: finish-step
description: Update FeatherOS's docs when a milestone step, part or whole milestone is done - docs/roadmap.md, docs/development.md, docs/posix.md and CLAUDE.md's Status - in the house style. Use when the user says a step or part is finished, or asks to update the docs or the handover.
---

# Finishing a step, part or milestone

The user clears the conversation between steps, so these docs are the handover. The next session must be able to continue from CLAUDE.md's Status and the roadmap alone.

## 1. Check it is really done

- Tests for the work pass on **both** architectures at the level the cadence asks for: unit tests and the affected smoke mode for a step, the full matrix (`tools/check.sh --matrix` or `./build.sh test-all`) for a part or milestone. If something was not run, say so and do not mark it done.
- `bash tools/quality.sh check` passes.
- Any limitation left behind is written down (step 4) and not lost.

## 2. `docs/roadmap.md`

- Find the milestone (`### <N>. <Title>`). Its parts are bullets of the form `- **Part K: <name> (done).** <what was built>`.
- For a finished part: add ` (done)` inside the bold label and replace the plan text with what was actually built. Name the files, types and functions a later reader needs to find it (`src/...`, `Namespace::Function`), say which test checks it (smoke mode, matrix case, CTest name), and record decisions and anything that turned out differently from the plan. Write in full sentences, past or present tense, no marketing.
- For a step inside a part, add it to that part's text in the same way (`step N done: ...`).
- Work that was moved out goes to `docs/ideas.md` or another milestone, with a note where it went.
- For a finished milestone: change the heading to `### <N>. <Title> (Complete)`, add a `**Summary:**` paragraph right under the heading (what the milestone gives the system, in a few sentences), and check the milestone's **Complete when** list item by item.

## 3. `docs/development.md` and `docs/posix.md`

- `development.md` is for users and developers: new programs and their options, test modes and environment toggles, syscalls (the syscall table), build options, how a subsystem behaves. Put each item into the existing section it belongs to. Do not write a changelog.
- Every new or changed libc function or header goes in `docs/posix.md`, including how it differs from POSIX and what is left out.

## 4. `CLAUDE.md` Status

- Keep it short. A finished milestone is one line in the "Complete, all on `main`" list (only once it is merged; until then it stays as "in progress" with its branch).
- The milestone in progress gets one bullet: branch, and `Part K done: <one or two sentences with the key files>` per finished part, then `Next: Part K+1, <name>.` Move detail from earlier parts to the roadmap if the bullet grows past about 15 lines.
- Update "Known hard limits" when a limit changes, and the Branches section when a branch is cut or merged.
- When a milestone completes, collapse its bullet to one line in the complete list and name the next milestone.

## 5. Memory and handover

- Do not write `docs/handover.md` unless work is left unfinished in the middle of a step; then say exactly what is half done and how to continue.
- Save a memory only for something non-obvious that the docs and code do not record (a testing gotcha, a user preference).

## 6. Report

Tell the user which files were updated, the test status the docs claim, and suggest a commit message in the repo's style (`feat(<area>): ...`, `docs(roadmap): ...`). Never commit; the user commits.
