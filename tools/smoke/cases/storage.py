import re
import tempfile
from pathlib import Path

from create_test_disk import (
    DATA_END,
    DATA_START,
    EXT2_END,
    EXT2_FILES,
    EXT2_START,
    EXT3_END,
    EXT3_FILES,
    EXT3_START,
    FAT16_START,
    ROOT_START,
    ext4_block,
)

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FatCopiesAgree,
    FindE2fsprogs,
    IsMarkedClean,
    Mdir,
    MtoolsSpec,
    Mtype,
    NeedsRecovery,
    NotFound,
    PartitionBytes,
    ReplayOnCopy,
)
from smoke.script import Lines

ETC_DEFAULTS = Path(__file__).resolve().parents[3] / "initramfs" / "etc"
# The init files the kernel test writes in /etc on boot 1 (ServiceFile and
# DisabledFile in kernel_test.cpp): init, started afterwards on both boots,
# runs the service, which prints its marker, and does not start dhcp.
SERVICE_FILE = (
    b"# storage-service-marker\n"
    b"once grep storage-service-marker /etc/init.d/storage\n"
)
DISABLED_FILE = b"# storage test\ndhcp\n"
SERVICE_MARKER = "# storage-service-marker"
DISABLED_MESSAGE = "init: /etc/init.disabled: not starting dhcp"
SECOND_EXT4_MESSAGE = "KERN/STORAGE: two ext4 volumes mounted at once; boot "
PAGE_CACHE_MESSAGE = (
    "KERN/STORAGE: second read of a large file did no disk reads; boot "
)
CACHE_RECLAIM_MESSAGE = (
    "KERN/STORAGE: allocation reclaimed the page cache; boot "
)
STORAGE_MESSAGE = (
    "KERN/STORAGE: sector, FAT16, ext2, ext3 and ext4 file writes verified; "
    "boot count "
)
# The journalled partitions: the ext4 root, ext3 and the data ext4. The kernel
# test (WriteVolumeFirstBoot and ChangeVolumeSecondBoot in kernel_test.cpp)
# writes all three the same way; the root is still mounted when the harness
# looks, and the data volume is mounted by the first boot's shell session.
JOURNALLED_PARTITIONS = {
    "ext3": (EXT3_START, EXT3_END),
    "ext4": (ROOT_START, FAT16_START),
    "data": (DATA_START, DATA_END),
}
REPLAYED = re.compile(
    r"ext2: journal recovered; transactions replayed: 0x([0-9a-fA-F]+)"
)
# What the STORAGE kernel test (WriteVolumeFirstBoot and
# ChangeVolumeSecondBoot in kernel_test.cpp) leaves on each volume after each
# boot.
EXT2_DATA = b"".join(
    b"feather ext2 line %05d\n" % line for line in range(1000)
)
WRITTEN_FILES = {
    1: {
        "/os/data.txt": EXT2_DATA,
        "/os/data-link.txt": EXT2_DATA,
        "/os/sub/moved.txt": b"moved\n",
        "/os/short.txt": b"short\n",
    },
    2: {
        "/os/data-link.txt": EXT2_DATA,
        "/moved-sub/moved.txt": b"moved\n",
        "/os/short.txt": b"short\nmore\n",
    },
}
EXT2_STATS = {
    1: {
        "/os/data-link.txt": "Links: 2",
        "/os/to-data": 'Fast link dest: "data.txt"',
    },
    2: {"/os/data-link.txt": "Links: 1"},
}
EXT2_LISTINGS = {
    1: ("data.txt", "data-link.txt", "to-data", "sub", "short.txt"),
    2: ("data-link.txt", "short.txt"),
}
EXT2_GONE = {
    1: ("gone.txt", "empty", "scratch.txt"),
    2: ("data.txt", "to-data", "sub", "gone.txt"),
}
PERSISTENT_FILE_LINES = 200
# Shell commands for each STORAGE_TEST boot, in /home on the FAT16 partition,
# which nothing mounts at boot: the first writes notes.txt, and the second must
# still find it after the restart. sync is FAT16's device flush, since it has
# no periodic sync. The first boot then writes a file on the ext4 root and on
# the ext3 and ext2 partitions, with no sync or umount, and sleeps past the
# writeback task's 5 s: the harness stops QEMU after that, so only the
# periodic sync can have made ext2 clean and put the two journalled changes in
# their logs, the data volume's mounted beside the root and mounted a second
# time in vain, and two copies of a program made at once, one on the root and
# one on the data volume, beside a write to /tmp (the harness compares them
# with the build's file). The second boot ends by remounting the root
# read-only, which closes its journal, so the harness finds the root clean.
FAT16_SHELL_SETUP = b"mount -t fat16 vda2 /fat\ncd /fat/home\n"
STORAGE_SHELL_INPUT = {
    1: FAT16_SHELL_SETUP + b"write notes.txt first line\n"
    b"write -a notes.txt second line\n"
    b"cat notes.txt\nmkdir docs\nwrite docs/plan.txt nested hello\n"
    b'cat "/fat/read me first.txt"\nwrite "My Plans.txt" long name test\n'
    b"sync\nmount -t ext3 vda3 /ext3\nmount -t ext2 vda6 /ext2\n"
    b"mount -t ext4 vda5 /data\nmount -t ext4 vda5 /d2\n"
    b"grep ext4 /proc/mounts\n"
    b"cp /bin/libctest /data/c1 &\ncp /bin/libctest /c2 &\n"
    b"write /tmp/t parallel &\n"
    b"wait\necho parallel copies done\ncat /tmp/t\n"
    b"write /periodic.txt periodic ext4\n"
    b"write /ext3/periodic.txt periodic ext3\n"
    b"write /data/periodic.txt periodic data\n"
    b"write /ext2/periodic.txt periodic ext2\nsleep 8\nls docs\nls\n",
    2: FAT16_SHELL_SETUP + b"cat notes.txt\nrm notes.txt\ncat notes.txt\n"
    b"rm notes.txt\n"
    b"cat docs/plan.txt\nrmdir docs\nrm docs/plan.txt\nrmdir docs\n"
    b'cat "my plans.txt"\nrm "MY PLANS.TXT"\nsync\n'
    b"mount -o remount,ro /\nwrite /remount.txt x\ngrep vda1 /proc/mounts\n"
    b"mount -o remount,rw /\nwrite /remount.txt remounted\n"
    b"mount -o remount,ro /\ngrep vda1 /proc/mounts\nls docs\nls\n",
}
NOTES_CONTENTS = b"first line\nsecond line\n"
PLAN_CONTENTS = b"nested hello\n"
LONG_NAME_CONTENTS = b"long name test\n"
NOTES_OUTPUT = re.compile(r"^first line\r?\nsecond line\r?$", re.MULTILINE)
# The FAT16 partition's root also holds the kernel test's PERSIST.TXT and
# Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
        "mount: vda5 on /d2: Device or resource busy",
        "vda1 / ext4 rw 0 0",
        "vda5 /data ext4 rw 0 0",
        "parallel copies done",
        "plan.txt",
        "notes.txt",
        "My Plans.txt",
        "docs/",
        "1 directory, 2 files, ",
    ),
    2: (
        "cat: cannot open notes.txt",
        "rm: notes.txt: no such file",
        "\nnested hello",
        "rmdir: docs: Directory not empty",
        "\nlong name test",
        "write: cannot open /remount.txt: Read-only file system",
        "vda1 / ext4 ro 0 0",
        "ls: docs: no such directory",
        "0 files, ",
    ),
}


def VerifyDiskImage(disk_path, boot_count):
    """Checks the guest's FAT16 writes with mtools, a separate FAT reader."""
    if not FatCopiesAgree(disk_path, FAT16_START):
        return "the FAT copies differ"

    image = MtoolsSpec(disk_path, FAT16_START)
    expected = "".join(
        f"FeatherOS boot {boot_count} line {line}\n"
        for line in range(PERSISTENT_FILE_LINES)
    ).encode()
    if Mtype(image, "PERSIST.TXT") != expected:
        return "mtools read different PERSIST.TXT contents"
    listing = Mdir(image)
    if listing is None or "SCRATCH" in listing:
        return "mtools still lists the removed scratch file"
    if boot_count == 1 and Mtype(image, "home/notes.txt") != NOTES_CONTENTS:
        return "mtools read different home/notes.txt contents"
    if boot_count == 1 and Mtype(image, "home/docs/plan.txt") != PLAN_CONTENTS:
        return "mtools read different home/docs/plan.txt contents"
    if (
        boot_count == 1
        and Mtype(image, "home/My Plans.txt") != LONG_NAME_CONTENTS
    ):
        return "mtools read different 'My Plans.txt' contents"
    listed = Mdir(image, "home") or ""
    if boot_count == 2 and any(
        name in listed for name in ("NOTES", "DOCS", "MY PLANS")
    ):
        return "mtools still lists a file or directory the shell removed"
    return None


def VerifyExtVolume(volume, boot_count, name="ext2"):
    """debugfs, a reader independent of the kernel, must see what the guest
    wrote on the volume (name is its type), once e2fsck -fn finds nothing to
    fix."""
    checked = E2fsck(volume)
    if checked is not None:
        return checked
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "debugfs not found; install e2fsprogs"
    files = dict(WRITTEN_FILES[boot_count])
    expected_names = list(EXT2_LISTINGS[boot_count])
    text = f"periodic {name}\n".encode()
    if boot_count == 1:
        files["/periodic.txt"] = text
    elif name != "ext2":
        files["/os/replayed.txt"] = text
        expected_names.append("replayed.txt")
    for path, contents in files.items():
        if Debugfs(debugfs, volume, f"cat {path}") != contents:
            return f"debugfs read different {path} contents"
    for path, stat_text in EXT2_STATS[boot_count].items():
        if stat_text not in Debugfs(debugfs, volume, f"stat {path}").decode():
            return f"debugfs stat {path} does not show {stat_text!r}"
    listing = Debugfs(debugfs, volume, "ls -p /os").decode().splitlines()
    names = {line.split("/")[5] for line in listing if line.count("/") >= 6}
    missing = [name for name in expected_names if name not in names]
    if missing:
        return f"debugfs does not list {missing} in /os"
    left = [name for name in EXT2_GONE[boot_count] if name in names]
    if left:
        return f"debugfs still lists {left} in /os"
    if boot_count == 2 and not NotFound(debugfs, volume, "/periodic.txt"):
        return "debugfs still finds /periodic.txt"
    return None


def VerifyExt2(disk_path, boot_count=None):
    """Checks the ext2 partition, copied out of the disk image: with e2fsck and
    the original files alone without a boot count (after the shell session),
    else also what that boot of the STORAGE test wrote. The volume must be
    marked clean: the writeback task did within 5 s of the shell session's last
    change, and the kernel test unmounted it."""
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "debugfs not found; install e2fsprogs"
    with tempfile.TemporaryDirectory() as work:
        volume = Path(work) / "ext2.img"
        volume.write_bytes(PartitionBytes(disk_path, EXT2_START, EXT2_END))
        if not IsMarkedClean(volume):
            return "the ext2 partition is not marked clean"
        if boot_count is not None:
            return VerifyExtVolume(str(volume), boot_count)
        checked = E2fsck(str(volume))
        if checked is not None:
            return checked
        if (
            Debugfs(debugfs, str(volume), "cat /hello.txt")
            != EXT2_FILES["hello.txt"]
        ):
            return "debugfs read a different /hello.txt"
        if not NotFound(debugfs, str(volume), "/made"):
            return "/made is still there"
    return None


PARALLEL_COPIES = {"ext4": "/c2", "data": "/c1"}


def VerifyParallelCopy(debugfs, volume, name, build_dir):
    """The copy the first boot's shell made on this volume, byte for byte."""
    path = PARALLEL_COPIES.get(name)
    if path is None:
        return None
    original = Path(build_dir) / "disk-programs" / "bin" / "libctest"
    if Debugfs(debugfs, volume, f"cat {path}") != original.read_bytes():
        return f"debugfs read a different {path} than the build's libctest"
    return None


def VerifyJournalled(disk_path, boot_count, build_dir):
    """Checks the ext4 root and ext3 on copies. After the first boot the shell
    session stopped both mounted: the change the writeback task committed is
    only in the log, so e2fsck -fy replays it on the copy. After the second,
    the kernel test replayed ext3's and unmounted it, and the shell remounted
    the root read-only, which closed its journal: both are clean as they are,
    and e2fsck -fn passes without a replay."""
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "e2fsprogs not found; install it"
    for name, (first, end) in JOURNALLED_PARTITIONS.items():
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / f"{name}.img"
            volume.write_bytes(PartitionBytes(disk_path, first, end))
            if boot_count == 1 and not NeedsRecovery(volume):
                return f"{name} was stopped mounted but needs no recovery"
            if boot_count == 1 and not NotFound(
                debugfs, str(volume), "/periodic.txt"
            ):
                return f"{name}: /periodic.txt is on disk before the replay"
            if boot_count == 1:
                failure = ReplayOnCopy(volume)
                if failure is not None:
                    return f"{name}: {failure}"
            elif NeedsRecovery(volume) or not IsMarkedClean(volume):
                return f"{name}: not clean after the second boot"
            failure = VerifyExtVolume(str(volume), boot_count, name)
            if failure is None and name == "ext4":
                failure = VerifyRootEtc(debugfs, str(volume), boot_count)
            if failure is None and name in ("ext4", "data"):
                failure = VerifyBootFiles(
                    debugfs, str(volume), name, boot_count
                )
            if failure is None:
                failure = VerifyParallelCopy(
                    debugfs, str(volume), name, build_dir
                )
            if failure is None and name == "ext4" and boot_count == 2:
                if Debugfs(debugfs, str(volume), "cat /remount.txt") != (
                    b"remounted\n"
                ):
                    failure = "debugfs read a different /remount.txt"
        if failure is not None:
            return f"{name}: {failure}"
    return None


def VerifyBootFiles(debugfs, volume, name, boot_count):
    """What CheckSecondExt4 wrote on each boot, on the volume it names."""
    for boot in range(1, boot_count + 1):
        path = (
            f"/boot{boot}.bin" if name == "data" else f"/boot{boot}-root.txt"
        )
        text = (
            f"data volume boot {boot}\n"
            if name == "data"
            else f"root boot {boot}\n"
        )
        if Debugfs(debugfs, volume, f"cat {path}") != text.encode():
            return f"debugfs read different {path} contents"
    return None


def VerifyRootEtc(debugfs, volume, boot_count):
    """The root's own /etc holds the initramfs defaults, the first boot's edit
    in hosts and resolv.conf, and after the second boot the restored default
    resolv.conf and the second edit in hosts; it has no init.conf, and holds
    the administrator's service and disabled files."""
    first = b"# storage test boot 1\n"
    second = b"# storage test boot 2\n"
    hosts = (ETC_DEFAULTS / "hosts").read_bytes() + first
    resolv = (ETC_DEFAULTS / "resolv.conf").read_bytes()
    if boot_count == 1:
        resolv += first
    else:
        hosts += second
    for name, expected in (
        ("hosts", hosts),
        ("resolv.conf", resolv),
        ("init.d/storage", SERVICE_FILE),
        ("init.disabled", DISABLED_FILE),
    ):
        if Debugfs(debugfs, volume, f"cat /etc/{name}") != expected:
            return f"debugfs read a different /etc/{name}"
    if not NotFound(debugfs, volume, "/etc/init.conf"):
        return "the root's /etc has an init.conf"
    return None


def VerifyJournalledClean(disk_path):
    """After the shell session: ext3 was unmounted, so nothing to recover; the
    root is still mounted, so its log is replayed on the copy first. e2fsck -fn
    then finds nothing, the session's file is there and what it removed is
    not."""
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "debugfs not found; install e2fsprogs"
    extents = b"".join(ext4_block(block) for block in range(2048))
    # The partition's range, where the session removed its files, and a file
    # it must not have changed.
    expected = {
        "ext3": (
            EXT3_START,
            EXT3_END,
            "",
            ("/big.txt", EXT3_FILES["big.txt"]),
        ),
        "ext4": (
            ROOT_START,
            FAT16_START,
            "/samples",
            ("/samples/extents.bin", extents),
        ),
    }
    for name, (first, end, prefix, original) in expected.items():
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / f"{name}.img"
            volume.write_bytes(PartitionBytes(disk_path, first, end))
            if name == "ext3" and NeedsRecovery(volume):
                return f"{name} needs recovery after the unmount"
            if name == "ext4":
                failure = ReplayOnCopy(volume)
                if failure is not None:
                    return f"{name}: {failure}"
            checked = E2fsck(str(volume))
            if checked is not None:
                return f"{name}: {checked}"
            if (
                Debugfs(debugfs, str(volume), "cat /shell.txt")
                != f"shell wrote {name}\n".encode()
            ):
                return f"{name}: debugfs read a different /shell.txt"
            for gone in ("/made", "/copy.bin"):
                if not NotFound(debugfs, str(volume), prefix + gone):
                    return f"{name}: {prefix + gone} is still there"
            if (
                Debugfs(debugfs, str(volume), f"cat {original[0]}")
                != original[1]
            ):
                return f"{name}: {original[0]} changed"
    return None


def CheckSecondBoot(output):
    """The root's and ext3's journals were replayed by writable mounts."""
    if ROOT_MESSAGE not in output:
        return f"the root did not mount read-write: expected {ROOT_MESSAGE!r}"
    replayed = [int(count, 16) for count in REPLAYED.findall(output)]
    if len(replayed) < 2 or min(replayed) < 1:
        return f"expected two journal replays of 1+ transactions: {replayed}"
    return None


class Storage(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.boot = args.expect_storage_boot

    def Script(self, session):
        return Lines(STORAGE_SHELL_INPUT[self.boot])

    def Markers(self):
        return (
            STORAGE_SHELL_OUTPUT[self.boot][-1].encode(),
            PANIC_MARKER.encode(),
        )

    def Check(self, result):
        output = result.output
        expected = f"{STORAGE_MESSAGE}{self.boot}"
        if (
            re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
            is None
        ):
            return f"expected '{expected}'"
        missing = [
            text
            for text in STORAGE_SHELL_OUTPUT[self.boot]
            if text not in output
        ]
        if missing or NOTES_OUTPUT.search(output) is None:
            return (
                "file tools output is missing "
                f"{missing or 'the cat output'}{result.limit_note}"
            )
        if SERVICE_MARKER not in output:
            return "init did not run the /etc/init.d service"
        if DISABLED_MESSAGE not in output:
            return (
                "init did not skip the built-in service in /etc/init.disabled"
            )
        disk_error = VerifyDiskImage(self.args.disk, self.boot)
        if disk_error is not None:
            return f"disk image check: {disk_error}"
        ext2_error = VerifyExt2(self.args.disk, self.boot)
        if ext2_error is not None:
            return f"ext2 check: {ext2_error}"
        for prefix in (
            SECOND_EXT4_MESSAGE,
            PAGE_CACHE_MESSAGE,
            CACHE_RECLAIM_MESSAGE,
        ):
            expected = f"{prefix}{self.boot}"
            if (
                re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
                is None
            ):
                return f"expected '{expected}'"
        if "VFS: two volume locks" in output:
            return "the kernel panicked on two volume locks"
        journalled_error = VerifyJournalled(
            self.args.disk, self.boot, self.args.build_dir
        )
        if journalled_error is not None:
            return f"ext4 root, ext3 and data ext4 check: {journalled_error}"
        if self.boot == 2:
            return CheckSecondBoot(output)
        return None

    def PassMessage(self, result):
        return (
            "sector, FAT16, ext2 and shell file-tool writes verified in the "
            "guest, with mtools, e2fsck and debugfs, two ext4 volumes mounted "
            "at once, and the writeback task synced the ext4 root, ext3, the "
            f"data ext4 and ext2; boot {self.boot} persisted"
        )
