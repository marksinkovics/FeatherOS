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
    b"date --help\nprimes -h\nfiletest\n"
    b"mkdir work\ncd work\nwrite a.txt in work\npwd\ncat a.txt\ncd ..\n"
    b"cat work/a.txt\ncd /\nls\ncat hello.txt\ncd nowhere\ncd\npwd\n"
    b"exit\n"
    b"memtest\nmemtest overflow\nmemtest exhaust\nmemtest exhaust\n"
    b"sleep 1100\nhardening\naslr\naslr\nprimes 4 100000\n"
    b"calc\n1 / 3\n7 / 0\nexit\n"
)
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
    b"sleep 100000\n",
    INTERRUPT_RUNNING,
    b"upper\n",
    CONTROL_C,
    b"calc\n",
    CONTROL_C,
    CONTROL_C,
    b"calc 1 +\n",
)
EXPECTED_INTERRUPTS = 5
# `date` prints only the timestamp, unlike the boot log's "Wall clock:" line.
DATE_OUTPUT = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\r?$", re.MULTILINE
)
# pwd prints the directory alone; prompts follow it with "> ".
WORKING_DIRECTORY_OUTPUT = re.compile(r"^(/disk(?:/work)?)\r?$", re.MULTILINE)
WORK_FILE_OUTPUT = re.compile(r"^in work\r?$", re.MULTILINE)
UPTIME_OUTPUT = re.compile(r"^uptime: \d+:\d{2}:\d{2}\r?$", re.MULTILINE)
PRIMES_WORKER_OUTPUT = re.compile(
    r"^primes: worker \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)
EXHAUST_OUTPUT = re.compile(
    r"^memtest: page limit reached after (\d+) heap pages\r?$", re.MULTILINE
)
# The 1024-page limit also covers the image, stack, and arguments pages.
MINIMUM_EXHAUST_PAGES = 1000
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
    "disk/",
    "FeatherOS initramfs is ready.",
    "cd: nowhere: no such directory",
    "hello feather os",
    "upper> ",
    "MIXED CASE",
    "fanout: 6 children exited",
    "memtest: stack and heap ok",
    # memtest overflow ends at the stack guard page with ExitStatusFault.
    "shell: program terminated by a CPU exception",
    "sleep: slept 1100 ms",
    "hardening: 45 checks passed",
    "primes: 9592 primes below 100000 with 4 workers in ",
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
    b'cat "read me first.txt"\nwrite "My Plans.txt" long name test\n'
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
# The disk also holds Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
        "plan.txt",
        "notes.txt",
        "My Plans.txt",
        "docs/",
        "1 directory, 5 files, ",
    ),
    2: (
        "cat: cannot open notes.txt",
        "rm: notes.txt: no such file",
        "\nnested hello",
        "rmdir: docs: not an empty directory",
        "\nlong name test",
        "ls: docs: no such directory",
        "3 files, ",
    ),
}
USER_PREEMPTION_OUTPUTS = {"x86_64": b"UIu", "aarch64": b"AIa"}
INPUT_ACCEPTED_MESSAGE = "KERN/I: Input line accepted"
# The shell's prompt shows its working directory, such as "/disk> ".
SHELL_PROMPT = re.compile(rb"^/[^\n>]*> ", re.MULTILINE)
# Each scripted shell line is typed only once a prompt asks for it, so
# typed-ahead input never overflows the kernel's input queue.
LINE_PROMPT = re.compile(rb"^(?:upper|calc|/[^\n>]*)?> ", re.MULTILINE)
INPUT_ECHO_SEQUENCE = b"abx\x08 \x08\r\n"
QEMU_TIMEOUT_SECONDS = 10
# The scripted shell session runs many programs; AArch64 emulation is slower.
SHELL_TIMEOUT_SECONDS = 30
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
        ["mtype", "-i", image, "::NOTES.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    plan = subprocess.run(
        ["mtype", "-i", image, "::DOCS/PLAN.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and notes.stdout != NOTES_CONTENTS:
        return "mtools read different NOTES.TXT contents"
    long_name = subprocess.run(
        ["mtype", "-i", image, "::My Plans.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and plan.stdout != PLAN_CONTENTS:
        return "mtools read different DOCS/PLAN.TXT contents"
    if boot_count == 1 and long_name.stdout != LONG_NAME_CONTENTS:
        return "mtools read different 'My Plans.txt' contents"
    listed = listing.stdout.upper()
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
    pending_lines = []
    if args.expect_calc:
        pending_lines = CALC_INPUT.splitlines(keepends=True)
    elif args.expect_shell:
        pending_lines = SHELL_INPUT.splitlines(keepends=True) + list(
            SHELL_INTERRUPT_INPUT
        )
    elif args.expect_storage_boot:
        pending_lines = STORAGE_SHELL_INPUT[
            args.expect_storage_boot
        ].splitlines(keepends=True)
    elif args.expect_power:
        pending_lines = [f"{args.expect_power}\n".encode()]
    lines_sent = 0
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
            markers = (INPUT_ACCEPTED_MESSAGE.encode(), PANIC_MARKER.encode())
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
                for character in pending_lines.pop(0):
                    process.stdin.write(bytes((character,)))
                    process.stdin.flush()
                    time.sleep(0.02)
                lines_sent += 1
                while pending_lines and pending_lines[0] is INTERRUPT_RUNNING:
                    pending_lines.pop(0)
                    time.sleep(INTERRUPT_DELAY_SECONDS)
                    process.stdin.write(CONTROL_C)
                    process.stdin.flush()
        except OSError as error:
            input_error = str(error)
            pending_lines = []
        if (
            args.expect_input
            and not input_sent
            and SHELL_PROMPT.search(clean_output)
        ):
            try:
                if use_qmp_keyboard:
                    qmp_connection, qmp_channel = OpenQmpChannel(
                        qmp_socket_path
                    )
                    # Ctrl+C at the shell prompt only discards the line.
                    SendQmpCommand(
                        qmp_channel,
                        7,
                        "human-monitor-command",
                        {"command-line": "sendkey ctrl-c"},
                    )
                    SendQmpCommand(
                        qmp_channel,
                        2,
                        "human-monitor-command",
                        {"command-line": "sendkey a"},
                    )
                    SendQmpCommand(
                        qmp_channel,
                        3,
                        "human-monitor-command",
                        {"command-line": "sendkey b"},
                    )
                    SendQmpCommand(
                        qmp_channel,
                        4,
                        "human-monitor-command",
                        {"command-line": "sendkey x"},
                    )
                    SendQmpCommand(
                        qmp_channel,
                        5,
                        "human-monitor-command",
                        {"command-line": "sendkey backspace"},
                    )
                    SendQmpCommand(
                        qmp_channel,
                        6,
                        "human-monitor-command",
                        {"command-line": "sendkey ret"},
                    )
                else:
                    for character in b"\x03abx\x7f\n":
                        process.stdin.write(bytes((character,)))
                        process.stdin.flush()
                        time.sleep(0.02)
            except (OSError, RuntimeError, json.JSONDecodeError) as error:
                input_error = str(error)
            input_sent = True
        if any(marker in clean_output for marker in markers):
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
        missing = [
            text for text in EXPECTED_SHELL_OUTPUT if text not in output
        ]
        if missing:
            print(f"FAIL: shell output is missing {missing}", file=sys.stderr)
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
        if len(PRIMES_WORKER_OUTPUT.findall(output)) != 4:
            print("FAIL: primes did not report four workers", file=sys.stderr)
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
        if WORKING_DIRECTORY_OUTPUT.findall(output) != ["/disk/work", "/disk"]:
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
        if INPUT_ACCEPTED_MESSAGE not in output:
            print(
                "FAIL: kernel did not accept the injected input line",
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
        print(
            "PASS: keyboard/serial input, Ctrl+C, backspace, and Enter were "
            "handled"
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
