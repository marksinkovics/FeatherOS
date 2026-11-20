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
| d43b5ea | M22 P6 s5 ext3/ext4 read-write in the OS, writeback task | opus | sonnet | 0 | 3/3 (I5 by T13, not host T5) | (older: virtio-blk 10^7-poll timeout aborted journals on slow flushes) |
| abc862e | M22 P6b s1 root= and ext3/ext4 roots | - | sonnet | 0 | - | |
| 5caa991 | M22 P6b s2 ext4 root on the test disk, ext2 at vda4 | - | sonnet | 0 | - | |
| 8bca671 | M29 P1 root without test files | - | sonnet | 0 | - | |
| 9d7885c | fix: virtio-blk poll timeout (flush flake) | - | opus | 0 | - | |
| 88ccb3e | fix: smoke top Ctrl+Z race | - | sonnet | 0 | - | |
| 3a3d8d1 | M29 P2 s1 /etc on the root | - | sonnet | 0 | - | |
| 5c81254 | M29 P2 s2 /etc/init.conf, HOME, cd | - | sonnet | 0 | - | |
| c1ad0b4 | M29 P3 programs from /bin, /sbin/init | - | sonnet | 0 | - | |
| 58e5484 | M29 P4 s1 core MakeReadOnly | opus | sonnet | 0 | 3/3 (I5 only via deeper guards) | |
| 2a35e93 | M29 P4 s2 kernel VFS remount | opus | sonnet | 0 | 3/3 | test fixture read a freed initramfs (UseRootEtc); null core reachable by remount |
| 56770ed | M29 P4 s3 MS_REMOUNT, STORAGE/SHELL, docs | opus | sonnet | 0 | 2/2 | |
| 7b83d66 | fix: failed remount reopen leaves a safe mount | - | sonnet | 0 | 1/1 | (M29 P4 s2: null core reachable by remount) |
| ef0c7e3 | M30 P1 s1 growable fd and disk tables, ENFILE | opus | sonnet | 0 | 6/6 (2 by tests beyond the design) | |
| f903174 | M30 P1 s2 RLIMIT_NOFILE, ulimit | opus | sonnet | 0 | 2/2 | |
| 98e6719 | fix: remount test mounts its own initramfs | - | claude | 0 | - | |
| 0b14d0a | M30 P2 s1 core rename crash tests | opus | sonnet | 0 | 3/3 | |
| f1d19e5 | M30 P2 s2 kernel rename, tmpfs replace | opus | sonnet | 0 | 3/3 | |
| c268c3b | M30 P3 /sbin admin programs | - | sonnet | 0 | - | |
| b464dd3 | M30 P4 /etc/init.d and init.disabled | - | sonnet | 0 | - | |
| f63b7ba | M30 P4 follow-up: no-console guard | - | sonnet | 0 | - | |
| 3ca7e21 | M30 P5 s1 MBR extended partitions, Linux numbering | opus | sonnet | 0 | 3/3 | |
| 38b3698 | M30 P5 s2 one driver per partition | opus | sonnet | 0 | 4/5 (static last_error only by step 5) | |
| (this) | M30 P5 s3 test disk vda5/vda6, two ext4 in QEMU | - | sonnet | 0 | 1/1 | |
| 1576b6b | M30 P5 s3 test disk vda5/vda6, two ext4 in QEMU | - | sonnet | 0 | 1/1 | |
| (this) | M30 P5 s4 VolumeMutex, TaskLocal, unit-test-tsan | opus | sonnet | 0 | 1/1 | |
