import hashlib
import json
import os
import re
import shutil
import tempfile
from pathlib import Path

import create_usb_disks as usb_disks
from create_usb_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FatCopiesAgree,
    FatStatusWord,
    FindE2fsprogs,
    FindFsckFat,
    FsckFat,
    IsMarkedClean,
    Mdir,
    MtoolsSpec,
    Mtype,
    NeedsRecovery,
    NotFound,
    PartitionBytes,
)
from smoke.qemu import OpenQmpChannel, SendQmpCommand
from smoke.script import Lines, WaitFor

IMAGE_KIBIBYTES = usb_disks.FAT16_IMAGE_SECTORS * SECTOR_SIZE // 1024
FAT16_KIBIBYTES = usb_disks.FAT16_SECTORS * SECTOR_SIZE // 1024
EXT2_KIBIBYTES = usb_disks.EXT2_SECTORS * SECTOR_SIZE // 1024
FLOPPY_KIBIBYTES = usb_disks.FLOPPY_SECTORS * SECTOR_SIZE // 1024
DIRTY_KIBIBYTES = usb_disks.DIRTY_SECTORS * SECTOR_SIZE // 1024
DEVICE_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): super-speed 46f4:0001 \"QEMU USB HARDDRIVE\" "
    r"usb-storage"
)
# The drives sit on root ports 1 to 4 in this order, and the storage worker
# probes them in port order, so the letters follow.
DRIVES = (
    ("sda", usb_disks.FAT16_IMAGE, IMAGE_KIBIBYTES, False),
    ("sdb", usb_disks.FLOPPY_IMAGE, FLOPPY_KIBIBYTES, False),
    ("sdc", "usb-dirty-copy.img", DIRTY_KIBIBYTES, False),
    ("sdd", "usb-protected.img", IMAGE_KIBIBYTES, True),
)
VOLUME_LINES = (
    ("sda1", "fat16", FAT16_KIBIBYTES),
    ("sda2", "ext2", EXT2_KIBIBYTES),
    ("sdb", "fat16", FLOPPY_KIBIBYTES),
    ("sdc", "fat16", DIRTY_KIBIBYTES),
    ("sdd1", "fat16", FAT16_KIBIBYTES),
    ("sdd2", "ext2", EXT2_KIBIBYTES),
)
SECTORS_LINE = (
    "KERN/USBSTORAGE: sda "
    f"{IMAGE_KIBIBYTES * 1024 // SECTOR_SIZE} sectors of "
    f"{SECTOR_SIZE} bytes"
)
STEPS = (
    "KERN/USBSTORAGE: sda1 mounted, read and unmounted",
    "KERN/USBSTORAGE: sda2 mounted, read and unmounted",
    "KERN/USBSTORAGE: last sector written and read back",
    "KERN/USBSTORAGE: transfer past the end refused",
    "KERN/USBSTORAGE: multi-command transfer matched",
    "KERN/USBSTORAGE: counters matched",
    "KERN/USBSTORAGE: tests passed",
)
FAILURES = (
    "reset recovery",
    "could not",
    "enumeration failed",
    "driver could not attach",
)
SMALL = b"usb small file\n"
LONG_NAME = b"long name on the stick\n"
INNER = b"inner file of dir\n"
EXT2_TEXT = b"ext2 on the stick\n"
FLOPPY_NOTE = b"written on the superfloppy\n"
LAST = b"written just before shutdown\n"
BEFORE_EJECT = b"written before eject\n"
# The shell session after the test hook finished. Each volume is mounted in
# /tmp; the 1 MiB copy makes the FAT16 core issue hundreds of WRITE commands.
# eject sda is refused while a background sleep holds a file of sda1 open and
# while the shell stands in sda2, then ejects both volumes; the last write goes
# to the superfloppy, which shutdown finds still mounted.
SHELL_INPUT = b"""sleep 1
cat /proc/partitions
mkdir /tmp/usb
mount -t fat16 sda1 /tmp/usb
write /tmp/usb/small.txt usb small file
cat /tmp/usb/small.txt
write "/tmp/usb/A long VFAT name.txt" long name on the stick
cat "/tmp/usb/A long VFAT name.txt"
cp /tmp/usb/BIG.BIN /tmp/usb/COPY.BIN
echo big copy done
mkdir /tmp/usb/dir
write /tmp/usb/dir/inner.txt inner file of dir
mv /tmp/usb/dir/inner.txt /tmp/usb/dir/moved.txt
write /tmp/usb/gone.txt temporary
rm /tmp/usb/gone.txt
ls /tmp/usb/dir
umount /tmp/usb
mount -t fat16 sda1 /tmp/usb
cat /tmp/usb/small.txt
cat "/tmp/usb/A long VFAT name.txt"
cat /tmp/usb/dir/moved.txt
ls /tmp/usb
sync
umount /tmp/usb
mount -t ext2 sda2 /tmp/usb
write /tmp/usb/usb-ext2.txt ext2 on the stick
cat /tmp/usb/usb-ext2.txt
sync
umount /tmp/usb
mkdir /tmp/floppy
mount -t fat16 sdb /tmp/floppy
write /tmp/floppy/NOTE.TXT written on the superfloppy
cat /tmp/floppy/FLOPPY.TXT
cat /tmp/floppy/NOTE.TXT
sync
umount /tmp/floppy
mkdir /tmp/ro
mount -t fat16 sdd1 /tmp/ro
grep sdd1 /proc/mounts
cat /tmp/ro/USBFILE.TXT
write /tmp/ro/f.txt refused
mount -o remount,rw /tmp/ro
write /tmp/ro/f.txt refused again
umount /tmp/ro
mkdir /tmp/dirty
mount -t fat16 sdc /tmp/dirty
grep sdc /proc/mounts
cat /tmp/dirty/DIRTY.TXT
write /tmp/dirty/f.txt refused
mount -o remount,rw /tmp/dirty
write /tmp/dirty/f.txt refused again
umount /tmp/dirty
mkdir /tmp/usb2
mount -t fat16 sda1 /tmp/usb
mount -t ext2 sda2 /tmp/usb2
write /tmp/usb/before-eject.txt written before eject
sleep 2 < /tmp/usb/small.txt &
eject sda
grep sda /proc/mounts
wait
cd /tmp/usb2
eject sda
cd /home
eject vda
eject sdz
eject sda
mount -t fat16 sda1 /tmp/usb
cat /proc/partitions
eject sda
echo second eject done
grep sda /proc/mounts
mount -t fat16 sdb /tmp/floppy
write /tmp/floppy/LAST.TXT written just before shutdown
shutdown
"""
SHELL_OUTPUT = (
    "usb small file",
    "long name on the stick",
    "big copy done",
    re.compile(r"moved\.txt +18"),
    re.compile(r"A long VFAT name\.txt +23"),
    "ext2 on the stick",
    "superfloppy",
    "written on the superfloppy",
    "sdd1 /tmp/ro fat16 ro 0 0",
    "fat16 on a stick",
    "write: cannot open /tmp/ro/f.txt: Read-only file system",
    "mount: /tmp/ro: Read-only file system",
    "sdc /tmp/dirty fat16 ro 0 0",
    "left dirty",
    "write: cannot open /tmp/dirty/f.txt: Read-only file system",
    "mount: /tmp/dirty: Read-only file system",
    "sda1 /tmp/usb fat16 rw 0 0",
    "sda2 /tmp/usb2 ext2 rw 0 0",
    "eject: vda: Operation not supported",
    "eject: sdz: no such disk",
    "mount: sda1 on /tmp/usb: No such device or address",
    re.compile(
        rf"sda +{IMAGE_KIBIBYTES} {SECTOR_SIZE} usb removable rw ejected"
    ),
    "second eject done",
)
KERNEL_MESSAGES = (
    "KERN/I: Storage: sda: ejected, safe to unplug",
    "KERN/W: VFS: sdd1 is write-protected, mounted read-only",
    "KERN/W: VFS: sdc was not cleanly unmounted (FAT16 dirty bits); mounted "
    "read-only, run fsck.fat",
    "KERN/I: Shutdown: stopping processes",
    "KERN/I: Shutdown: disk flushed",
    "KERN/I: Powering off",
)


def Digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class UsbStorage(Case):
    """Four QEMU usb-storage drives on the xHCI controller, each backed by a
    scratch raw image from create_usb_disks.py: sda, an MBR with FAT16 and
    ext2; sdb, FAT16 on the whole drive; sdc, FAT16 with FAT[1]'s
    clean-shutdown bit cleared; sdd, a copy of sda that QEMU opens read-only
    (the drive reports write protect). The first three are there at boot; sdd
    is plugged in through QMP once the test hook passed, because the AArch64
    firmware crashes on a write-protected USB drive it finds at boot (its mass
    storage driver writes to it). The kernel enumerates them at SuperSpeed,
    binds usb-storage, probes them, and the storage worker opens their
    volumes. The
    test hook lists /proc/partitions, mounts, reads and unmounts sda1 and
    sda2, then writes and reads a pattern in sda's unused tail, checks that
    transfers past the end are refused and that 128 KiB takes several commands,
    all through the registry's disk sda, whose counters it checks. The shell
    then writes the FAT16 volumes (a small file, a long name, a 1 MiB copy,
    mkdir, mv, rm), remounts and reads them, writes ext2 and a superfloppy,
    meets the write-protected and the dirty volume read-only, and shuts down
    with sda1 mounted right after a write. The harness checks the images with
    mtools, e2fsck and, when installed, fsck.fat."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "four USB drives were probed, FAT16, ext2 and a superfloppy were "
        "written and read back, the write-protected and the dirty volume were "
        "mounted read-only, and the images passed the host checks"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.build_dir) / "usb-disks"
        self.images = {}
        self.digests = {}
        self.fsck_note = ""
        self.socket_dir = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.socket_dir.name, "qmp.sock")
        self.connection = None
        self.channel = None
        self.plugged = False
        self.error = None

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        built = usb_disks.Build(self.directory)
        self.images = {
            "sda": built[usb_disks.FAT16_IMAGE],
            "sdb": built[usb_disks.FLOPPY_IMAGE],
            "sdd": self.directory / "usb-protected.img",
            "sdc": self.directory / "usb-dirty-copy.img",
        }
        shutil.copyfile(built[usb_disks.FAT16_IMAGE], self.images["sdd"])
        shutil.copyfile(built[usb_disks.DIRTY_IMAGE], self.images["sdc"])
        self.digests = {
            letter: Digest(self.images[letter]) for letter in ("sdd", "sdc")
        }

    def QmpSocket(self):
        return self.socket_path

    def QemuArguments(self):
        arguments = ["-device", "qemu-xhci,id=xhci"]
        for port, (letter, _, _, protected) in enumerate(DRIVES, start=1):
            if protected:
                continue
            arguments += [
                "-drive",
                f"if=none,id=usb{port},file={self.images[letter]},format=raw",
                "-device",
                f"usb-storage,bus=xhci.0,port={port},drive=usb{port},"
                "removable=on",
            ]
        return arguments

    def Script(self, session):
        return [
            WaitFor("KERN/USBSTORAGE: tests passed"),
            WaitFor("Storage: sdd2: ext2, "),
            *Lines(SHELL_INPUT),
        ]

    def OnOutput(self, session, clean_output):
        if (
            self.plugged
            or b"KERN/USBSTORAGE: tests passed" not in clean_output
        ):
            return
        self.plugged = True
        try:
            self.connection, self.channel = OpenQmpChannel(self.socket_path)
            image = str(self.images["sdd"])
            SendQmpCommand(
                self.channel,
                2,
                "blockdev-add",
                {
                    "driver": "raw",
                    "node-name": "usbprotected",
                    "read-only": True,
                    "file": {
                        "driver": "file",
                        "filename": image,
                        "read-only": True,
                    },
                },
            )
            SendQmpCommand(
                self.channel,
                3,
                "device_add",
                {
                    "driver": "usb-storage",
                    "bus": "xhci.0",
                    "port": str(len(DRIVES)),
                    "drive": "usbprotected",
                    "removable": True,
                    "id": "protected",
                },
            )
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            self.error = str(error)

    def Stop(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()
        self.socket_dir.cleanup()

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Check(self, result):
        output = result.output
        if self.error is not None:
            return f"could not plug in sdd through QMP: {self.error}"
        if PANIC_MARKER in output:
            return f"the kernel panicked{result.limit_note}"
        if "KERN/W: Shutdown:" in output:
            return "the shutdown reported a problem"
        failure = (
            self.CheckBoot(output)
            or self.CheckHook(output)
            or self.CheckShell(output, result)
            or self.CheckImages()
        )
        return failure

    def CheckBoot(self, output):
        if len(DEVICE_LINE.findall(output)) != len(DRIVES):
            return "the boot log lacks a line for each USB storage device"
        for letter, _, kibibytes, protected in DRIVES:
            protection = ", write-protected" if protected else ""
            pattern = re.compile(
                rf"KERN/I: Storage: {letter}: USB 1-\d+, \"QEMU\" "
                rf"\"QEMU HARDDISK\", {kibibytes} KiB, {SECTOR_SIZE}-byte "
                rf"sectors, removable{protection}\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"the probe did not log {letter} as expected"
        for source, kind, kibibytes in VOLUME_LINES:
            pattern = re.compile(
                rf"KERN/I: Storage: {source}: {kind}, {kibibytes} KiB\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"the worker did not log the volume {source}"
        return None

    def CheckHook(self, output):
        if SECTORS_LINE not in output:
            return f"the test hook did not report '{SECTORS_LINE}'"
        position = 0
        for step in STEPS:
            found = output.find(step, position)
            if found < 0:
                return f"the test hook never reported '{step}' in order"
            position = found
        kernel_lines = [
            line for line in output.splitlines() if "KERN/" in line
        ]
        for warning in FAILURES:
            if any(warning in line for line in kernel_lines):
                return f"the log reports a USB storage failure: {warning}"
        return None

    def CheckShell(self, output, result):
        for letter, _, kibibytes, protected in DRIVES:
            state = "ro" if protected else "rw"
            pattern = re.compile(
                rf"^{letter} +{kibibytes} {SECTOR_SIZE} usb removable "
                rf"{state}\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"/proc/partitions lacks the disk line of {letter}"
        for source, kind, kibibytes in VOLUME_LINES:
            pattern = re.compile(
                rf"^{source} +{kibibytes} {SECTOR_SIZE} {kind}\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"/proc/partitions lacks the volume line of {source}"
        for line in SHELL_OUTPUT:
            text = line if isinstance(line, str) else line.pattern
            pattern = re.escape(line) if isinstance(line, str) else text
            if re.search(rf"^{pattern}\r?$", output, re.MULTILINE) is None:
                return (
                    f"the shell session never printed '{text}'"
                    f"{result.limit_note}"
                )
        for message in KERNEL_MESSAGES:
            if message not in output:
                return f"the log lacks '{message}'{result.limit_note}"
        return self.CheckEject(output, result)

    def CheckEject(self, output, result):
        busy = output.count("eject: sda: Device or resource busy")
        if busy != 2:
            return (
                f"eject sda was refused as busy {busy} times, not twice "
                f"(an open file, a working directory){result.limit_note}"
            )
        for mount in ("sda1 /tmp/usb fat16 rw", "sda2 /tmp/usb2 ext2 rw"):
            if output.count(mount) != 1:
                return f"'{mount}' was not mounted exactly until the eject"
        if output.count("ejected, safe to unplug") != 1:
            return "the log did not report the eject exactly once"
        silent = re.compile(
            r"/home> eject sda\r?\n(?:KERN/I: Storage: sda: ejected[^\n]*\n)?"
            r"/home> (?:mount -t fat16 sda1|echo second)",
            re.MULTILINE,
        )
        if len(silent.findall(output)) != 2:
            return "a successful eject printed something or failed"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def VerifyFat16(self, spec, expected, absent):
        for name, contents in expected.items():
            found = Mtype(spec, name)
            if found != contents:
                return f"mtools read different {name} contents"
        listing = Mdir(spec) or ""
        for name in absent:
            if name in listing:
                return f"mtools still lists {name}"
        return None

    def CheckImages(self):
        failure = (
            self.CheckPartitioned()
            or self.CheckFloppy()
            or self.CheckUntouched()
        )
        if FindFsckFat() is None:
            self.fsck_note = "; fsck.fat is not installed, skipped"
        return failure

    def CheckPartitioned(self):
        image = self.images["sda"]
        if not FatCopiesAgree(image, usb_disks.FAT16_START):
            return "sda1: the FAT copies differ"
        spec = MtoolsSpec(image, usb_disks.FAT16_START)
        big = usb_disks.BigContents()
        failure = self.VerifyFat16(
            spec,
            {
                usb_disks.SMALL_FILE: usb_disks.SMALL_CONTENTS,
                usb_disks.BIG_FILE: big,
                "COPY.BIN": big,
                "small.txt": SMALL,
                "A long VFAT name.txt": LONG_NAME,
                "dir/moved.txt": INNER,
                "before-eject.txt": BEFORE_EJECT,
            },
            ("GONE",),
        )
        if failure is not None:
            return f"sda1: {failure}"
        if "INNER" in (Mdir(spec, "dir") or ""):
            return "sda1: mtools still lists dir/inner.txt"
        if FatStatusWord(image, usb_disks.FAT16_START) != 0xFFFF:
            return "sda1: FAT[1] changed"
        checked = FsckFat(
            image,
            usb_disks.FAT16_START,
            usb_disks.FAT16_START + usb_disks.FAT16_SECTORS,
        )
        if checked is not None:
            return f"sda1: {checked}"
        return self.CheckExt2(image)

    def CheckExt2(self, image):
        debugfs = FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "ext2.img"
            volume.write_bytes(
                PartitionBytes(
                    image,
                    usb_disks.EXT2_START,
                    usb_disks.EXT2_START + usb_disks.EXT2_SECTORS,
                )
            )
            if not IsMarkedClean(volume) or NeedsRecovery(volume):
                return "sda2 is not clean after the umount"
            checked = E2fsck(str(volume))
            if checked is not None:
                return f"sda2: {checked}"
            if Debugfs(debugfs, str(volume), "cat /usb-ext2.txt") != EXT2_TEXT:
                return "sda2: debugfs read a different /usb-ext2.txt"
            if NotFound(debugfs, str(volume), "/hello.txt"):
                return "sda2: /hello.txt is gone"
        return None

    def CheckFloppy(self):
        image = self.images["sdb"]
        if not FatCopiesAgree(image):
            return "sdb: the FAT copies differ"
        failure = self.VerifyFat16(
            str(image),
            {
                usb_disks.FLOPPY_FILE: usb_disks.FLOPPY_CONTENTS,
                "NOTE.TXT": FLOPPY_NOTE,
                "LAST.TXT": LAST,
            },
            (),
        )
        if failure is not None:
            return f"sdb: {failure}"
        if FatStatusWord(image) != 0xFFFF:
            return "sdb: FAT[1] changed"
        checked = FsckFat(image, 0, usb_disks.FLOPPY_SECTORS)
        if checked is not None:
            return f"sdb: {checked}"
        return None

    def CheckUntouched(self):
        for letter in ("sdd", "sdc"):
            if Digest(self.images[letter]) != self.digests[letter]:
                return (
                    f"{letter}'s image changed although nothing may write it"
                )
        dirty = self.images["sdc"]
        if FatStatusWord(dirty) & 0x8000:
            return "sdc: the clean-shutdown bit was set by the guest"
        return None

    def PassMessage(self, result):
        return self.passed + self.fsck_note
