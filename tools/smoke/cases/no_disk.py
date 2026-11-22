import re

from smoke.boot import PANIC_MARKER, RESCUE_INIT_MESSAGE, SHELL_BANNER
from smoke.cases.base import INPUT_TIMEOUT_SECONDS, Case
from smoke.script import Lines

NO_DISK_MESSAGE = "KERN/I: Storage: no disk; running from the initramfs"
ROOT_MESSAGE = "KERN/I: VFS: / is tmpfs root, read-write"
ETC_MESSAGE = "KERN/I: VFS: /etc is the initramfs's RAM copy"
ECHOED = re.compile(r"^nodisk ok\r?$", re.MULTILINE)
HOME_DIRECTORY = re.compile(r"^/home\r?$", re.MULTILINE)
FILE_CONTENTS = re.compile(r"^written without a disk\r?$", re.MULTILINE)
ROOT_MOUNT = re.compile(r"^root / tmpfs rw 0 0\r?$", re.MULTILINE)
ETC_MOUNT = re.compile(r"^etc /etc tmpfs rw 0 0\r?$", re.MULTILINE)
HOME_LISTING = re.compile(r"^nodisk\.txt +23\r?$", re.MULTILINE)
SHUTDOWN_MESSAGES = (
    "KERN/I: Shutdown: no disk to flush",
    "KERN/I: Powering off",
)


class NoDisk(Case):
    """QEMU without a virtio-blk device: the kernel logs that it has no disk,
    mounts a tmpfs root holding /home beside the initramfs's /system and the
    RAM copy of /etc, and starts /system/init and a shell, which echoes,
    writes and reads a file under /home, lists the mounts, syncs and shuts the
    machine down."""

    timeout = INPUT_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    has_disk = False
    passed = (
        "no disk: a tmpfs root with /home, /system/init and a shell that "
        "echoed, wrote and read a file, synced and powered off"
    )

    def Script(self, session):
        return Lines(
            b"pwd\n"
            b"echo nodisk ok\n"
            b"write /home/nodisk.txt written without a disk\n"
            b"cat /home/nodisk.txt\n"
            b"ls /home\n"
            b"mount\n"
            b"sync\n"
            b"shutdown\n"
        )

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Check(self, result):
        output = result.output
        if PANIC_MARKER in output:
            return "the kernel panicked"
        for message, failure in (
            (NO_DISK_MESSAGE, "the kernel did not log that it has no disk"),
            (ROOT_MESSAGE, "the root was not the tmpfs"),
            (ETC_MESSAGE, "/etc was not the initramfs's RAM copy"),
            (RESCUE_INIT_MESSAGE, "the kernel did not start /system/init"),
            (SHELL_BANNER, "the shell did not start"),
        ):
            if message not in output:
                return f"{failure}{result.limit_note}"
        for pattern, failure in (
            (HOME_DIRECTORY, "the shell did not start in /home"),
            (ECHOED, "echo did not print"),
            (FILE_CONTENTS, "the file written under /home was not read back"),
            (HOME_LISTING, "ls /home did not list the new file"),
            (ROOT_MOUNT, "/proc/mounts does not list the tmpfs root"),
            (ETC_MOUNT, "/proc/mounts does not list the RAM /etc"),
        ):
            if pattern.search(output) is None:
                return f"{failure}{result.limit_note}"
        for message in SHUTDOWN_MESSAGES:
            if message not in output:
                return f"expected {message!r}{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None
