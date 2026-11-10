#!/usr/bin/env python3

import argparse
import os
import shutil
import sys

from smoke import qemu, session
from smoke.boot import MAX_CPU_PROTECTIONS
from smoke.cases import AddArguments, Select


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
        "--build-dir", required=True, help="The build's folder, build/<arch>"
    )
    parser.add_argument(
        "--session-output",
        help="Where the whole session is saved; "
        "smoke-output.txt in the build folder by default",
    )
    parser.add_argument(
        "--cpu", help="QEMU CPU model; 'max' also requires its CPU protections"
    )
    AddArguments(parser)
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()
    case = Select(parser, args)

    qemu_name = (
        "qemu-system-aarch64"
        if args.arch == "aarch64"
        else "qemu-system-x86_64"
    )
    qemu_path = shutil.which(qemu_name)
    if qemu_path is None:
        print(f"ERROR: {qemu_name} was not found on PATH", file=sys.stderr)
        return 1
    if args.arch == "aarch64" and not args.firmware:
        print("ERROR: --firmware is required for AArch64", file=sys.stderr)
        return 1

    try:
        case.Start()
        result = session.Run(
            qemu.Command(qemu_path, args, case),
            case,
            args.arch,
            args.session_output
            or os.path.join(args.build_dir, "smoke-output.txt"),
        )
    finally:
        case.Stop()

    serial_lines = [
        line for line in result.output.splitlines() if "KERN/" in line
    ]
    if serial_lines:
        print("\n".join(serial_lines))
    if (
        args.cpu == "max"
        and MAX_CPU_PROTECTIONS[args.arch] not in result.output
    ):
        print(
            "FAIL: kernel did not enable the protections of QEMU's max CPU",
            file=sys.stderr,
        )
        return 1
    failure = case.Check(result)
    if failure is not None:
        print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"PASS: {case.PassMessage(result)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
