#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# The firmware and Limine clear the screen on every boot; those codes would
# also clear the terminal and anyone viewing the log.
# A two-byte escape never uses '[', which starts the longer CSI form, so a bare
# "ESC [" at the end of a read counts as incomplete.
ANSI_ESCAPE = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-Z\\\]^_c])")
LONGEST_ESCAPE = 32
QEMU_CPU_COUNT = 4


def main():
    parser = argparse.ArgumentParser(
        description="Run FeatherOS in QEMU with a saved serial log."
    )
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--firmware", help="AArch64 UEFI firmware image")
    parser.add_argument("--disk", required=True, help="Raw virtio block image")
    parser.add_argument(
        "--log-file", required=True, help="Path for raw serial output"
    )
    parser.add_argument(
        "--no-reboot",
        action="store_true",
        help="Exit instead of resetting, to catch a triple fault",
    )
    parser.add_argument("iso", help="Path to the bootable ISO")
    args = parser.parse_args()

    if args.arch == "aarch64":
        qemu_name = "qemu-system-aarch64"
    else:
        qemu_name = "qemu-system-x86_64"
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
            "-serial",
            "stdio",
            "-monitor",
            "none",
            # The shell's reboot and shutdown should restart or close QEMU.
            *(("-no-reboot",) if args.no_reboot else ()),
            "-cdrom",
            args.iso,
        ]
    )

    log_path = Path(args.log_file)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("wb") as log_file:
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        try:
            # Read whatever has arrived, so prompts show before a newline.
            pending = b""
            while True:
                output_chunk = os.read(process.stdout.fileno(), 4096)
                data = pending + output_chunk
                ready_length = len(data)
                # Hold back an escape sequence split across reads.
                escape = data.rfind(
                    b"\x1b", max(0, len(data) - LONGEST_ESCAPE)
                )
                if (
                    output_chunk
                    and escape != -1
                    and ANSI_ESCAPE.match(data, escape) is None
                ):
                    ready_length = escape
                pending = data[ready_length:]
                text = ANSI_ESCAPE.sub(b"", data[:ready_length])
                log_file.write(text)
                log_file.flush()
                sys.stdout.buffer.write(text)
                sys.stdout.buffer.flush()
                if not output_chunk:
                    break
            return_code = process.wait()
        except KeyboardInterrupt:
            process.terminate()
            process.wait()
            return 130

    return return_code


if __name__ == "__main__":
    sys.exit(main())
