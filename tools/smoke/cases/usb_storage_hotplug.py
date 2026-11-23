import json
import os
import re
import shutil
import tempfile
import time
from pathlib import Path

import create_usb_disks as usb_disks

from smoke.boot import PANIC_MARKER
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.disk_checks import (
    FatCopiesAgree,
    FatStatusWord,
    FindFsckFat,
    FsckFat,
    MtoolsSpec,
    Mtype,
)
from smoke.qemu import OpenQmpChannel
from smoke.script import WaitUntil

MULTILINE = re.MULTILINE
ROUNDS = 3
ROOT_PORT = "1"
SECOND_PORT = "2"
THIRD_PORT = "3"
QMP_ATTEMPTS = 50
MARKER_PREFIX = "HP-"
FAILURES = ("driver could not attach", "enumeration failed")
WARNING = re.compile(r"reset recovery after [a-z-]+ cancelled")
ENUMERATED = re.compile(
    r"KERN/I: USB 1-\d: super-speed 46f4:0001 \"QEMU USB HARDDRIVE\" "
    r"usb-storage\r?$",
    MULTILINE,
)
DISCONNECTED = re.compile(r"KERN/I: USB 1-\d: disconnected\r?$", MULTILINE)
LISTING_LINE = re.compile(
    r"^(?:1-\d+(?:\.\d+)* dev |(?:vd|sd)[a-z]\d* +\d+ \d+ )"
)
ERRORS = (re.compile(r"^(?:umount|mount|write|cat|ls): ", MULTILINE),)
SOURCE_FILE = "/samples/extents.bin"
COPY_HEAD_START_SECONDS = 1.0
COPIES = 3


def Pattern(text):
    return re.compile(text.encode(), MULTILINE)


class Qmp:
    """A QMP connection that keeps the events it reads while it waits for a
    command's answer, so DEVICE_DELETED is still there to wait for."""

    def __init__(self, socket_path):
        self.socket_path = socket_path
        self.connection = None
        self.channel = None
        self.next_id = 2
        self.events = []

    def Open(self):
        for _ in range(QMP_ATTEMPTS):
            try:
                self.connection, self.channel = OpenQmpChannel(
                    self.socket_path
                )
                return
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.1)
        raise RuntimeError("QEMU's QMP socket never came up")

    def Close(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()

    def Read(self):
        message = json.loads(self.channel.readline())
        if "event" in message:
            self.events.append(message)
        return message

    def Send(self, execute, arguments=None):
        identifier = self.next_id
        self.next_id += 1
        request = {"execute": execute, "id": identifier}
        if arguments is not None:
            request["arguments"] = arguments
        self.channel.write(json.dumps(request).encode() + b"\r\n")
        self.channel.flush()
        while True:
            message = self.Read()
            if message.get("id") == identifier:
                if "error" in message:
                    raise RuntimeError(
                        f"{execute}: "
                        f"{message['error'].get('desc', 'QMP error')}"
                    )
                return message

    def WaitDeleted(self, device):
        while True:
            for event in self.events:
                if (
                    event["event"] == "DEVICE_DELETED"
                    and event.get("data", {}).get("device") == device
                ):
                    self.events.remove(event)
                    return
            self.Read()


class UsbStorageHotplug(Case):
    """No USB drive at boot; the harness plugs QEMU usb-storage drives in and
    pulls them out through QMP (blockdev-add under a fresh node name each
    time, device_add, device_del, the DEVICE_DELETED event, blockdev-del).
    Three rounds mount sda1, write, unmount, unplug; lsusb and
    /proc/partitions are then back to the starting lists. Round 4 pulls the
    drive while a copy writes to it: reads and writes fail with EIO, the
    mount shows ro, sync reports EIO, umount works and the drive comes back
    as sda. Round 5 unplugs a mounted drive and plugs another one before the
    umount: it is sdb, and sda returns to the pool only once the dead mount
    is gone. Round 6 pulls a drive as soon as its device descriptor was read,
    in the middle of the probe. The images are checked with mtools."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "USB drives were plugged and unplugged six times, a dead mount "
        "failed with EIO, unmounted without device I/O and gave its name "
        "back, and no panic appeared"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.build_dir) / "usb-hotplug-disks"
        self.images = {}
        self.socket_dir = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.socket_dir.name, "qmp.sock")
        self.qmp = Qmp(self.socket_path)
        self.output = b""
        self.wanted = {}
        self.nodes = 0
        self.plugs = {}
        self.error = None
        self.fsck_note = ""

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.directory.mkdir(parents=True)
        first = self.directory / "hp-a.img"
        usb_disks.BuildPartitioned(first)
        self.images = {"a": first}
        for name in ("b", "c", "d"):
            copy = self.directory / f"hp-{name}.img"
            shutil.copyfile(first, copy)
            self.images[name] = copy

    def QmpSocket(self):
        return self.socket_path

    def QemuArguments(self):
        return ["-device", "qemu-xhci,id=xhci"]

    def OnOutput(self, session, clean_output):
        self.output = clean_output

    def Stop(self):
        self.qmp.Close()
        self.socket_dir.cleanup()

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Do(self, action):
        state = {"done": False}

        def ready():
            if not state["done"] and self.error is None:
                try:
                    action()
                except (OSError, RuntimeError, json.JSONDecodeError) as error:
                    self.error = f"{error}"
                state["done"] = True
            return True

        return WaitUntil(ready)

    def Expect(self, text):
        """Waits for one more copy of a pattern than the script asked for
        before; a log line or a marker."""
        count = self.wanted.get(text, 0) + 1
        self.wanted[text] = count
        pattern = Pattern(text)
        return WaitUntil(
            lambda: (
                self.error is not None
                or len(pattern.findall(self.output)) >= count
            )
        )

    def Marker(self, name):
        """A shell line whose output cannot be told from the log; its upper
        case echo is the marker."""
        return [
            f"echo {MARKER_PREFIX.lower()}{name} | upper\n".encode(),
            self.Expect(rf"^{MARKER_PREFIX}{name.upper()}\r?$"),
        ]

    def Plug(self, image, port, device):
        def action():
            self.nodes += 1
            node = f"hpnode{self.nodes}"
            self.qmp.Send(
                "blockdev-add",
                {
                    "driver": "raw",
                    "node-name": node,
                    "file": {
                        "driver": "file",
                        "filename": str(self.images[image]),
                    },
                },
            )
            self.qmp.Send(
                "device_add",
                {
                    "driver": "usb-storage",
                    "bus": "xhci.0",
                    "port": port,
                    "drive": node,
                    "removable": True,
                    "id": device,
                },
            )
            self.nodes_of[device] = node

        self.plugs[port] = self.plugs.get(port, 0) + 1
        return self.Do(action)

    def Enumerated(self, port):
        text = (
            rf"KERN/I: USB 1-{port}: super-speed 46f4:0001 "
            r"\"QEMU USB HARDDRIVE\" usb-storage"
        )
        self.wanted[text] = self.plugs[port] - 1
        return self.Expect(text)

    def Unplug(self, device, remove_node=True):
        def action():
            self.qmp.Send("device_del", {"id": device})
            self.qmp.WaitDeleted(device)
            if remove_node:
                self.qmp.Send(
                    "blockdev-del", {"node-name": self.nodes_of[device]}
                )

        return self.Do(action)

    def Connect(self):
        self.qmp.Open()

    def Gone(self, letter, port):
        return [
            self.Expect(rf"KERN/I: Storage: {letter}: gone"),
            self.Expect(rf"KERN/I: USB 1-{port}: disconnected"),
        ]

    def Listing(self, name):
        return [
            *self.Marker(f"{name}begin"),
            b"lsusb\n",
            b"cat /proc/partitions\n",
            *self.Marker(f"{name}end"),
        ]

    def PlugLine(self, letter, port):
        return rf"KERN/I: Storage: {letter}: USB 1-{port}, "

    def Rounds(self, count):
        steps = []
        for number in range(1, count + 1):
            steps += [
                self.Plug("a", ROOT_PORT, f"hp{number}"),
                self.Expect(self.PlugLine("sda", ROOT_PORT)),
                self.Expect(r"KERN/I: Storage: sda2: ext2, "),
                b"mount -t fat16 sda1 /tmp/usb\n",
                f"write /tmp/usb/r{number}.txt round {number}\n".encode(),
                f"cat /tmp/usb/r{number}.txt\n".encode(),
                b"ls /tmp/usb\n",
                b"umount /tmp/usb\n",
                *self.Marker(f"round{number}"),
                self.Unplug(f"hp{number}"),
                *self.Gone("sda", ROOT_PORT),
            ]
        return steps

    def MidTransfer(self):
        return [
            self.Plug("b", ROOT_PORT, "hp4"),
            self.Expect(self.PlugLine("sda", ROOT_PORT)),
            self.Expect(r"KERN/I: Storage: sda2: ext2, "),
            b"mount -t fat16 sda1 /tmp/usb\n",
            b"write /tmp/usb/x written before the unplug\n",
            *[
                f"cp {SOURCE_FILE} /tmp/usb/big{number} &\n".encode()
                for number in range(1, COPIES + 1)
            ],
            self.Do(lambda: time.sleep(COPY_HEAD_START_SECONDS)),
            self.Unplug("hp4"),
            *self.Gone("sda", ROOT_PORT),
            b"sleep 1\n",
            b"jobs\n",
            b"cat /tmp/usb/x\n",
            b"grep sda1 /proc/mounts\n",
            b"sync\n",
            b"echo sync status done\n",
            b"cat /proc/partitions\n",
            *self.Marker("dead"),
            b"umount /tmp/usb\n",
            b"grep -c sda1 /proc/mounts\n",
            *self.Marker("freed"),
            self.Plug("b", ROOT_PORT, "hp4b"),
            self.Expect(self.PlugLine("sda", ROOT_PORT)),
            self.Expect(r"KERN/I: Storage: sda2: ext2, "),
            b"mount -t fat16 sda1 /tmp/usb\n",
            b"cat /tmp/usb/x\n",
            b"umount /tmp/usb\n",
            *self.Marker("replugged"),
            self.Unplug("hp4b"),
            *self.Gone("sda", ROOT_PORT),
        ]

    def NameReuse(self):
        return [
            self.Plug("c", ROOT_PORT, "hp5"),
            self.Expect(self.PlugLine("sda", ROOT_PORT)),
            self.Expect(r"KERN/I: Storage: sda2: ext2, "),
            b"mount -t fat16 sda1 /tmp/usb\n",
            b"write /tmp/usb/five.txt five\n",
            b"cat /tmp/usb/five.txt\n",
            *self.Marker("written"),
            self.Unplug("hp5"),
            *self.Gone("sda", ROOT_PORT),
            self.Plug("d", SECOND_PORT, "hp5b"),
            self.Expect(self.PlugLine("sdb", SECOND_PORT)),
            self.Expect(r"KERN/I: Storage: sdb2: ext2, "),
            b"cat /proc/partitions\n",
            b"grep sda1 /proc/mounts\n",
            *self.Marker("both"),
            b"umount /tmp/usb\n",
            *self.Marker("unmounted"),
            self.Plug("c", THIRD_PORT, "hp5c"),
            self.Expect(self.PlugLine("sda", THIRD_PORT)),
            self.Expect(r"KERN/I: Storage: sda2: ext2, "),
            b"cat /proc/partitions\n",
            *self.Marker("again"),
            self.Unplug("hp5b"),
            *self.Gone("sdb", SECOND_PORT),
            self.Unplug("hp5c"),
            *self.Gone("sda", THIRD_PORT),
        ]

    def DuringProbe(self):
        return [
            self.Plug("a", ROOT_PORT, "hp6"),
            self.Enumerated(ROOT_PORT),
            self.Unplug("hp6"),
            self.Expect(rf"KERN/I: USB 1-{ROOT_PORT}: disconnected"),
            b"sleep 1\n",
        ]

    def Script(self, session):
        self.nodes_of = {}
        return [
            self.Do(self.Connect),
            b"mkdir /tmp/usb\n",
            *self.Listing("start"),
            *self.Rounds(ROUNDS),
            b"sleep 1\n",
            *self.Listing("end"),
            *self.MidTransfer(),
            *self.NameReuse(),
            *self.DuringProbe(),
            b"sleep 1\n",
            *self.Listing("final"),
            b"shutdown\n",
        ]

    def Check(self, result):
        output = result.output
        if self.error is not None:
            return f"the QMP driver failed: {self.error}{result.limit_note}"
        if PANIC_MARKER in output:
            return f"the kernel panicked{result.limit_note}"
        if "KERN/W: Shutdown:" in output:
            return "the shutdown reported a problem"
        return (
            self.CheckRounds(output)
            or self.CheckMidTransfer(output)
            or self.CheckNameReuse(output)
            or self.CheckListings(output)
            or self.CheckLog(output)
            or self.CheckEnd(result)
            or self.CheckImages()
        )

    def CheckLog(self, output):
        kernel_lines = [
            line[line.index("KERN/") :]
            for line in output.splitlines()
            if "KERN/" in line
        ]
        for line in kernel_lines:
            if any(text in line for text in FAILURES):
                return f"the log reports a USB storage failure: {line}"
            if line.startswith("KERN/W: ") and not WARNING.search(line):
                return f"the log has an unexpected warning: {line}"
        arrived = len(ENUMERATED.findall(output))
        left = len(DISCONNECTED.findall(output))
        if arrived != left or arrived != sum(self.plugs.values()):
            return (
                f"{arrived} drives were enumerated and {left} disconnected "
                f"for {sum(self.plugs.values())} plugs"
            )
        return None

    def Between(self, output, first, last):
        start = re.search(
            rf"^{MARKER_PREFIX}{first.upper()}\r?$", output, MULTILINE
        )
        end = re.search(
            rf"^{MARKER_PREFIX}{last.upper()}\r?$", output, MULTILINE
        )
        if start is None or end is None or end.start() < start.end():
            return None
        return output[start.end() : end.start()]

    def CheckListings(self, output):
        listings = {}
        for name in ("start", "end", "final"):
            section = self.Between(output, f"{name}begin", f"{name}end")
            if section is None:
                return f"the {name} listing never printed"
            listings[name] = [
                line
                for line in section.splitlines()
                if LISTING_LINE.match(line)
            ]
        if not any(line.startswith("vda ") for line in listings["start"]):
            return f"the starting listing lacks vda: {listings['start']}"
        for name in ("end", "final"):
            if listings[name] != listings["start"]:
                return (
                    f"the {name} lsusb and /proc/partitions differ from the "
                    f"start: {listings[name]} against {listings['start']}"
                )
        return None

    def CheckRounds(self, output):
        for number in range(1, ROUNDS + 1):
            section = self.Between(
                output,
                "startend" if number == 1 else f"round{number - 1}",
                f"round{number}",
            )
            if section is None:
                return f"round {number} never finished"
            if re.search(rf"^round {number}\r?$", section, MULTILINE) is None:
                return f"round {number} did not read its file back"
            for earlier in range(1, number + 1):
                if (
                    re.search(rf"^r{earlier}\.txt +8\r?$", section, MULTILINE)
                    is None
                ):
                    return f"round {number}: ls lacks r{earlier}.txt"
            for error in ERRORS:
                if error.search(section):
                    return f"round {number} printed an error: {error.pattern}"
        return None

    def CheckMidTransfer(self, output):
        dead = self.Between(output, "endend", "dead")
        if dead is None:
            return "round 4 never reached its dead-mount checks"
        if "KERN/I: Storage: sda: gone" not in dead:
            return "round 4: the drive was not logged gone"
        copies = re.findall(r"^cp: /tmp/usb/big\d: (.*?)\r?$", dead, MULTILINE)
        if any(reason != "Input/output error" for reason in copies):
            return f"round 4: a copy failed with {copies}, not EIO"
        wanted = (
            (
                r"^cat: cannot open /tmp/usb/x: Input/output error\r?$",
                "cat of a file on the dead volume does not give EIO",
            ),
            (
                r"^sda1 /tmp/usb fat16 ro 0 0\r?$",
                "the dead mount is not shown as ro",
            ),
            (r"^sync: Input/output error\r?$", "sync did not report EIO"),
            (r"^sync status done\r?$", "sync did not return"),
        )
        for pattern, message in wanted:
            if re.search(pattern, dead, MULTILINE) is None:
                return f"round 4: {message}"
        if re.search(r"^sd[a-z]\d* +\d+ \d+ ", dead, MULTILINE):
            return "round 4: /proc/partitions still lists the dead disk"
        freed = self.Between(output, "dead", "freed")
        if freed is None:
            return "round 4 never unmounted the dead volume"
        for error in ERRORS:
            if error.search(freed):
                return "round 4: umount of the dead volume failed"
        if re.search(r"^0\r?$", freed, MULTILINE) is None:
            return (
                "round 4: the dead mount is still in /proc/mounts after umount"
            )
        replugged = self.Between(output, "freed", "replugged")
        if replugged is None:
            return "round 4 never replugged the drive"
        if "Storage: sda: USB 1-1, " not in replugged:
            return "round 4: the replugged drive is not sda"
        if (
            re.search(r"^written before the unplug\r?$", replugged, MULTILINE)
            is None
        ):
            return "round 4: the replugged drive lost the file written before"
        for error in ERRORS:
            if error.search(replugged):
                return f"round 4: replug printed {error.pattern}"
        return None

    def CheckNameReuse(self, output):
        before = self.Between(output, "replugged", "written")
        if before is None or "Storage: sdb" in before:
            return "round 5: sdb appeared before a second drive was plugged"
        both = self.Between(output, "written", "both")
        if both is None:
            return "round 5 never listed the two drives"
        if "Storage: sda: USB 1-2" in both:
            return "round 5: the second drive took the dead mount's name"
        if "Storage: sdb: USB 1-2, " not in both:
            return "round 5: the second drive is not sdb"
        if (
            re.search(r"^sdb +65536 512 usb removable rw\r?$", both, MULTILINE)
            is None
        ):
            return "round 5: /proc/partitions lacks sdb"
        if re.search(r"^sda\d? +\d+ \d+ ", both, MULTILINE):
            return "round 5: /proc/partitions lists the dead sda"
        if (
            re.search(r"^sda1 /tmp/usb fat16 ro 0 0\r?$", both, MULTILINE)
            is None
        ):
            return "round 5: the dead mount of sda1 is not listed as ro"
        unmounted = self.Between(output, "both", "unmounted")
        if unmounted is None or any(
            error.search(unmounted) for error in ERRORS
        ):
            return "round 5: umount of the dead volume failed"
        again = self.Between(output, "unmounted", "again")
        if again is None:
            return "round 5 never plugged the third drive"
        if "Storage: sda: USB 1-3, " not in again:
            return "round 5: the third drive did not get sda back"
        for name in ("sda", "sda1", "sda2", "sdb", "sdb1", "sdb2"):
            if re.search(rf"^{name} +\d+ \d+ ", again, MULTILINE) is None:
                return f"round 5: /proc/partitions lacks {name} at the end"
        return None

    def CheckEnd(self, result):
        output = result.output
        line = (
            "Storage: USB 1-1: unplugged during the probe" in output
            or output.count("Storage: sda: gone") > 7
        )
        if not line:
            return "round 6 left no log line about the unplug during the probe"
        for message in (
            "KERN/I: Shutdown: disk flushed",
            "KERN/I: Powering off",
        ):
            if message not in output:
                return f"the log lacks '{message}'{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckImages(self):
        for name in ("a", "b", "c"):
            image = self.images[name]
            if not FatCopiesAgree(image, usb_disks.FAT16_START):
                return f"image {name}: the FAT copies differ"
            if FatStatusWord(image, usb_disks.FAT16_START) != 0xFFFF:
                return f"image {name}: FAT[1] changed"
        spec = MtoolsSpec(self.images["a"], usb_disks.FAT16_START)
        for number in range(1, ROUNDS + 1):
            if Mtype(spec, f"r{number}.txt") != f"round {number}\n".encode():
                return f"mtools reads another r{number}.txt from image a"
        if Mtype(spec, usb_disks.SMALL_FILE) != usb_disks.SMALL_CONTENTS:
            return "image a lost USBFILE.TXT"
        checked = FsckFat(
            self.images["a"],
            usb_disks.FAT16_START,
            usb_disks.FAT16_START + usb_disks.FAT16_SECTORS,
        )
        if checked is not None:
            return f"image a: {checked}"
        spec = MtoolsSpec(self.images["b"], usb_disks.FAT16_START)
        if Mtype(spec, "x") != b"written before the unplug\n":
            return "mtools reads another x from image b"
        spec = MtoolsSpec(self.images["c"], usb_disks.FAT16_START)
        if Mtype(spec, "five.txt") != b"five\n":
            return "mtools reads another five.txt from image c"
        if FindFsckFat() is None:
            self.fsck_note = "; fsck.fat is not installed, skipped"
        return None

    def PassMessage(self, result):
        return self.passed + self.fsck_note
