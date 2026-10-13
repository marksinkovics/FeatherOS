#!/usr/bin/env python3

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import time

READY_MESSAGE = "KERN/I: Kernel initialization complete"
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
QEMU_TIMEOUT_SECONDS = 10
ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
ANSI_ESCAPE_BYTES = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")


def main():
    parser = argparse.ArgumentParser(
        description="Boot FeatherOS in QEMU and check serial startup output."
    )
    parser.add_argument(
        "--arch", choices=("x86_64", "aarch64"), default="x86_64"
    )
    parser.add_argument("--firmware", help="AArch64 UEFI firmware image")
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
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()
    if (
        sum(
            (
                args.expect_panic,
                args.expect_exception,
                args.expect_interrupt,
                args.expect_memory,
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
                "-bios",
                args.firmware,
                "-device",
                "ramfb",
            ]
        )
    else:
        command.extend(["-m", "2G"])
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

    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0
    )
    captured = bytearray()
    deadline = time.monotonic() + QEMU_TIMEOUT_SECONDS
    markers = (
        READY_MESSAGE.encode(),
        PANIC_MARKER.encode(),
        EXPECTED_PANIC.encode(),
        EXPECTED_EXCEPTION[args.arch].encode(),
        EXPECTED_INTERRUPT[args.arch].encode(),
        EXPECTED_MEMORY.encode(),
    )

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
    if PANIC_MARKER in output:
        print("FAIL: kernel panicked during startup", file=sys.stderr)
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
