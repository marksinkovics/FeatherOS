# Autopilot quality log

One row per committed step. Compare the Opus-implemented steps (before 2026-11-17) with the Sonnet-implemented ones: fix rounds, uncaught invariants, and above all later bugs.

- **Design / Impl:** the models (`-` when there was no design pass).
- **Rounds:** review fix rounds before the commit.
- **Mutations:** invariants caught by tests out of those checked (`-` when none were run).
- **Later bugs:** bugs found after the commit and traced back to this step, with the commit that fixed them.

| Commit | Step | Design | Impl | Rounds | Mutations | Later bugs |
|---|---|---|---|---|---|---|
| ea8f728 | M22 P4 s5 ext2 read-write in the OS | - | opus | 0 | - | |
| 46db5ce | M22 P4b ext2 root | - | opus | 0 | - | |
| d2f3849 | M22 P5 s1 ext3/ext4 layout | - | opus | 0 | - | |
| e36c490 | M22 P5 s2 extent read | - | opus | 0 | 2/2 | |
| 811dfbf | M22 P5 s3 ext3/ext4 in the OS | - | opus | 0 | - | |
| 1aeda8d | M22 P6 s1 jbd2 replay | - | opus | 1 | - | |
| facf33f | M22 P6 s2 unjournalled ext4 writes | - | opus | 0 | - | |
| 4e75e57 | M22 P6 s3 extent writes | - | opus | 0 | 4/4 | |
| a98a6de | M22 P6 s4a jbd2 encoders and routing | opus | sonnet | 0 | 3/3 | |
| e3d549f | M22 P6 s4b jbd2 transactions | opus | sonnet | 1 | 18/18 | |
| 8249ec3 | M22 P6 s4c orphans and split changes | opus | sonnet | 0 | 2/2 | |
| 49696ba | M22 P6 s4d crash, model and fscli tests, step 4 docs | opus | sonnet | 0 | 3/3 | |
| d43b5ea | M22 P6 s5 ext3/ext4 read-write in the OS, writeback task | opus | sonnet | 0 | 3/3 (I5 by T13, not host T5) | |
| abc862e | M22 P6b s1 root= and ext3/ext4 roots | - | sonnet | 0 | - | |
| 5caa991 | M22 P6b s2 ext4 root on the test disk, ext2 at vda4 | - | sonnet | 0 | - | |
| (this) | M29 P1 root without test files | - | sonnet | 0 | - | |
