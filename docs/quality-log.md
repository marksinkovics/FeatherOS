# Autopilot quality log

One row per committed step (used by the global `autopilot` skill since 2026-11-23). Compare the Opus-implemented steps (before 2026-11-17) with the Sonnet-implemented ones: fix rounds, uncaught invariants, and above all later bugs.

- **Design / Impl:** the models (`-` when there was no design pass).
- **Rounds:** review fix rounds before the commit.
- **Mutations:** invariants caught by tests out of those checked (`-` when none were run).
- **Later bugs:** bugs found after the commit and traced back to this step, with the commit that fixed them.
- **Tokens:** `wtok` per model from the autopilot skill's `scripts/usage.py` (cache reads 0.1, cache writes 1.25/2, output 5, relative to the model's input price). Rows before C2b were backfilled from transcripts on 2026-11-23: they cover the step's workers and the coordinator until the next agent, not the Opus design passes or matrix runs, and are empty where no transcript was left.

| Commit | Step | Design | Impl | Rounds | Mutations | Later bugs | Tokens |
|---|---|---|---|---|---|---|---|
| ea8f728 | M22 P4 s5 ext2 read-write in the OS | - | opus | 0 | - | | |
| 46db5ce | M22 P4b ext2 root | - | opus | 0 | - | | |
| d2f3849 | M22 P5 s1 ext3/ext4 layout | - | opus | 0 | - | | |
| e36c490 | M22 P5 s2 extent read | - | opus | 0 | 2/2 | | |
| 811dfbf | M22 P5 s3 ext3/ext4 in the OS | - | opus | 0 | - | | |
| 1aeda8d | M22 P6 s1 jbd2 replay | - | opus | 1 | - | | |
| facf33f | M22 P6 s2 unjournalled ext4 writes | - | opus | 0 | - | | |
| 4e75e57 | M22 P6 s3 extent writes | - | opus | 0 | 4/4 | | |
| a98a6de | M22 P6 s4a jbd2 encoders and routing | opus | sonnet | 0 | 3/3 | | |
| e3d549f | M22 P6 s4b jbd2 transactions | opus | sonnet | 1 | 18/18 | | |
| 8249ec3 | M22 P6 s4c orphans and split changes | opus | sonnet | 0 | 2/2 | | |
| 49696ba | M22 P6 s4d crash, model and fscli tests, step 4 docs | opus | sonnet | 0 | 3/3 | | |
| d43b5ea | M22 P6 s5 ext3/ext4 read-write in the OS, writeback task | opus | sonnet | 0 | 3/3 (I5 by T13, not host T5) | (older: virtio-blk 10^7-poll timeout aborted journals on slow flushes) | |
| abc862e | M22 P6b s1 root= and ext3/ext4 roots | - | sonnet | 0 | - | | |
| 5caa991 | M22 P6b s2 ext4 root on the test disk, ext2 at vda4 | - | sonnet | 0 | - | | |
| 8bca671 | M29 P1 root without test files | - | sonnet | 0 | - | | |
| 9d7885c | fix: virtio-blk poll timeout (flush flake) | - | opus | 0 | - | | |
| 88ccb3e | fix: smoke top Ctrl+Z race | - | sonnet | 0 | - | | |
| 3a3d8d1 | M29 P2 s1 /etc on the root | - | sonnet | 0 | - | | |
| 5c81254 | M29 P2 s2 /etc/init.conf, HOME, cd | - | sonnet | 0 | - | | |
| c1ad0b4 | M29 P3 programs from /bin, /sbin/init | - | sonnet | 0 | - | | |
| 58e5484 | M29 P4 s1 core MakeReadOnly | opus | sonnet | 0 | 3/3 (I5 only via deeper guards) | | |
| 2a35e93 | M29 P4 s2 kernel VFS remount | opus | sonnet | 0 | 3/3 | test fixture read a freed initramfs (UseRootEtc); null core reachable by remount | |
| 56770ed | M29 P4 s3 MS_REMOUNT, STORAGE/SHELL, docs | opus | sonnet | 0 | 2/2 | | |
| 7b83d66 | fix: failed remount reopen leaves a safe mount | - | sonnet | 0 | 1/1 | (M29 P4 s2: null core reachable by remount) | |
| ef0c7e3 | M30 P1 s1 growable fd and disk tables, ENFILE | opus | sonnet | 0 | 6/6 (2 by tests beyond the design) | | |
| f903174 | M30 P1 s2 RLIMIT_NOFILE, ulimit | opus | sonnet | 0 | 2/2 | | |
| 98e6719 | fix: remount test mounts its own initramfs | - | claude | 0 | - | | |
| 0b14d0a | M30 P2 s1 core rename crash tests | opus | sonnet | 0 | 3/3 | | |
| f1d19e5 | M30 P2 s2 kernel rename, tmpfs replace | opus | sonnet | 0 | 3/3 | | |
| c268c3b | M30 P3 /sbin admin programs | - | sonnet | 0 | - | | |
| b464dd3 | M30 P4 /etc/init.d and init.disabled | - | sonnet | 0 | - | | |
| f63b7ba | M30 P4 follow-up: no-console guard | - | sonnet | 0 | - | | |
| 3ca7e21 | M30 P5 s1 MBR extended partitions, Linux numbering | opus | sonnet | 0 | 3/3 | | |
| 38b3698 | M30 P5 s2 one driver per partition | opus | sonnet | 0 | 4/5 (static last_error only by step 5) | | |
| 1576b6b | M30 P5 s3 test disk vda5/vda6, two ext4 in QEMU | - | sonnet | 0 | 1/1 | | 1.4M wtok: agents sonnet 1.3M; coord opus 153.6k |
| 325454b | M30 P5 s4 VolumeMutex, TaskLocal, unit-test-tsan | opus | sonnet | 0 | 1/1 | | 1.4M wtok: agents sonnet 1.2M; coord opus 163.8k |
| 08bfd9c | M30 P5 s5 per-volume locking in Files and the VFS | opus | sonnet | 1 (CountOpen mutex under process_lock spinlock) | 4/4 | | 8.1M wtok: agents sonnet 7.8M; coord opus 223.2k |
| 34e43c0 | M30 P6 s1 core PageCache, CachedDevice, PageSource | opus | sonnet | 0 | 3/3 | | 1.3M wtok: agents sonnet 1.2M; coord opus 145.0k |
| 8b8aeed | M30 P6 s2 core metadata path over the page cache, pass-through BlockCache | opus | sonnet | 0 | 3/3 | | 3.4M wtok: agents sonnet 3.2M; coord opus 135.8k |
| 5373130 | M30 P6 s3 kernel frames, sizing, reclaim, device counters | opus | sonnet | 0 | 2/2 | | 2.0M wtok: agents sonnet 1.9M; coord opus 117.6k |
| 2436d5d | M30 P6 s4 CoreDriver over CachedDevice, overlap, Cached in meminfo | opus | sonnet | 0 | 4/4 | | 2.9M wtok: agents sonnet 2.7M; coord opus 211.2k |
| 3fb3914 | M30 P6 s5 QEMU page-cache hit and reclaim checks, docs | opus | sonnet | 0 | 2/2 | | 1.1M wtok: agents sonnet 931.3k; coord opus 209.8k |
| 117a4bf | M32 P1 s1 /dev/fb0, syscall 73, graphics mode, fbtest | opus | sonnet | 0 | 3/3 | | |
| 1eaa268 | M32 P1 s2 fbdemo, GRAPHICS_TEST screendumps | - | sonnet | 0 | 1/1 (after widening checks) | | |
| 9d1091c | M32 P2 s1 key events: held keys, queue, read/poll, Ctrl+C | opus | sonnet | 0 | 3/3 | | |
| 87a1cad | M32 P2 s2 fbdemo --keys, QMP key checks | - | sonnet | 0 | 1/1 | | |
| 632727b | M32 P3 graphics library, fbdemo on it, Swift gfxcheck | - | sonnet | 0 | - | | |
| 79e74e6 | M32 P4 featherfall rules, selftest, demo | - | sonnet | 0 | 2/2 | | |
| a764ca4 | M32 P5 playable featherfall, high scores, played GRAPHICS check | - | sonnet | 0 | 1/1 | | |
| 38f5ea4 | M33 P1 tile layer, BlitPart, TileSheet | - | sonnet | 0 | - | | |
| 0bed505 | M33 P2 wickdeep floors, sight, DUNGEON mode | - | sonnet | 0 | 2/2 (connectivity via pinned values) | | |
| e3bd253 | M33 P3 monsters, combat, items, full bot | - | sonnet | 0 | 2/2 | | |
| 432ed22 | M33 P4 saves, hall of runs, --check-save | - | sonnet | 0 | 2/2 | | |
| 0575364 | M33 P5 playable wickdeep, tiles, played DUNGEON check | - | sonnet | 0 | 1/1 | DUNGEON harness walk race, fixed with M34 P3 | |
| 5c41d22 | M34 P1 FFNT format, generator, committed fonts, parser | opus | sonnet | 0 | 4/4 | | |
| 50e8304 | M34 P2 Font, anti-aliased DrawText, sans U+FFFD, gfxcheck | opus | sonnet | 1 | 5/6 (one equivalent) | | |
| d4fad81 | M34 P3 UTF-8 console, anti-aliased mono-16, FONTS mode | opus | sonnet | 1 | 7/8 (one equivalent) | | |
| aaf4ffa | M34 P4 UTF-8 in canonical mode, key decoder, line editor | opus | sonnet | 1 | 3/3 | | |
| 1192924 | M34 P5 programs on the new fonts, scale API and font.c gone | opus | sonnet | 0 | 3/3 (two needed new checks) | | |
| ef163ed | M34 P6 fontview, test card, final docs | opus | sonnet | 0 | 2/2 | | |
| 2c2aea9 | Shell smoke: wait for every pipeline stage before Ctrl+C/Ctrl+Z | - | sonnet | 0 | not run | | |
| 20459bb | M14 P1 s1 PCI layer, x86 port config, AArch64 ECAM, MapDevice ranges | - | sonnet | 0 | 2/2 (I2 step limit caught as a hang) | | 1.9M wtok: agents sonnet 1.8M; coord opus 142.3k |
| be681f8 | M14 P1 s2 DMA, MSI/MSI-X, GICv2m, AArch64 INTx, test=pci with edu | M14.md (opus) | sonnet | 0 | 4/4 (I3, I4 by T3/T4; I5 both arches by T30) | | 3.3M wtok: agents <synthetic> 0, sonnet 3.0M; coord <synthetic> 0, opus 320.5k |
| 0af9637 | M14 P2 s3 xHCI TRBs, rings, contexts, PORTSC mask, fake controller (host) | M14.md (opus) | sonnet | 0 | 3/3 (I6 T6, I7 T5, I14 T12) | | 1.6M wtok: agents sonnet 1.4M; coord opus 138.8k |
| b3d7599 | M14 P2 s4 xHCI bring-up, UsbEvents/UsbManager, interrupt choice, root-port Address Device, USB_TEST | M14.md (opus) | sonnet | 0 | 3/3 (I10 T9 after test fix, I11 T13, I12 T14) | | 3.9M wtok: agents sonnet 3.7M; coord opus 234.7k |
| bb77cee | M14 P2 s5a HostController, descriptor parser, control/interrupt transfers, stall and error recovery | M14.md (opus) | sonnet | 0 | 3/3 (I16 T18 after test fix, I17 T15, I19 T16) | | 4.7M wtok: agents sonnet 4.5M; coord opus 150.3k |
| 507b749 | M14 P2 s5b Device tree, driver binding, root hot-plug, /proc/usb, lsusb | M14.md (opus) | sonnet | 0 | 2/2 (I20 T28 and I20 tests, I21 lock test) | | 3.7M wtok: agents sonnet 3.3M; coord opus 428.8k |
| 55bcf46 | M14 P3 s6 HID report parser, key map, diff, repeater, global locks (host) | M14.md (opus) | sonnet | 0 | 3/3 (I22 T19/T20, I23 T21, I24 T23) | | 1.6M wtok: agents sonnet 1.5M; coord opus 94.9k |
| 7dac9c9 | M14 P3 s7 HID driver, LEDs, usbhid=, ps2=off, USB keys into the shell | M14.md (opus) | sonnet | 0 | 3/3 (I21, I25 hid test; I23 after test fix) — I21 relaxed: detach releases keys into Input under topology_lock, documented | | 4.9M wtok: agents sonnet 4.8M; coord opus 177.3k |
| 5a66ca0 | M14 P3 s8 pointer queue, /dev/mouse, featheros/input.h, mousetest | M14.md (opus) | sonnet | 0 | 2/2 (I27, I28 by T25) | | 3.6M wtok: agents sonnet 3.3M; coord opus 245.5k |
| fb12ce9 | M14 P3 follow-up: mice use report protocol, boot fallback (user decision) | - | sonnet (medium) | 0 | - | | 409.5k wtok: agents sonnet 353.5k; coord opus 56.0k |
| 0deaa8d | M14 P4 s9 hubs: state machine, hub driver, TT, recursive detach, USB_HUB_TEST | M14.md (opus) | sonnet | 0 | 3/3 (I29 T26, I30 T27, I20 T28) | | 3.8M wtok: agents sonnet 3.2M; coord opus 569.7k |
| e686dd9 | M14 P5 s10a i8042 after handoff, ACPI FindTable, FADT flag, OVMF usb case | M14.md (opus) | sonnet | 0 | I32 bits by [i8042]; I32 order not caught (laptop only); I31 by INPUT with usb=off | | 1.9M wtok: agents sonnet 1.8M; coord opus 105.0k |
| 4623da9 | M14 P5 s10b boot without a disk: tmpfs root with /home, nodisk case | M14.md (opus) | sonnet | 0 (agent ran out of turns; coordinator finished verification) | I33 by NODISK case (2 mutations) | | 1.4M wtok: agents sonnet 1.2M; coord opus 126.9k |
| c931dda | M14 P5 s10c log ring, /proc/kmsg, dmesg, LIMINE_TIMEOUT, USB stick image, docs/hardware.md | M14.md (opus) | sonnet | 0 | 2/2 (wrap by [logring], lock by TSan) | | 2.3M wtok: agents sonnet 2.1M; coord opus 135.3k |
| 7a08c5a | M19 P2 s1a xHCI bulk TDs, residue, CancelEndpoint, ClearHalt | M19-P2.md (opus) | sonnet | 0 | 3/3 (I1 by T1, I4 by T6, I5 by T7) | | 2.8M wtok: agents sonnet 2.5M; coord opus 271.6k |
| aaf571c | M19 P2 s1b driver service hook, stalls_are_protocol, device work dispatch | M19-P2.md (opus) | sonnet | 0 | 3/4 (stall count by T23, hub branch by [hub], generation by [service]; detaching check not testable single-threaded) | | 1.8M wtok: agents sonnet 1.3M; coord opus 448.4k |
| a54e4cc | M19 P2 s2a SCSI encoders/parsers, sense table, BOT Execute over a model pipe | M19-P2.md (opus) | sonnet | 0 | 2/2 (I7 by T10/T11, I8 by T11) | | 1.1M wtok: agents sonnet 979.6k; coord opus 114.5k |
| 1307480 | M19 P2 s2b mass-storage drive, driver, TaskPipe, worker, probe, USB_STORAGE_TEST | M19-P2.md (opus) | sonnet | 0 (resumed once after a session restart) | 3/3 (I5 by I5 pipe test, I14 by I14 test/T12/T22, I15 by I15 replug test) | fake device sent the CSW as data on a failed IN command (test fake only; fixed in s2c) | 6.2M wtok: agents sonnet 6.0M; coord opus 292.3k |
| a2ab688 | M19 P2 s2c port reset and re-enumeration, drive retries, media change | M19-P2.md (opus) | sonnet | 0 | 3/3 (I8 by T12, retries by T14, I18 by T24/T12/T15) | | 2.4M wtok: agents sonnet 2.3M; coord opus 150.7k |
| 70f5dea | M19 P2 s3a Storage::Disk, disk registry, virtio as vda, USB drive as sda | M19-P2.md (opus) | sonnet (high) | 0 | 4/4 (I16 x2 by T16, I19 by T16, chunking by T17) | | 3.1M wtok: agents sonnet 2.1M; coord opus 974.0k |
| e777096 | M19 P2 s3b volumes per disk, dead volumes, core Discard, /proc/partitions | M19-P2.md (opus) | sonnet (high) | 0 | 4/4 (I20, I21 by T18; I22 by filesystem_tests boot numbering; Discard by T19) | | 5.9M wtok: agents sonnet 5.7M; coord opus 140.0k |
| 2d597d1 | M19 P2 s4a USB writes, host checks, write protect, FAT16 dirty bits, flush rules | M19-P2.md (opus) | sonnet (high) | 1 (tolerated poweroff panic in the case; DATA PROTECT on flush) | 4/4 (I24 by T20, I27 by T33, I13 by USB I13, I23 by T17) | | 5.0M wtok: agents sonnet 4.7M; coord opus 257.1k |
| 25d50a2 | M19 P2 s4b hot-unplug in QEMU, EIO on dead volumes, cp short writes, sync EIO | M19-P2.md (opus) | sonnet (high) | 0 | 2/2 (I21 by T28 round 4, I19 by T28 round 5) | hotplug checked the FAT copies of the drive pulled mid-copy (flaky under matrix load; fixed 6731359) | 3.3M wtok: agents sonnet 3.0M; coord opus 254.7k |
| 4e0bf26 | M19 P2 s4c eject: syscall 74, libc, /sbin/eject, all-or-nothing unmount | M19-P2.md (opus) | sonnet (high) | 0 | 2/2 (I25 by T21, I24 by T21) | | 3.3M wtok: agents sonnet 3.1M; coord opus 159.8k |
| 603df33 | M19 P2 s4d drives above 2 TiB, 4096-byte sectors, behind a hub | M19-P2.md (opus) | sonnet (high) | 0 | 2/2 (I12 by T8/T30, sector size by partition host test and T30) | | 1.6M wtok: agents sonnet 1.4M; coord opus 144.8k |
| 9af0e78 | M19 P2 s5a stick log partition, boot-from-stick harness, USB_STICK_TEST | M19-P2.md (opus) | sonnet (medium) | 0 | 1/1 (I26 by T32) | | 682.0k wtok: agents sonnet 602.8k; coord opus 79.3k |
| 3ec8ee8 | M16 A1 disk kinds, nvme names, read-only reason, flush skip, 16 disks | M16.md (opus) | sonnet (high) | 0 | 2/2 (I1 Disk half by T2, I15 by T1) | | 997.2k wtok: agents sonnet 723.8k; coord opus 273.5k |
| eed52ef | M16 A2 storage start, environment move, storage=, write policy, shutdown hook | M16.md (opus) | sonnet (high) | 0 | 1/1 (I2 by T4) | | 1.1M wtok: agents sonnet 962.0k; coord opus 149.2k |
| c2a80ad | M16 A3 ControllerHardware, PciHardware, test support (PhysicalMemory, ModelDisk, rigs) | M16.md (opus) | sonnet (high) | 0 | - (no invariant yet; ModelDisk power-loss mutation caught) | | 951.2k wtok: agents sonnet 857.0k; coord opus 94.2k |
| be088fe | M16 B1 NVMe registers, SQE/CQE, PRPs, status map, identify parsing | M16.md (opus) | sonnet (high) | 0 | 3/3 (I13 x2 by T5, I11 by T6) | | 845.8k wtok: agents sonnet 688.3k; coord opus 157.4k |
| 5be2ce2 | M16 B2a NVMe queue pair, controller, namespaces, fake controller | M16.md (opus) | sonnet (high) | 0 | 3/3 (I1 by T12, I4 by T8, I6 by T7) | | 2.9M wtok: agents sonnet 2.8M; coord opus 106.8k |
| 5fb5794 | M16 B2b NVMe recovery ladder (CSTS, abort, reset, dead), T23 concurrency | M16.md (opus) | sonnet (high) | 0 | 3/3 (I3 by T9, I10 by T9, I14 by T23) | | 1.7M wtok: agents sonnet 1.5M; coord opus 159.4k |
| 99cb311 | M16 B3 NVMe probe, test=nvme, storage-controller image, NVME_TEST | M16.md (opus) | sonnet (high) | 0 | 1/2 (I16 by T24; I8 not caught in QEMU, covered by host T10) | image built under build/<arch>, shared by nvme and nvme_without_usb running concurrently (fixed 6731359) | 1.7M wtok: agents sonnet 1.5M; coord opus 207.7k |
| 5391e59 | M16 B4 NVMe namespaces, storage-policy case, test=storage-policy, command counters | M16.md (opus) | sonnet (medium) | 0 | 2/2 (I1 by T27, I11 by T25) | | 1.4M wtok: agents sonnet 1.3M; coord opus 105.0k |
| 62fea08 | M16 C1 AHCI registers, FIS, PRD, signatures, ATA identify | M16.md (opus) | sonnet (high) | 0 | 2/2 (I12 by T16, I13 PRD by T15) | | 911.0k wtok: agents sonnet 742.4k; coord opus 168.7k |
| 01f22c8 | M16 C2a AHCI HBA, ports, I/O, flush, policy, standby, fake HBA | M16.md (opus) | sonnet (high) | 0 | 3/3 (I6 by T17, I17 by T21, I1 driver half by T22) | | 2.8M wtok: agents sonnet 2.6M; coord opus 209.5k |
| b251320 | M16 C2b AHCI TFES/COMRESET recovery, dead rules, T20, AHCI T23 | M16.md (opus) | sonnet (high) | 0 | 4/4 (I3 by T20, I10 by T20 x2, I14 by T20/T23) | | 1.5M wtok: agents sonnet 1.4M; coord opus 98.2k |
| f3f3325 | M16 C3 AHCI probe, test=ahci, AHCI_TEST, policy case for both controllers | M16.md (opus) | sonnet (high) | 0 | - | | 1.9M wtok: agents sonnet 1.9M; coord opus 90.1k |
| 0ec06e7 | M16 D1 root= grammar, boot disk, boot-slot names, deferred scans, boot-disk removal | M16.md (opus) | sonnet (high) | 0 | - | | 3.2M wtok: agents sonnet 3.0M; coord opus 160.3k |
| c3a2c2e | M16 D2 timer first, controllers probed and waited for before the root | M16.md (opus) | sonnet (high) | 0 | 3/4 (I21 by T35, I19 by T32, I23 MayWait by T35; I23 call site not caught) | | 1.6M wtok: agents sonnet 1.4M; coord opus 144.2k |
| 0c7b0b7 | M16 D3 DISK_BUS, 4K root image, ROOT_DISK_TEST cases T36-T42, T29 | M16.md (opus) | sonnet (high) | 0 | - | | 1.5M wtok: agents sonnet 1.3M; coord opus 196.5k |
| d6994c3 | M16 E1 docs, roadmap summary, Complete-when check | - | sonnet | 0 | - | | 408.9k wtok: agents sonnet 341.1k; coord opus 67.8k |
| b25283e | Review F1 FAT16 rename keeps source and target (R1-06, R1-07) | review-F1.md (opus) | sonnet (high) | 0 | 3/3 (I1 by T2, I3 by T5, I4 by T4) | | 3.4M wtok: agents opus 293.4k, sonnet 2.8M; coord opus 286.6k |
| (F2) | Review F2 FAT16 end marker and linear scans (R1-08, R1-09) | - | sonnet (high) | 0 | - | | 1.1M wtok: agents sonnet 611.9k; coord opus 484.7k |
