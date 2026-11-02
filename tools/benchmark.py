#!/usr/bin/env python3

import argparse
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import time

from smoke_test import ANSI_ESCAPE_BYTES, QEMU_CPU_COUNT, SHELL_PROMPT

# The baseline first; each later program's times are given as a ratio to it.
PROGRAMS = (
    ("cppbench", "C++ -Os"),
    ("swiftbench", "Swift -Osize"),
    ("cppbench-O0", "C++ -O0"),
)
RESULT = re.compile(
    rb"^(\w+): check (\d+), (\d+) runs? in (\d+) ms, (\d+) us each\r?$",
    re.MULTILINE,
)
WORKLOADS = 5
BOOT_TIMEOUT_SECONDS = 120
PT_LOAD = 1
PF_X = 1


def QemuCommand(args):
    if args.arch == "aarch64":
        command = [
            "qemu-system-aarch64",
            "-machine",
            "virt",
            "-cpu",
            "cortex-a72",
            "-global",
            "virtio-mmio.force-legacy=false",
            "-bios",
            args.firmware,
            "-device",
            "ramfb",
        ]
        block_device = "virtio-blk-device,drive=featheros_disk"
    else:
        command = ["qemu-system-x86_64", "-boot", "order=d"]
        block_device = "virtio-blk-pci,drive=featheros_disk,disable-legacy=off"
    return command + [
        "-m",
        "2G",
        "-smp",
        str(QEMU_CPU_COUNT),
        "-drive",
        f"if=none,id=featheros_disk,file={args.disk},format=raw",
        "-device",
        block_device,
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        "none",
        "-no-reboot",
        "-cdrom",
        args.iso,
    ]


class Session:
    def __init__(self, process):
        self.process = process
        self.captured = bytearray()

    def Clean(self):
        return ANSI_ESCAPE_BYTES.sub(b"", self.captured)

    # The output after start once a new shell prompt follows it.
    def WaitForPrompt(self, start, timeout):
        deadline = time.monotonic() + timeout
        while True:
            clean = self.Clean()
            prompt = SHELL_PROMPT.search(clean, start)
            if prompt is not None:
                return clean[start : prompt.start()], prompt.end()
            remaining = deadline - time.monotonic()
            if remaining <= 0 or self.process.poll() is not None:
                raise TimeoutError("no shell prompt")
            readable, _, _ = select.select(
                [self.process.stdout], [], [], min(0.1, remaining)
            )
            if readable:
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if not chunk:
                    raise TimeoutError("QEMU closed its output")
                self.captured.extend(chunk)

    # Typed a byte at a time, as the smoke test does, so the input queue
    # never overflows.
    def Run(self, line, after, timeout):
        for character in f"{line}\n".encode():
            self.process.stdin.write(bytes((character,)))
            self.process.stdin.flush()
            time.sleep(0.02)
        return self.WaitForPrompt(after, timeout)


def ParseResults(output, results):
    for match in RESULT.finditer(output):
        results.setdefault(match[1].decode(), []).append(
            (int(match[2]), int(match[5]))
        )


# Loaded bytes of an ELF64 file: executable segments, then the others with
# their zero-filled parts.
def SegmentSizes(path):
    with open(path, "rb") as file:
        data = file.read()
    (header_offset,) = struct.unpack_from("<Q", data, 0x20)
    entry_size, count = struct.unpack_from("<HH", data, 0x36)
    code = 0
    other = 0
    for index in range(count):
        kind, flags, _, _, _, file_size, memory_size, _ = struct.unpack_from(
            "<IIQQQQQQ", data, header_offset + index * entry_size
        )
        if kind != PT_LOAD:
            continue
        if flags & PF_X:
            code += file_size
        else:
            other += memory_size
    return code, other


# Each workload's fastest run over the rounds, since QEMU's timing varies
# with the host's load.
def Report(arch, seconds, rounds, results, programs):
    print(
        f"FeatherOS benchmark on {arch} in QEMU, {seconds} s a workload, "
        f"best time per run of {rounds} rounds"
    )
    header = f"{'workload':<10} {'check':>10}"
    for index, (_, label) in enumerate(programs):
        header += f" {label:>13}"
        if index > 0:
            header += f" {'ratio':>6}"
    print(header)
    mismatched = False
    baseline = results[programs[0][0]]
    for workload, baseline_runs in baseline.items():
        check = baseline_runs[0][0]
        fastest = min(taken for _, taken in baseline_runs)
        row = f"{workload:<10} {check:>10}"
        for index, (program, _) in enumerate(programs):
            runs = results[program].get(workload)
            if runs is None or len(runs) < rounds:
                row += f" {'missing':>13}"
                mismatched = True
                continue
            taken = min(taken for _, taken in runs)
            row += f" {taken:>10} us"
            if index > 0:
                row += f" {taken / fastest:>5.2f}x"
            wrong = [found for found, _ in runs if found != check]
            if wrong:
                row += f" (check {wrong[0]})"
                mismatched = True
        print(row)
    print("\nLoaded bytes: code, then data")
    for program, label in programs:
        code, other = SegmentSizes(
            os.path.join(ProgramsDirectory(arch), f"{program}-{arch}.elf")
        )
        print(f"{label:<13} {code:>8} {other:>8}")
    if mismatched:
        print("ERROR: the programs' check values differ", file=sys.stderr)
    return 1 if mismatched else 0


def ProgramsDirectory(arch):
    return os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "build",
        arch,
        "programs",
    )


def main():
    parser = argparse.ArgumentParser(
        description="Boot FeatherOS in QEMU and compare cppbench with "
        "swiftbench."
    )
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--firmware", help="AArch64 UEFI firmware image")
    parser.add_argument("--disk", required=True, help="Raw virtio block image")
    parser.add_argument(
        "--seconds", type=int, default=3, help="Time for each workload"
    )
    parser.add_argument(
        "--rounds", type=int, default=3, help="Runs of every program"
    )
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()
    if args.arch == "aarch64" and not args.firmware:
        parser.error("--firmware is required for AArch64")
    command = QemuCommand(args)
    if shutil.which(command[0]) is None:
        print(f"ERROR: {command[0]} was not found on PATH", file=sys.stderr)
        return 1

    directory = ProgramsDirectory(args.arch)
    programs = [
        program
        for program in PROGRAMS
        if os.path.exists(
            os.path.join(directory, f"{program[0]}-{args.arch}.elf")
        )
    ]
    if len(programs) < len(PROGRAMS):
        print("NOTE: Swift programs were not built; skipping swiftbench")

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    session = Session(process)
    results = {program: {} for program, _ in programs}
    try:
        _, position = session.WaitForPrompt(0, BOOT_TIMEOUT_SECONDS)
        # Interleaved, so a slow stretch of the host affects every program.
        for _ in range(args.rounds):
            for program, _ in programs:
                output, position = session.Run(
                    f"{program} -t {args.seconds}",
                    position,
                    BOOT_TIMEOUT_SECONDS + 4 * WORKLOADS * args.seconds,
                )
                ParseResults(output, results[program])
    except TimeoutError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    finally:
        process.kill()
        process.wait()
        log = os.path.join(os.path.dirname(directory), "benchmark-output.txt")
        with open(log, "wb") as file:
            file.write(session.captured)
    return Report(args.arch, args.seconds, args.rounds, results, programs)


if __name__ == "__main__":
    sys.exit(main())
