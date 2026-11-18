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
| (this) | M22 P6 s3 extent writes | - | opus | 0 | 4/4 | |
