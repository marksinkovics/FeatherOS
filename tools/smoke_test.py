#!/usr/bin/env python3

import argparse
import datetime
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

READY_MESSAGE = "KERN/I: Kernel initialization complete"
TIMER_STARTED_MESSAGE = "KERN/I: Timer started at 100 Hz"
UPTIME_MESSAGE = "KERN/I: Uptime:"
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
EXPECTED_ARCHITECTURE = {
    "x86_64": "KERN/I: Architecture: x86_64",
    "aarch64": "KERN/I: Architecture: aarch64",
}
EXPECTED_MEMORY = "KERN/MEMORY: tests passed"
EXPECTED_TIMER = "KERN/TIMER: periodic ticks passed"
EXPECTED_CALC = "calc: 2 + 3 = 5"
EXPECTED_UNKNOWN_COMMAND = "Unknown command: nope"
CALC_INPUT = b"nope\n\ncalc 2 3\n"
# Covers argv, foreground input in a child, and nonzero exit statuses.
SHELL_INPUT = (
    b"echo hello   feather os\nupper\nmixed Case\ndate\nfanout 6\n"
    b"memtest\nmemtest overflow\ncalc 1\n"
)
# `date` prints only the timestamp, unlike the boot log's "Wall clock:" line.
DATE_OUTPUT = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\r?$", re.MULTILINE
)
# QEMU's RTC follows the host clock; allow for boot time and slow hosts.
MAXIMUM_CLOCK_SKEW_SECONDS = 120
EXPECTED_SHELL_OUTPUT = (
    "hello feather os",
    "upper> ",
    "MIXED CASE",
    "fanout: 6 children exited",
    "memtest: stack and heap ok",
    # memtest overflow ends at the stack guard page with ExitStatusFault.
    "shell: program terminated by a CPU exception",
    "usage: calc <number> <number>",
    "exit status 1",
)
EXPECTED_SCHEDULER = (
    "KERN/SCHEDULER: preemption, wait, exit, reap, blocking, and sleep passed"
)
EXPECTED_USER_PREEMPTION = "KERN/SCHEDULER: user-mode preemption passed"
EXPECTED_SMP = (
    "KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact"
)
USER_PREEMPTION_OUTPUTS = {"x86_64": b"UIu", "aarch64": b"AIa"}
INPUT_ACCEPTED_MESSAGE = "KERN/I: Input line accepted"
INPUT_PROMPT = b"FeatherOS> "
INPUT_ECHO_SEQUENCE = b"abx\x08 \x08\r\n"
QEMU_TIMEOUT_SECONDS = 10
QEMU_CPU_COUNT = 4
SMP_MESSAGE = f"KERN/I: SMP: {QEMU_CPU_COUNT} CPUs online with IPIs and timers"
ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
ANSI_ESCAPE_BYTES = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")


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
                "cortex-a72",
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
            "-no-reboot",
            "-no-shutdown",
            "-cdrom",
            args.iso,
        ]
    )
    if qmp_socket_path is not None:
        command.extend(["-qmp", f"unix:{qmp_socket_path},server=on,wait=off"])

    uses_serial_input = (
        args.expect_calc
        or args.expect_shell
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
    deadline = time.monotonic() + QEMU_TIMEOUT_SECONDS
    input_sent = False
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
        markers = (UPTIME_MESSAGE.encode(), PANIC_MARKER.encode())

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
            break
        captured.extend(chunk)
        clean_output = ANSI_ESCAPE_BYTES.sub(b"", captured)
        if (
            (args.expect_input or args.expect_calc or args.expect_shell)
            and not input_sent
            and INPUT_PROMPT in clean_output
        ):
            try:
                if use_qmp_keyboard:
                    qmp_connection, qmp_channel = OpenQmpChannel(
                        qmp_socket_path
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
                    if args.expect_calc:
                        input_sequence = CALC_INPUT
                    elif args.expect_shell:
                        input_sequence = SHELL_INPUT
                    else:
                        input_sequence = b"abx\x7f\n"
                    for character in input_sequence:
                        process.stdin.write(bytes((character,)))
                        process.stdin.flush()
                        time.sleep(0.02)
            except (OSError, RuntimeError, json.JSONDecodeError) as error:
                input_error = str(error)
            input_sent = True
        if any(marker in clean_output for marker in markers):
            break

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
        print("PASS: unknown command rejected; calculator command returned 5")
        return 0
    if args.expect_shell:
        missing = [
            text for text in EXPECTED_SHELL_OUTPUT if text not in output
        ]
        if missing:
            print(f"FAIL: shell output is missing {missing}", file=sys.stderr)
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
            f"and date ({date_match.group(1)} UTC)"
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
        print("PASS: keyboard/serial input, backspace, and Enter were handled")
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
    if re.search(r"KERN/I: Uptime: \d+ seconds", output) is None:
        print("FAIL: kernel did not report uptime", file=sys.stderr)
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
