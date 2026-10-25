#!/usr/bin/env python3

import argparse
import datetime
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

READY_MESSAGE = "KERN/I: Kernel initialization complete"
TIMER_STARTED_MESSAGE = "KERN/I: Timer started at 100 Hz"
SHELL_BANNER = "FeatherOS shell. Type 'help' for commands."
VFS_MESSAGE = "KERN/I: VFS: hello.txt read passed"
FAT16_MESSAGE = "KERN/I: VFS: FAT16 BLOCK.TXT read passed"
ELF_LOAD_MESSAGE = "KERN/I: ELF64: test image loaded into user address space"
USER_MODE_MESSAGES = {
    "x86_64": "KERN/I: ELF64: ring 3 console and exit syscalls passed",
    "aarch64": "KERN/I: ELF64: EL0 console and exit syscalls passed",
}
USER_ISOLATION_MESSAGES = {
    "x86_64": "KERN/I: ELF64: ring 3 kernel-memory isolation passed",
    "aarch64": "KERN/I: ELF64: EL0 kernel-memory isolation passed",
}
USER_MODE_OUTPUTS = {
    "x86_64": (
        b"UKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
    "aarch64": (
        b"AKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
}
PANIC_MARKER = "KERN/PANIC:"
EXPECTED_PANIC = "KERN/PANIC: Intentional panic test"
EXPECTED_EXCEPTION = {
    "x86_64": "KERN/EXCEPTION: Divide error",
    "aarch64": "KERN/EXCEPTION: AArch64 undefined instruction",
}
EXPECTED_INTERRUPT = {
    "x86_64": "KERN/IRQ: PIT interrupt acknowledged",
    "aarch64": "KERN/IRQ: GIC software interrupt acknowledged",
}
MAX_CPU_PROTECTIONS = {
    "x86_64": "KERN/I: CPU protections: SMEP SMAP RDRAND",
    "aarch64": "KERN/I: CPU protections: PAN RNDR",
}
EXPECTED_ARCHITECTURE = {
    "x86_64": "KERN/I: Architecture: x86_64",
    "aarch64": "KERN/I: Architecture: aarch64",
}
EXPECTED_MEMORY = "KERN/MEMORY: tests passed"
EXPECTED_TIMER = "KERN/TIMER: periodic ticks passed"
EXPECTED_CALC = "69.25"
EXPECTED_UNKNOWN_COMMAND = "Unknown command: nope"
CALC_INPUT = b"nope\n\ncalc 4 + 66 - (3 / 4)\n"
# Covers argv, foreground input in a child, and nonzero exit statuses.
SHELL_INPUT = (
    b"echo hello   feather os\nupper\nmixed Case\ndate\nuptime\nfanout 6\n"
    b'date --help\nprimes -h\nfiletest\nlibctest one "two words"\n'
    # libctest reads these at its "> " prompts: read(), fgets(), scanf().
    b"typed into stdin\nread by fgets\n42 answer\nenv\n"
    b"mkdir work\ncd work\nwrite a.txt in work\npwd\ncat a.txt\ncd ..\n"
    b"cat work/a.txt\ncd /\nls\ncat hello.txt\ncd nowhere\ncd\npwd\n"
    b"ls /dev\nwrite /tmp/note.txt kept in tmp\ncat /tmp/note.txt\n"
    b"cat /proc/self/status\ncat /proc/meminfo\n"
    b"echo first redirected line > /tmp/r.txt\n"
    b"echo second redirected line >> /tmp/r.txt\n"
    b"upper < /tmp/r.txt\ncat < /tmp/r.txt\ncat < /tmp/none\n"
    b"echo through the pipe | upper\nls /dev|upper\n"
    b"echo two stages|upper|cat > /tmp/p.txt\ncat /tmp/p.txt\n"
    b"echo a | | upper\n"
    # A pipeline is one foreground group, so its first command can read the
    # console.
    b"upper | upper\ntwo uppers\n"
    b"upper &\nsleep 1000 &\njobs\nwait\nsleep 800 &\nfg\n"
    # A background job that starts a program of its own must not take the
    # console from the shell.
    b"primes 1 1000000 > /dev/null &\njobs\n"
    b"echo the shell kept the console\nwait\n"
    b"kill -l\nkill 99999\nkill -TERM 1\n"
    # top must show a busy background worker using most of a CPU, which
    # kill %1 then stops.
    b"ps\nprimes 1 1000000000 > /dev/null &\ntop -n 1 -d 500\nkill %1\nwait\n"
    b"kill %7\n"
    b"exit\n"
    b"memtest\nmemtest overflow\nmemtest exhaust\nmemtest exhaust\n"
    b"sleep 1100\nhardening\naslr\naslr\nprimes 4 100000\nprimes 16 20000\n"
    b"fptest 8\n"
    b"calc\n1 / 3\n7 / 0\nexit\n"
)


class WholeWrite(bytes):
    """Input written in one piece, so escape sequences arrive together."""


# Line editing at the shell prompt: Home and Right, Up to recall, Ctrl+U and
# Ctrl+W, Delete, and a lone Escape, which the editor ignores.
SHELL_EDIT_INPUT = (
    WholeWrite(b"eco edited line\x1b[H\x1b[C\x1b[Ch\n"),
    WholeWrite(b"\x1b[A\n"),
    WholeWrite(b"echo wrong\x15echo right words\x17\x17after-kill\n"),
    WholeWrite(b"echo deleteXX\x1b[D\x1b[D\x1b[3~\x1b[3~ done\n"),
    WholeWrite(b"echo esc\x1b ok\n"),
    b"history\n",
    b"clear\n",
)
CLEAR_SEQUENCE = b"\x1b[H\x1b[2J"
# Program output lines, which the edited input lines echoed before them do not
# match, and how often each must appear.
EXPECTED_SHELL_EDITS = {
    "edited line": 2,
    "after-kill": 1,
    "delete done": 1,
    "esc ok": 1,
}
EXPECTED_HISTORY_LINE = re.compile(
    r"^ +\d+  echo edited line\r?$", re.MULTILINE
)
KEYS_READY = "keys: press keys; q quits"
# keys reads raw input: every special key, Ctrl+C as a byte, then q to quit.
SHELL_KEYS_INPUT = (
    b"keys\n",
    WholeWrite(
        b"\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[2~\x1b[3~\x1b[5~\x1b[6~"
        b"\x1b[Z\x1bOA\x1b[1~\t\x01\x03\x7f \rxq"
    ),
)
EXPECTED_KEYS = [
    KEYS_READY,
    "keys: up",
    "keys: down",
    "keys: right",
    "keys: left",
    "keys: home",
    "keys: end",
    "keys: insert",
    "keys: delete",
    "keys: page-up",
    "keys: page-down",
    "keys: shift-tab",
    "keys: up",
    "keys: home",
    "keys: tab",
    "keys: ctrl-a",
    "keys: ctrl-c",
    "keys: backspace",
    "keys: space",
    "keys: enter",
    "keys: x",
    "keys: done",
]
# Ctrl+C for a command that shows no prompt of its own, sent while it runs.
INTERRUPT_RUNNING = object()
INTERRUPT_DELAY_SECONDS = 1.5
CONTROL_C = b"\x03"
# Interrupts a CPU-bound job, a sleep, programs blocked reading input, and the
# shell's own prompt (which only discards the line). The shell test's last
# command must stay last because its output ends the run.
SHELL_INTERRUPT_INPUT = (
    b"primes 1 1000000000\n",
    INTERRUPT_RUNNING,
    # The reader of an interrupted writer sees its input end and exits too.
    b"primes 1 1000000000 | upper\n",
    INTERRUPT_RUNNING,
    # Ctrl+C reaches every command in the foreground group.
    b"sleep 100000 | sleep 100000\n",
    INTERRUPT_RUNNING,
    b"sleep 100000\n",
    INTERRUPT_RUNNING,
    b"upper\n",
    CONTROL_C,
    b"calc\n",
    CONTROL_C,
    CONTROL_C,
    b"calc 1 +\n",
)
EXPECTED_INTERRUPTS = 7
# `date` prints only the timestamp, unlike the boot log's "Wall clock:" line.
DATE_OUTPUT = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\r?$", re.MULTILINE
)
# pwd prints the directory alone; prompts follow it with "> ".
WORKING_DIRECTORY_OUTPUT = re.compile(r"^(/home(?:/work)?)\r?$", re.MULTILINE)
WORK_FILE_OUTPUT = re.compile(r"^in work\r?$", re.MULTILINE)
UPTIME_OUTPUT = re.compile(r"^uptime: \d+:\d{2}:\d{2}\r?$", re.MULTILINE)
PRIMES_WORKER_OUTPUT = re.compile(
    r"^primes: worker \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)
EXHAUST_OUTPUT = re.compile(
    r"^memtest: page limit reached after (\d+) heap pages, twice\r?$",
    re.MULTILINE,
)
# The 1024-page limit also covers the stack, the arguments page, and the
# image, which is about 20 pages with the C library linked in.
MINIMUM_EXHAUST_PAGES = 960
ASLR_OUTPUT = re.compile(
    r"^aslr: image=(\d+) stack=(\d+) args=(\d+) heap=(\d+)\r?$", re.MULTILINE
)
# QEMU's RTC follows the host clock; allow for boot time and slow hosts.
MAXIMUM_CLOCK_SKEW_SECONDS = 120
EXPECTED_SHELL_OUTPUT = (
    "init: FeatherOS is starting its services",
    # init restarts the console service after the shell's exit command.
    "init: shell exited with status 0",
    "init: restarting shell",
    "usage: date",
    "usage: primes <workers 1-16> [limit]",
    "filetest: 33 checks passed",
    "libctest: standard output works",
    "libctest: standard error works",
    "libctest: printf works 7",
    "libctest: dprintf 3",
    "libctest: fprintf to stderr",
    "libctest: perror: No such file or directory",
    "libctest: puts works",
    "Assertion `argc == 1' failed.",
    "spawned by posix_spawn",
    "SPAWNED=yes",
    "libctest: 359 checks passed",
    "libctest: written through dup",
    "libctest: stdout is back after posix_spawn",
    "libctest: exit handlers ran in order",
    # env prints the variables init.conf sets, which the shell passes on.
    "HOME=/home",
    "PATH=/system",
    "system/",
    "home/",
    "FeatherOS initramfs is ready.",
    "cd: nowhere: no such directory",
    # /dev, /tmp, and /proc; the console shows tabs as spaces.
    "random                  0",
    "kept in tmp",
    "Name: cat",
    "MemTotal: ",
    # Redirection; cat's lines start lines of their own, unlike the typed
    # commands that contain the same words.
    "FIRST REDIRECTED LINE",
    "SECOND REDIRECTED LINE",
    "\nfirst redirected line",
    "\nsecond redirected line",
    "shell: /tmp/none: No such file or directory",
    # Pipelines; the typed commands are lower case.
    "THROUGH THE PIPE",
    "RANDOM",
    "\nTWO STAGES",
    "shell: a pipeline has an empty command",
    "TWO UPPERS",
    # Background jobs: upper cannot read the console there and fails.
    "Running  sleep 1000 &",
    "Done  sleep 1000 &",
    "Exit 1  upper &",
    "\nsleep 800 &",
    "\nthe shell kept the console",
    "Done  primes 1 1000000 > /dev/null &",
    " 9 KILL",
    "kill: 99999: no such process",
    "kill: 1: Operation not permitted",
    "  PID  PPID  PGID S CPU     TIME NAME",
    "  PID S  %CPU CPU     TIME NAME",
    "top: up ",
    "Terminated  primes 1 1000000000 > /dev/null &",
    "kill: %7: no such job",
    "hello feather os",
    "upper> ",
    "MIXED CASE",
    "fanout: 6 children exited",
    "memtest: stack and heap ok",
    # memtest overflow ends at the stack guard page with ExitStatusFault.
    "shell: program terminated by a CPU exception",
    "sleep: slept 1100 ms",
    "hardening: 48 checks passed",
    "fptest: 8 of 8 workers kept their registers",
    "primes: 9592 primes below 100000 with 4 workers in ",
    # Sixteen workers keep sixteen pipes open at once.
    "primes: 2262 primes below 20000 with 16 workers in ",
    "calc> ",
    "0.333333",
    "calc: division by zero",
    "calc: unexpected end of expression",
    "exit status 1",
)
EXPECTED_SCHEDULER = (
    "KERN/SCHEDULER: preemption, wait, exit, reap, blocking, sleep, "
    "and priorities passed"
)
EXPECTED_USER_PREEMPTION = "KERN/SCHEDULER: user-mode preemption passed"
EXPECTED_SMP = (
    "KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact; "
    "busy CPUs counted"
)
POWER_MESSAGES = {
    "shutdown": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Powering off",
    ),
    "reboot": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Restarting",
    ),
}
STORAGE_MESSAGE = (
    "KERN/STORAGE: sector and FAT16 file writes verified; boot count "
)
PERSISTENT_FILE_LINES = 200
# Shell commands for each STORAGE_TEST boot: the first writes notes.txt, and
# the second must still find it after the restart.
STORAGE_SHELL_INPUT = {
    1: b"write notes.txt first line\nwrite -a notes.txt second line\n"
    b"cat notes.txt\nmkdir docs\nwrite docs/plan.txt nested hello\n"
    b'cat "/read me first.txt"\nwrite "My Plans.txt" long name test\n'
    b"ls docs\nls\n",
    2: b"cat notes.txt\nrm notes.txt\ncat notes.txt\nrm notes.txt\n"
    b"cat docs/plan.txt\nrmdir docs\nrm docs/plan.txt\nrmdir docs\n"
    b'cat "my plans.txt"\nrm "MY PLANS.TXT"\nls docs\nls\n',
}
NOTES_CONTENTS = b"first line\nsecond line\n"
PLAN_CONTENTS = b"nested hello\n"
LONG_NAME_CONTENTS = b"long name test\n"
NOTES_OUTPUT = re.compile(r"^first line\r?\nsecond line\r?$", re.MULTILINE)
# The disk also holds BLOCK.TXT and the kernel test's PERSIST.TXT.
# The shell runs in /home; the disk's root also holds BLOCK.TXT, the kernel
# test's PERSIST.TXT, and Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
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
        "rmdir: docs: not an empty directory",
        "\nlong name test",
        "ls: docs: no such directory",
        "0 files, ",
    ),
}
USER_PREEMPTION_OUTPUTS = {"x86_64": b"UIu", "aarch64": b"AIa"}
# Ctrl+C at the shell prompt only discards the line; "ab" is then an unknown
# command, and keys takes raw input from the same keyboard.
INPUT_LINE_KEYS = (
    "ctrl-c",
    "a",
    "b",
    "x",
    "backspace",
    "ret",
    # Left then z gives "echo azb"; Up recalls it.
    "e",
    "c",
    "h",
    "o",
    "spc",
    "a",
    "b",
    "left",
    "z",
    "ret",
    "up",
    "ret",
    "k",
    "e",
    "y",
    "s",
    "ret",
)
INPUT_RAW_KEYS = ("up", "home", "delete", "esc", "ctrl-a", "shift-tab", "q")
EXPECTED_INPUT_KEYS = [
    KEYS_READY,
    "keys: up",
    "keys: home",
    "keys: delete",
    "keys: escape",
    "keys: ctrl-a",
    "keys: shift-tab",
    "keys: done",
]
# The shell's prompt shows its working directory, such as "/home> ".
SHELL_PROMPT = re.compile(rb"^/[^\n>]*> ", re.MULTILINE)
# Each scripted shell line is typed only once a prompt asks for it, so
# typed-ahead input never overflows the kernel's input queue.
LINE_PROMPT = re.compile(
    rb"^(?:(?:upper|calc|/[^\n>]*)?> |keys: press keys; q quits\r?$)",
    re.MULTILINE,
)
INPUT_ECHO_SEQUENCE = b"abx\x08 \x08\r\n"
QEMU_TIMEOUT_SECONDS = 10
# The scripted shell session runs many programs; AArch64 emulation is slower.
SHELL_TIMEOUT_SECONDS = 60
QEMU_CPU_COUNT = 4
SMP_MESSAGE = f"KERN/I: SMP: {QEMU_CPU_COUNT} CPUs online with IPIs and timers"
ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
ANSI_ESCAPE_BYTES = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")


def PartitionOffset(disk):
    """Byte offset of the first partition, from a GPT or the MBR."""
    mbr = disk.read(512)
    if mbr[446 + 4] != 0xEE:
        return struct.unpack_from("<I", mbr, 446 + 8)[0] * 512
    disk.seek(512)
    header = disk.read(512)
    disk.seek(struct.unpack_from("<Q", header, 72)[0] * 512)
    return struct.unpack_from("<Q", disk.read(128), 32)[0] * 512


def VerifyDiskImage(disk_path, boot_count):
    """Checks the guest's FAT16 writes with mtools, a separate FAT reader."""
    with open(disk_path, "rb") as disk:
        partition_offset = PartitionOffset(disk)
        disk.seek(partition_offset)
        boot_sector = disk.read(512)
        reserved_sectors = struct.unpack_from("<H", boot_sector, 14)[0]
        fat_count = boot_sector[16]
        fat_bytes = struct.unpack_from("<H", boot_sector, 22)[0] * 512
        disk.seek(partition_offset + reserved_sectors * 512)
        fats = [disk.read(fat_bytes) for _ in range(fat_count)]
    if any(fat != fats[0] for fat in fats):
        return "the FAT copies differ"

    image = f"{disk_path}@@{partition_offset}"
    environment = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    expected = "".join(
        f"FeatherOS boot {boot_count} line {line}\n"
        for line in range(PERSISTENT_FILE_LINES)
    ).encode()
    contents = subprocess.run(
        ["mtype", "-i", image, "::PERSIST.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if contents.returncode != 0 or contents.stdout != expected:
        return "mtools read different PERSIST.TXT contents"
    listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if listing.returncode != 0 or "SCRATCH" in listing.stdout:
        return "mtools still lists the removed scratch file"
    notes = subprocess.run(
        ["mtype", "-i", image, "::home/notes.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    plan = subprocess.run(
        ["mtype", "-i", image, "::home/docs/plan.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and notes.stdout != NOTES_CONTENTS:
        return "mtools read different home/notes.txt contents"
    long_name = subprocess.run(
        ["mtype", "-i", image, "::home/My Plans.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and plan.stdout != PLAN_CONTENTS:
        return "mtools read different home/docs/plan.txt contents"
    if boot_count == 1 and long_name.stdout != LONG_NAME_CONTENTS:
        return "mtools read different 'My Plans.txt' contents"
    home_listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::home"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    listed = home_listing.stdout.upper()
    if boot_count == 2 and any(
        name in listed for name in ("NOTES", "DOCS", "MY PLANS")
    ):
        return "mtools still lists a file or directory the shell removed"
    return None


def SendQmpCommand(channel, command_id, execute, arguments=None):
    request = {"execute": execute, "id": command_id}
    if arguments is not None:
        request["arguments"] = arguments
    channel.write(json.dumps(request).encode() + b"\r\n")
    channel.flush()
    while True:
        response = json.loads(channel.readline())
        if response.get("id") == command_id:
            if "error" in response:
                raise RuntimeError(response["error"].get("desc", "QMP error"))
            return response


def SendQmpKeys(channel, first_id, keys):
    for offset, key in enumerate(keys):
        SendQmpCommand(
            channel,
            first_id + offset,
            "human-monitor-command",
            {"command-line": f"sendkey {key}"},
        )
    return first_id + len(keys)


def OpenQmpChannel(socket_path):
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5)
    connection.connect(socket_path)
    channel = connection.makefile("rwb")
    greeting = json.loads(channel.readline())
    if "QMP" not in greeting:
        raise RuntimeError("QEMU did not send a QMP greeting")
    SendQmpCommand(channel, 1, "qmp_capabilities")
    return connection, channel


def main():
    parser = argparse.ArgumentParser(
        description="Boot FeatherOS in QEMU and check serial startup output."
    )
    parser.add_argument(
        "--arch", choices=("x86_64", "aarch64"), default="x86_64"
    )
    parser.add_argument("--firmware", help="AArch64 UEFI firmware image")
    parser.add_argument("--disk", required=True, help="Raw virtio block image")
    parser.add_argument(
        "--cpu", help="QEMU CPU model; 'max' also requires its CPU protections"
    )
    parser.add_argument(
        "--expect-panic",
        action="store_true",
        help="Expect the intentional panic test",
    )
    parser.add_argument(
        "--expect-exception",
        action="store_true",
        help="Expect an intentional CPU exception",
    )
    parser.add_argument(
        "--expect-interrupt",
        action="store_true",
        help="Expect the architecture-specific hardware interrupt test",
    )
    parser.add_argument(
        "--expect-memory",
        action="store_true",
        help="Expect the physical and virtual memory tests to pass",
    )
    parser.add_argument(
        "--expect-timer",
        action="store_true",
        help="Expect the architecture-specific periodic timer test",
    )
    parser.add_argument(
        "--expect-scheduler",
        action="store_true",
        help="Expect cooperative task switching to pass",
    )
    parser.add_argument(
        "--expect-smp",
        action="store_true",
        help="Expect tasks to run concurrently on several CPUs",
    )
    parser.add_argument(
        "--expect-power",
        choices=("shutdown", "reboot"),
        help="Run the shell's shutdown or reboot and expect QEMU to exit or "
        "boot again",
    )
    parser.add_argument(
        "--expect-storage-boot",
        type=int,
        default=0,
        help="Expect a verified disk write that counts this many boots",
    )
    parser.add_argument(
        "--expect-user-preemption",
        action="store_true",
        help="Expect timer-driven user-task switching to pass",
    )
    parser.add_argument(
        "--expect-input",
        action="store_true",
        help="Send serial input and expect the kernel to accept a line",
    )
    parser.add_argument(
        "--expect-calc",
        action="store_true",
        help="Send a calculator command and expect its result",
    )
    parser.add_argument(
        "--expect-shell",
        action="store_true",
        help="Run shell commands and expect their output",
    )
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()
    if (
        sum(
            (
                args.expect_panic,
                args.expect_exception,
                args.expect_interrupt,
                args.expect_memory,
                args.expect_timer,
                args.expect_scheduler,
                args.expect_smp,
                args.expect_user_preemption,
                args.expect_input,
                args.expect_calc,
                args.expect_shell,
            )
        )
        > 1
    ):
        parser.error("select at most one expected result")

    qemu_name = (
        "qemu-system-aarch64"
        if args.arch == "aarch64"
        else "qemu-system-x86_64"
    )
    qemu = shutil.which(qemu_name)
    if qemu is None:
        print(f"ERROR: {qemu_name} was not found on PATH", file=sys.stderr)
        return 1

    command = [qemu]
    use_qmp_keyboard = args.expect_input
    qmp_directory = (
        tempfile.TemporaryDirectory(prefix="fq-") if use_qmp_keyboard else None
    )
    qmp_socket_path = (
        os.path.join(qmp_directory.name, "qmp.sock")
        if qmp_directory is not None
        else None
    )
    if args.arch == "aarch64":
        if not args.firmware:
            print("ERROR: --firmware is required for AArch64", file=sys.stderr)
            return 1
        command.extend(
            [
                "-machine",
                "virt",
                "-cpu",
                args.cpu or "cortex-a72",
                "-m",
                "2G",
                "-smp",
                str(QEMU_CPU_COUNT),
                "-global",
                "virtio-mmio.force-legacy=false",
                "-bios",
                args.firmware,
                "-device",
                "ramfb",
                "-device",
                "virtio-keyboard-device",
            ]
        )
    else:
        command.extend(
            ["-m", "2G", "-smp", str(QEMU_CPU_COUNT), "-boot", "order=d"]
        )
        if args.cpu:
            command.extend(["-cpu", args.cpu])
    command.extend(
        [
            "-drive",
            f"if=none,id=featheros_disk,file={args.disk},format=raw",
            "-device",
            "virtio-blk-device,drive=featheros_disk"
            if args.arch == "aarch64"
            else "virtio-blk-pci,drive=featheros_disk,disable-legacy=off",
        ]
    )
    command.extend(
        [
            "-display",
            "none",
            "-serial",
            "stdio",
            "-monitor",
            "none",
            *(() if args.expect_power == "reboot" else ("-no-reboot",)),
            *(() if args.expect_power == "shutdown" else ("-no-shutdown",)),
            "-cdrom",
            args.iso,
        ]
    )
    if qmp_socket_path is not None:
        command.extend(["-qmp", f"unix:{qmp_socket_path},server=on,wait=off"])

    uses_serial_input = (
        args.expect_calc
        or args.expect_shell
        or args.expect_storage_boot
        or args.expect_power
        or (args.expect_input and not use_qmp_keyboard)
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE if uses_serial_input else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    captured = bytearray()
    timeout = (
        SHELL_TIMEOUT_SECONDS
        if args.expect_shell or args.expect_storage_boot or args.expect_power
        else QEMU_TIMEOUT_SECONDS
    )
    deadline = time.monotonic() + timeout
    input_sent = False
    raw_keys_sent = False
    qmp_command_id = 2
    pending_lines = []
    if args.expect_calc:
        pending_lines = CALC_INPUT.splitlines(keepends=True)
    elif args.expect_shell:
        pending_lines = (
            SHELL_INPUT.splitlines(keepends=True)
            + list(SHELL_EDIT_INPUT)
            + list(SHELL_KEYS_INPUT)
            + list(SHELL_INTERRUPT_INPUT)
        )
    elif args.expect_storage_boot:
        pending_lines = STORAGE_SHELL_INPUT[
            args.expect_storage_boot
        ].splitlines(keepends=True)
    elif args.expect_power:
        pending_lines = [f"{args.expect_power}\n".encode()]
    lines_sent = 0
    markers_before_end = {}
    qmp_connection = None
    qmp_channel = None
    input_error = None
    if any(
        (
            args.expect_panic,
            args.expect_exception,
            args.expect_interrupt,
            args.expect_memory,
            args.expect_timer,
            args.expect_scheduler,
            args.expect_smp,
            args.expect_storage_boot,
            args.expect_power,
            args.expect_user_preemption,
            args.expect_input,
            args.expect_calc,
            args.expect_shell,
        )
    ):
        if args.expect_panic:
            markers = (EXPECTED_PANIC.encode(),)
        elif args.expect_exception:
            markers = (EXPECTED_EXCEPTION[args.arch].encode(),)
        elif args.expect_interrupt:
            markers = (EXPECTED_INTERRUPT[args.arch].encode(),)
        elif args.expect_memory:
            markers = (EXPECTED_MEMORY.encode(),)
        elif args.expect_scheduler:
            markers = (EXPECTED_SCHEDULER.encode(),)
        elif args.expect_smp:
            markers = (EXPECTED_SMP.encode(), PANIC_MARKER.encode())
        elif args.expect_power:
            # A shutdown ends when QEMU exits; a reboot when the shell is back.
            markers = (PANIC_MARKER.encode(),)
        elif args.expect_storage_boot:
            markers = (
                STORAGE_SHELL_OUTPUT[args.expect_storage_boot][-1].encode(),
                PANIC_MARKER.encode(),
            )
        elif args.expect_user_preemption:
            markers = (EXPECTED_USER_PREEMPTION.encode(),)
        elif args.expect_input:
            markers = (EXPECTED_INPUT_KEYS[-1].encode(), PANIC_MARKER.encode())
        elif args.expect_calc:
            markers = (EXPECTED_CALC.encode(), PANIC_MARKER.encode())
        elif args.expect_shell:
            markers = (
                EXPECTED_SHELL_OUTPUT[-1].encode(),
                PANIC_MARKER.encode(),
            )
        else:
            markers = (EXPECTED_TIMER.encode(),)
    else:
        markers = (SHELL_BANNER.encode(), PANIC_MARKER.encode())

    output_closed = False
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        readable, _, _ = select.select(
            [process.stdout], [], [], min(0.1, remaining)
        )
        if not readable:
            continue

        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            output_closed = True
            break
        captured.extend(chunk)
        clean_output = ANSI_ESCAPE_BYTES.sub(b"", captured)
        prompt_count = len(LINE_PROMPT.findall(clean_output))
        try:
            while pending_lines and lines_sent < prompt_count:
                line = pending_lines.pop(0)
                pieces = (
                    [line]
                    if isinstance(line, WholeWrite)
                    else [bytes((character,)) for character in line]
                )
                for piece in pieces:
                    process.stdin.write(piece)
                    process.stdin.flush()
                    time.sleep(0.02)
                lines_sent += 1
                markers_before_end = {
                    marker: clean_output.count(marker) for marker in markers
                }
                while pending_lines and pending_lines[0] is INTERRUPT_RUNNING:
                    pending_lines.pop(0)
                    time.sleep(INTERRUPT_DELAY_SECONDS)
                    process.stdin.write(CONTROL_C)
                    process.stdin.flush()
        except OSError as error:
            input_error = str(error)
            pending_lines = []
        try:
            if (
                args.expect_input
                and not input_sent
                and SHELL_PROMPT.search(clean_output)
            ):
                qmp_connection, qmp_channel = OpenQmpChannel(qmp_socket_path)
                qmp_command_id = SendQmpKeys(qmp_channel, 2, INPUT_LINE_KEYS)
                input_sent = True
            if (
                args.expect_input
                and not raw_keys_sent
                and KEYS_READY.encode() in clean_output
            ):
                SendQmpKeys(qmp_channel, qmp_command_id, INPUT_RAW_KEYS)
                raw_keys_sent = True
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            input_error = str(error)
            input_sent = True
            raw_keys_sent = True
        # The end marker may also appear earlier in a scripted session, so
        # only a new one after the last typed line ends it. Counting copies
        # holds even when stripping escape codes shifts earlier output.
        if not pending_lines and any(
            clean_output.count(marker) > markers_before_end.get(marker, 0)
            for marker in markers
        ):
            break
        if (
            args.expect_power == "reboot"
            and clean_output.count(SHELL_BANNER.encode()) >= 2
        ):
            break

    # QEMU closes its output as it exits, which can come before the
    # process can be reaped.
    exited_by_itself = output_closed or process.poll() is not None
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    captured.extend(process.stdout.read() or b"")
    if process.stdin is not None:
        process.stdin.close()
    if qmp_channel is not None:
        qmp_channel.close()
    if qmp_connection is not None:
        qmp_connection.close()
    if qmp_directory is not None:
        qmp_directory.cleanup()
    output = captured.decode("utf-8", errors="replace")

    output = ANSI_ESCAPE.sub("", output)
    serial_lines = [line for line in output.splitlines() if "KERN/" in line]
    if serial_lines:
        print("\n".join(serial_lines))
    if args.cpu == "max" and MAX_CPU_PROTECTIONS[args.arch] not in output:
        print(
            "FAIL: kernel did not enable the protections of QEMU's max CPU",
            file=sys.stderr,
        )
        return 1
    if args.expect_panic:
        if EXPECTED_PANIC not in output:
            print(
                "FAIL: expected panic record was not observed", file=sys.stderr
            )
            return 1
        print("PASS: expected panic was reported over serial")
        return 0
    if args.expect_exception:
        if EXPECTED_EXCEPTION[args.arch] not in output:
            print(
                "FAIL: expected CPU exception was not reported",
                file=sys.stderr,
            )
            return 1
        print("PASS: expected CPU exception was reported over serial")
        return 0
    if args.expect_interrupt:
        if EXPECTED_INTERRUPT[args.arch] not in output:
            print(
                "FAIL: expected hardware interrupt was not acknowledged",
                file=sys.stderr,
            )
            return 1
        print("PASS: hardware interrupt was acknowledged over serial")
        return 0
    if args.expect_memory:
        if EXPECTED_MEMORY not in output:
            print("FAIL: memory tests did not report success", file=sys.stderr)
            return 1
        print("PASS: memory tests reported success over serial")
        return 0
    if args.expect_timer:
        if EXPECTED_TIMER not in output:
            print(
                "FAIL: periodic timer test did not report success",
                file=sys.stderr,
            )
            return 1
        print("PASS: periodic timer test reported success over serial")
        return 0
    if args.expect_scheduler:
        if EXPECTED_SCHEDULER not in output:
            failure_message = "FAIL: scheduler preemption test did not pass"
            print(failure_message, file=sys.stderr)
            return 1
        print("PASS: timer preemption and task lifecycle passed")
        return 0
    if args.expect_power:
        expected = POWER_MESSAGES[args.expect_power]
        missing = [text for text in expected if text not in output]
        boots = output.count(
            "KERN/I: Limine transferred control to the kernel"
        )
        if missing or PANIC_MARKER in output:
            print(f"FAIL: power output is missing {missing}", file=sys.stderr)
            return 1
        if args.expect_power == "shutdown" and not exited_by_itself:
            print("FAIL: QEMU kept running after shutdown", file=sys.stderr)
            return 1
        if args.expect_power == "reboot" and boots < 2:
            print("FAIL: the system did not boot again", file=sys.stderr)
            return 1
        outcome = (
            "powered off"
            if args.expect_power == "shutdown"
            else "booted again"
        )
        print(
            f"PASS: {args.expect_power} stopped processes, flushed the disk, "
            f"and {outcome}"
        )
        return 0
    if args.expect_storage_boot:
        expected = f"{STORAGE_MESSAGE}{args.expect_storage_boot}"
        if (
            re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
            is None
        ):
            print(f"FAIL: expected '{expected}'", file=sys.stderr)
            return 1
        missing = [
            text
            for text in STORAGE_SHELL_OUTPUT[args.expect_storage_boot]
            if text not in output
        ]
        if missing or NOTES_OUTPUT.search(output) is None:
            print(
                "FAIL: file tools output is missing "
                f"{missing or 'the cat output'}",
                file=sys.stderr,
            )
            return 1
        disk_error = VerifyDiskImage(args.disk, args.expect_storage_boot)
        if disk_error is not None:
            print(f"FAIL: disk image check: {disk_error}", file=sys.stderr)
            return 1
        print(
            "PASS: sector, FAT16, and shell file-tool writes verified in the "
            f"guest and with mtools; boot {args.expect_storage_boot} persisted"
        )
        return 0
    if args.expect_smp:
        if EXPECTED_SMP not in output:
            print(
                "FAIL: multi-CPU scheduling test did not pass", file=sys.stderr
            )
            return 1
        print("PASS: tasks ran concurrently on several CPUs")
        return 0
    if args.expect_calc:
        if EXPECTED_CALC not in output:
            print(
                "FAIL: calculator command result was not observed",
                file=sys.stderr,
            )
            return 1
        if output.count(EXPECTED_UNKNOWN_COMMAND) != 1:
            print(
                "FAIL: expected exactly one unknown-command message",
                file=sys.stderr,
            )
            return 1
        print("PASS: unknown command rejected; calculator returned 69.25")
        return 0
    if args.expect_shell:
        # The shell exits with one of these when it cannot read a command.
        read_failure = re.search(
            r"^shell: (?:lost the console|could not read input).*$",
            output,
            re.MULTILINE,
        )
        if read_failure:
            print(f"FAIL: {read_failure.group(0).strip()}", file=sys.stderr)
            return 1
        missing = [
            text for text in EXPECTED_SHELL_OUTPUT if text not in output
        ]
        if missing:
            print(f"FAIL: shell output is missing {missing}", file=sys.stderr)
            return 1
        edits = {
            text: len(re.findall(rf"^{text}\r?$", output, re.MULTILINE))
            for text in EXPECTED_SHELL_EDITS
        }
        if edits != EXPECTED_SHELL_EDITS or not EXPECTED_HISTORY_LINE.search(
            output
        ):
            print(f"FAIL: line editing printed {edits}", file=sys.stderr)
            return 1
        if CLEAR_SEQUENCE not in captured:
            print("FAIL: clear did not clear the screen", file=sys.stderr)
            return 1
        keys = [
            line.strip()
            for line in output.splitlines()
            if line.startswith("keys: ")
        ]
        if keys != EXPECTED_KEYS:
            print(f"FAIL: keys reported {keys}", file=sys.stderr)
            return 1
        exhausted = [int(pages) for pages in EXHAUST_OUTPUT.findall(output)]
        if (
            len(exhausted) != 2
            or exhausted[0] != exhausted[1]
            or exhausted[0] < MINIMUM_EXHAUST_PAGES
        ):
            print(
                f"FAIL: page-limit exhaustion runs reported {exhausted}",
                file=sys.stderr,
            )
            return 1
        layouts = ASLR_OUTPUT.findall(output)
        if len(layouts) != 2 or any(
            first == second for first, second in zip(*layouts)
        ):
            print(
                f"FAIL: two aslr runs did not get distinct layouts: {layouts}",
                file=sys.stderr,
            )
            return 1
        busy = [
            int(percent)
            for percent in re.findall(
                r"^\s*\d+ R\s+(\d+)\.\d\s+\d+\s+\S+ primes\r?$",
                output,
                re.MULTILINE,
            )
        ]
        if not busy or max(busy) < 50:
            print(
                f"FAIL: top did not show a busy primes worker: {busy}",
                file=sys.stderr,
            )
            return 1
        # primes 4 and primes 16 report one line per worker.
        if len(PRIMES_WORKER_OUTPUT.findall(output)) != 20:
            print("FAIL: primes did not report every worker", file=sys.stderr)
            return 1
        # A prompt may precede the echo on the same line.
        interrupts = len(re.findall(r"\^C\r?$", output, re.MULTILINE))
        if (
            interrupts != EXPECTED_INTERRUPTS
            or "primes below 1000000000" in output
            or "slept 100000" in output
        ):
            print(
                f"FAIL: Ctrl+C was echoed {interrupts} times, expected "
                f"{EXPECTED_INTERRUPTS}, or an interrupted command finished",
                file=sys.stderr,
            )
            return 1
        if WORKING_DIRECTORY_OUTPUT.findall(output) != ["/home/work", "/home"]:
            print(
                "FAIL: pwd did not follow cd: "
                f"{WORKING_DIRECTORY_OUTPUT.findall(output)}",
                file=sys.stderr,
            )
            return 1
        if len(WORK_FILE_OUTPUT.findall(output)) != 2:
            print(
                "FAIL: a file was not readable through a relative path",
                file=sys.stderr,
            )
            return 1
        if UPTIME_OUTPUT.search(output) is None:
            print("FAIL: uptime did not print H:MM:SS", file=sys.stderr)
            return 1
        date_match = DATE_OUTPUT.search(output)
        if date_match is None:
            print("FAIL: date did not print a timestamp", file=sys.stderr)
            return 1
        guest_time = datetime.datetime.strptime(
            date_match.group(1), "%Y-%m-%d %H:%M:%S"
        ).replace(tzinfo=datetime.timezone.utc)
        skew = abs(
            datetime.datetime.now(datetime.timezone.utc) - guest_time
        ).total_seconds()
        if skew > MAXIMUM_CLOCK_SKEW_SECONDS:
            print(
                f"FAIL: date printed {date_match.group(1)} UTC, "
                f"{skew:.0f} seconds from the host clock",
                file=sys.stderr,
            )
            return 1
        print(
            "PASS: shell ran programs with arguments, foreground input, "
            f"uptime, Ctrl+C, and date ({date_match.group(1)} UTC)"
        )
        return 0
    if args.expect_user_preemption:
        if EXPECTED_USER_PREEMPTION not in output:
            print(
                "FAIL: user-mode timer preemption did not pass",
                file=sys.stderr,
            )
            return 1
        if USER_PREEMPTION_OUTPUTS[args.arch] not in captured:
            print(
                "FAIL: user-program preemption did not interleave",
                file=sys.stderr,
            )
            return 1
        print("PASS: user programs interleaved under timer preemption")
        return 0
    if args.expect_input:
        if input_error is not None:
            print(
                f"FAIL: could not inject QEMU keyboard input: {input_error}",
                file=sys.stderr,
            )
            return 1
        if "Unknown command: ab" not in output:
            print(
                "FAIL: the shell did not run the typed line", file=sys.stderr
            )
            return 1
        if len(re.findall(r"^azb\r?$", output, re.MULTILINE)) != 2:
            print(
                "FAIL: the cursor keys and history did not edit the line",
                file=sys.stderr,
            )
            return 1
        if INPUT_ECHO_SEQUENCE not in captured:
            print(
                "FAIL: terminal input echo/backspace sequence was incorrect",
                file=sys.stderr,
            )
            return 1
        if b"> ^C" not in captured:
            print("FAIL: Ctrl+C was not echoed at the prompt", file=sys.stderr)
            return 1
        keys = [
            line.strip()
            for line in output.splitlines()
            if line.startswith("keys: ")
        ]
        if keys != EXPECTED_INPUT_KEYS:
            print(f"FAIL: keys reported {keys}", file=sys.stderr)
            return 1
        print(
            "PASS: keyboard input, Ctrl+C, backspace, Enter, and raw special "
            "keys were handled"
        )
        return 0
    if PANIC_MARKER in output:
        print("FAIL: kernel panicked during startup", file=sys.stderr)
        return 1
    if USER_MODE_MESSAGES[args.arch] not in output:
        failure = f"FAIL: {args.arch} user-mode test did not complete"
        print(failure, file=sys.stderr)
        return 1
    if USER_MODE_OUTPUTS[args.arch] not in captured:
        print(
            "FAIL: user-mode console syscall output was not observed",
            file=sys.stderr,
        )
        return 1
    if READY_MESSAGE not in output:
        print(
            "FAIL: kernel did not report ready within "
            f"{QEMU_TIMEOUT_SECONDS}s",
            file=sys.stderr,
        )
        diagnostics = [
            line.strip()
            for line in output.splitlines()
            if "error:" in line.lower() or "error " in line.lower()
        ]
        for line in diagnostics[-8:]:
            print(line[:240], file=sys.stderr)
        return 1
    if TIMER_STARTED_MESSAGE not in output:
        print("FAIL: production timer did not start", file=sys.stderr)
        return 1
    if SMP_MESSAGE not in output:
        print(
            f"FAIL: kernel did not start all {QEMU_CPU_COUNT} CPUs",
            file=sys.stderr,
        )
        return 1
    serial_line_set = set(output.splitlines())
    missing_cpus = [
        cpu
        for cpu in range(1, QEMU_CPU_COUNT)
        if f"KERN/I: CPU {cpu} online" not in serial_line_set
    ]
    if missing_cpus:
        print(
            f"FAIL: CPUs {missing_cpus} did not report a complete online line",
            file=sys.stderr,
        )
        return 1
    if VFS_MESSAGE not in output:
        print(
            "FAIL: kernel did not read a file through the VFS", file=sys.stderr
        )
        return 1
    if FAT16_MESSAGE not in output:
        print(
            "FAIL: kernel did not read a file from the FAT16 partition",
            file=sys.stderr,
        )
        return 1
    if ELF_LOAD_MESSAGE not in output:
        print(
            "FAIL: kernel did not load the ELF into a user address space",
            file=sys.stderr,
        )
        return 1
    if SHELL_BANNER not in output:
        print("FAIL: the shell did not start", file=sys.stderr)
        return 1
    if EXPECTED_ARCHITECTURE[args.arch] not in output:
        print("FAIL: kernel did not report its architecture", file=sys.stderr)
        return 1
    if (
        re.search(
            r"KERN/I: Memory map coverage: \d+ MiB, usable RAM: \d+ MiB",
            output,
        )
        is None
    ):
        print(
            "FAIL: kernel did not report total and usable memory",
            file=sys.stderr,
        )
        return 1

    print("PASS: kernel reached the ready state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
