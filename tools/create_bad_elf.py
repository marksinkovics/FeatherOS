#!/usr/bin/env python3

import argparse
import struct
from pathlib import Path

# A well-formed executable whose only segment lies in the kernel half,
# so the kernel must refuse to load it.
MACHINES = {"x86_64": 62, "aarch64": 183}
KERNEL_HALF_ADDRESSES = {
    "x86_64": 0xFFFF800000400000,
    "aarch64": 0xFFFF000000400000,
}
HEADER_SIZE = 64
PROGRAM_HEADER_SIZE = 56
CODE_SIZE = 16


def main():
    parser = argparse.ArgumentParser(
        description="Create an ELF64 executable mapped in the kernel half."
    )
    parser.add_argument("output", help="Output ELF path")
    parser.add_argument("--arch", choices=sorted(MACHINES), required=True)
    args = parser.parse_args()

    base = KERNEL_HALF_ADDRESSES[args.arch]
    file_size = HEADER_SIZE + PROGRAM_HEADER_SIZE + CODE_SIZE
    entry = base + HEADER_SIZE + PROGRAM_HEADER_SIZE
    identity = b"\x7fELF" + bytes((2, 1, 1)) + bytes(9)
    header = identity + struct.pack(
        "<HHIQQQIHHHHHH",
        2,
        MACHINES[args.arch],
        1,
        entry,
        HEADER_SIZE,
        0,
        0,
        HEADER_SIZE,
        PROGRAM_HEADER_SIZE,
        1,
        0,
        0,
        0,
    )
    readable_executable = 4 | 1
    program_header = struct.pack(
        "<IIQQQQQQ",
        1,
        readable_executable,
        0,
        base,
        base,
        file_size,
        file_size,
        0x1000,
    )
    Path(args.output).write_bytes(header + program_header + bytes(CODE_SIZE))


if __name__ == "__main__":
    main()
