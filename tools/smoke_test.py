#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import sys

READY_MESSAGE = "KERN/I: Kernel initialization complete"
PANIC_MARKER = "KERN/PANIC:"
QEMU_TIMEOUT_SECONDS = 10


def main():
    parser = argparse.ArgumentParser(
        description="Boot FeatherOS in QEMU and check serial startup output."
    )
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()

    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        print(
            "ERROR: qemu-system-x86_64 was not found on PATH", file=sys.stderr
        )
        return 1

    command = [
        qemu,
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

    print(output, end="" if output.endswith("\n") else "\n")
    if PANIC_MARKER in output:
        print("FAIL: kernel panicked during startup", file=sys.stderr)
        return 1
    if READY_MESSAGE not in output:
        print(
            "FAIL: kernel did not report ready within "
            f"{QEMU_TIMEOUT_SECONDS}s",
            file=sys.stderr,
        )
        return 1

    print("PASS: kernel reached the ready state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
