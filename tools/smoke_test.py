#!/usr/bin/env python3

import argparse
import re
import shutil
import subprocess
import sys

READY_MESSAGE = "KERN/I: Kernel initialization complete"
PANIC_MARKER = "KERN/PANIC:"
QEMU_TIMEOUT_SECONDS = 10
ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")


def main():
    parser = argparse.ArgumentParser(
        description="Boot FeatherOS in QEMU and check serial startup output."
    )
    parser.add_argument(
        "--arch", choices=("x86_64", "aarch64"), default="x86_64"
    )
    parser.add_argument("--firmware", help="AArch64 UEFI firmware image")
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()

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

    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=QEMU_TIMEOUT_SECONDS,
            check=False,
        )
        output = result.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")

    output = ANSI_ESCAPE.sub("", output)
    serial_lines = [line for line in output.splitlines() if "KERN/" in line]
    if serial_lines:
        print("\n".join(serial_lines))
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

    print("PASS: kernel reached the ready state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
