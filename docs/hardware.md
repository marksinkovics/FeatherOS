# Booting FeatherOS on a laptop

The checklist for booting an x86-64 UEFI laptop from a USB stick and typing on it. It was written for M14 and moved to M19 Part 2 (2026-11-22), where USB mass storage lets the log be saved to the stick; it is manual. Design background: `docs/design/M14.md`.

FeatherOS has no disk driver for real machines (only virtio-blk), so on a laptop it runs without a disk: a tmpfs root with `/home`, programs from the initramfs's `/system`, `/etc` as a RAM copy. Nothing is saved across a reboot.

## 1. Firmware setup

- Secure Boot off.
- Note the "USB legacy support" and "xHCI mode" options; test with legacy support on, then off.
- Fast Boot off.
- No CSM is needed.

## 2. Build the stick

Either of these, after `./build.sh --arch x86_64 build`:

- `LIMINE_TIMEOUT=5 ./build.sh --arch x86_64 iso` writes `build/x86_64/featheros.iso`, a hybrid image with Limine's BIOS install and an EFI partition. Without the timeout Limine's `timeout: 0` hides the menu where the command line is edited. Write it with `sudo dd if=build/x86_64/featheros.iso of=/dev/rdiskN bs=4m` on macOS (find N with `diskutil list`, unmount the stick first) or `of=/dev/sdX` on Linux.
- `bash tools/make_usb_stick.sh [image]` writes `build/x86_64/featheros-usb.img`: a GPT disk with a FAT32 EFI system partition (entry 1) holding `EFI/BOOT/BOOTX64.EFI` (Limine's), `limine.conf`, `kernel.elf` and `initramfs.tar`, then a 32 MiB FAT16 partition named `FeatherOS log` (entry 2, volume `FEATHERLOG`, a `logs/` directory) for saving the kernel log. `LOG_MIB=N` (16 to 127, default 32) sizes it. `LIMINE_TIMEOUT=N` (default 5) sets the menu delay and `KERNEL_CMDLINE="usb=poll"` puts words on the kernel command line. It needs mtools (`mformat`, `mcopy`, `mmd`) and python3, about 135 MiB of disk with a Debug build, and writes to a stick with the same `dd` command.

Rehearse the stick in QEMU first (Homebrew's edk2 firmware shown):

```sh
qemu-system-x86_64 -machine q35 -m 512 -smp 2 -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/share/qemu/edk2-x86_64-code.fd \
    -drive file=build/x86_64/featheros-usb.img,format=raw,if=none,id=stick \
    -device qemu-xhci -device usb-storage,drive=stick,bootindex=0
```

In the rehearsal the stick is `sda`: the log shows `USB 1-1: ... usb-storage`, `Storage: sda: USB 1-1, ...` and, for the log partition, `Storage: sda2: fat16, 32768 KiB`; the ESP, `sda1`, is FAT32 and gets `Storage: sda1: no supported filesystem`. `/proc/partitions` lists `sda` and `sda2`.

`USB_TEST=ON X86_UEFI=ON ./build.sh --arch x86_64 test` runs the USB checks under the same firmware, and `USB_STICK_TEST=ON X86_UEFI=ON ./build.sh --arch x86_64 test` boots the stick image from a USB drive, saves `dmesg` to the log partition and checks the image on the host.

## 3. Boot

Boot from the stick through the firmware's one-time boot menu. At Limine's menu press `e` to edit the entry and add words after `cmdline:`. Try, in order:

1. the default;
2. `usb=poll` (controller events from the timer, not an interrupt);
3. `ps2=off` (USB keyboard only);
4. `usb=off` (PS/2 only);
5. `usbhid=report` if a USB keyboard types wrong keys (boot keyboards through their report descriptor).

## 4. What to look for in the log

Every line has a fixed prefix; `dmesg` prints them again from the kernel's 64 KiB ring.

| Line | Meaning |
|---|---|
| `KERN/I: Limine transferred control to the kernel` | the first line |
| `KERN/I: PCI: bb:dd.f vvvv:dddd class cccccc` | one per function; look for class `0c0330` (xHCI) and any `0c0320` (EHCI) |
| `KERN/I: Storage: no disk; running from the initramfs` | expected: no virtio-blk |
| `KERN/I: SMP: N CPUs online with IPIs and timers` | the timer and the CPU count |
| `KERN/I: USB: xHCI vvvv:dddd at bb:dd.f, N ports, N slots, N-byte contexts, <interrupt mode>` | the controller; `interrupts by polling` follows when no MSI worked |
| `KERN/W: USB: EHCI at bb:dd.f not supported` | an EHCI controller the kernel leaves alone |
| `KERN/I: USB 1-x: <speed> vvvv:dddd "<product>" <drivers>` | one per device, `1-5.2` behind a hub; no driver means `unbound`; a stick shows `USB 1-1: super-speed 46f4:0001 "QEMU USB HARDDRIVE" usb-storage` (QEMU's, yours has its own ids) |
| `KERN/I: Storage: sda: USB 1-1, "<vendor>" "<product>", N KiB, 512-byte sectors, removable` | a USB drive; `sda1: no supported filesystem` is the stick's FAT32 ESP and `sda2: fat16, 32768 KiB` its log partition |
| `KERN/I: ACPI: FADT 8042 flag set\|clear\|unknown` | whether the firmware says a PS/2 controller exists |
| `KERN/I: Keyboard: i8042 config 0xNN, was 0xNN, flushed N bytes` | the PS/2 set-up, or `Keyboard: no i8042 controller` |

Then type on the internal keyboard and on a USB keyboard, and run `lsusb`, `mousetest 20` (move a USB mouse) and `dmesg`.

## 5. Without a serial port

Most laptops have none.

- The framebuffer shows the log as it happens. Photograph each screen.
- `dmesg | head -n 60` and `dmesg | tail -n 60` page through the ring. `/proc/kmsg` is a snapshot of the same text, not Linux's consuming stream: every open starts at the oldest line still in the ring, and a line is dropped whole when the ring is full.
- Save the log to the stick's log partition by hand (the root is still a tmpfs, so nothing is saved otherwise):

```sh
cat /proc/partitions          # the stick is the sd disk with a fat16 sdX2
mkdir /mnt
mount -t fat16 sda2 /mnt
dmesg > /mnt/logs/dmesg-1.txt # pick a new number per boot
eject sda                     # then pull the stick
```

  The file appears under `FEATHERLOG/logs` on the Mac; record it under "Runs". With a USB card reader holding a card, the stick may be `sdb`: `/proc/partitions` shows which. Photos remain the fallback when the stick is not recognised.
- The xHCI Debug Capability (a USB 3 A-to-A debug cable) is a possible later serial console.

## 6. Known blockers this milestone does not fix

Record which one appears:

- firmware-enabled x2APIC: the xAPIC MMIO accesses in `local_apic.cpp` fail;
- a gated 8254 PIT: timer calibration hangs;
- no 1024x768 GOP mode.

## Runs

One entry per attempt: machine and firmware settings, the boot option, what the log showed (`dmesg | head -n 60`), whether the internal and a USB keyboard typed, the mouse, and which blocker from section 6 appeared.

(none yet)
