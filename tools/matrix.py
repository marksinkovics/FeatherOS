#!/usr/bin/env python3
"""Runs the QEMU matrix: builds each architecture and build type once, then
boots the cases side by side, each with its own ISO, disk and session under
build/matrix/<case>/."""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_SCRIPT = os.path.join(REPO_ROOT, "build.sh")
MATRIX_DIR = os.path.join(REPO_ROOT, "build", "matrix")
TOGGLES = (
    "PANIC_TEST",
    "EXCEPTION_TEST",
    "INTERRUPT_TEST",
    "MEMORY_TEST",
    "TIMER_TEST",
    "SCHEDULER_TEST",
    "USER_PREEMPTION_TEST",
    "SMP_TEST",
    "STORAGE_TEST",
    "NETWORK_TEST",
    "CALC_TEST",
    "SHELL_TEST",
    "INPUT_TEST",
    "ROOT_TEST",
    "ROOT_DISK_TEST",
    "GRAPHICS_TEST",
    "DUNGEON_TEST",
    "FONTS_TEST",
    "SCREEN_SIZE_TEST",
    "PCI_TEST",
    "USB_TEST",
    "USB_KEYS_TEST",
    "USB_FALLBACK_TEST",
    "USB_HUB_TEST",
    "USB_TABLET_TEST",
    "USB_STORAGE_TEST",
    "USB_STORAGE_HOTPLUG_TEST",
    "USB_STORAGE_LARGE_TEST",
    "USB_STICK_TEST",
    "NVME_TEST",
    "NVME_NAMESPACES_TEST",
    "AHCI_TEST",
    "STORAGE_POLICY_TEST",
    "NODISK_TEST",
    "X86_UEFI",
)
# Rough minutes each kind of case takes, so the longest start first.
WEIGHTS = {
    "SHELL_TEST": 3,
    "NETWORK_TEST": 3,
    "STORAGE_TEST": 2,
    "POWER_TEST": 1,
    "DUNGEON_TEST": 2,
    "FONTS_TEST": 1,
    "SCREEN_SIZE_TEST": 1,
}


class Case:
    def __init__(self, name, arch, build_type="Debug", **settings):
        self.name = name
        self.arch = arch
        self.build_type = build_type
        self.settings = settings

    def Weight(self):
        return max(
            (WEIGHTS.get(key, 0) for key in self.settings), default=0
        ) + (1 if self.build_type == "Release" else 0)


def Both(number, name, **settings):
    return [
        Case(f"{number:02d}_x86_64_{name}", "x86_64", **settings),
        Case(f"{number + 1:02d}_aarch64_{name}", "aarch64", **settings),
    ]


def Each(number, x86_64_name, aarch64_name, **settings):
    return [
        Case(f"{number:02d}_x86_64_{x86_64_name}", "x86_64", **settings),
        Case(
            f"{number + 1:02d}_aarch64_{aarch64_name}", "aarch64", **settings
        ),
    ]


ON = "ON"
CASES = [
    *Each(1, "panic_reporting", "panic_reporting", PANIC_TEST=ON),
    *Each(
        3,
        "divide_error_reporting",
        "undefined_instruction_reporting",
        EXCEPTION_TEST=ON,
    ),
    Case("05_x86_64_normal_boot", "x86_64"),
    Case("06_x86_64_pit_interrupt", "x86_64", INTERRUPT_TEST=ON),
    Case("07_aarch64_normal_boot", "aarch64"),
    Case("08_aarch64_gic_interrupt", "aarch64", INTERRUPT_TEST=ON),
    *Both(9, "memory", MEMORY_TEST=ON),
    *Both(11, "timer", TIMER_TEST=ON),
    *Both(13, "input", INPUT_TEST=ON),
    *Both(15, "cooperative_scheduler", SCHEDULER_TEST=ON),
    *Both(17, "user_mode_preemption", USER_PREEMPTION_TEST=ON),
    *Both(19, "calculator_command", CALC_TEST=ON),
    *Both(21, "shell_programs", SHELL_TEST=ON),
    *Both(23, "smp_scheduling", SMP_TEST=ON),
    # QEMU's max CPU has SMEP/SMAP/RDRAND or PAN/RNDR, so user programs run
    # under them.
    *Both(25, "shell_with_cpu_protections", SHELL_TEST=ON, QEMU_CPU="max"),
    *Both(27, "storage_persistence", STORAGE_TEST=ON),
    *Both(29, "storage_on_gpt_disk", STORAGE_TEST=ON, DISK_LAYOUT="gpt"),
    *Both(31, "shutdown", POWER_TEST="shutdown"),
    *Both(33, "reboot", POWER_TEST="reboot"),
    *Both(35, "network", NETWORK_TEST=ON),
    # Optimization can expose what -O0 hides, such as a missing volatile.
    *Both(37, "shell_release", SHELL_TEST=ON, build_type="Release"),
    *Both(39, "ext3_root", ROOT_TEST=ON, KERNEL_ROOT="vda3"),
    *Both(41, "unusable_root", ROOT_TEST=ON, KERNEL_ROOT="vda9"),
    *Both(43, "ext2_root", ROOT_TEST=ON, KERNEL_ROOT="vda6"),
    *Both(45, "empty_ext4_root", ROOT_TEST=ON, KERNEL_ROOT="vda1"),
    *Both(47, "graphics", GRAPHICS_TEST=ON),
    *Both(49, "dungeon", DUNGEON_TEST=ON),
    *Both(51, "fonts", FONTS_TEST=ON),
    *Both(53, "pci", PCI_TEST=ON),
    *Both(55, "usb", USB_TEST=ON),
    *Both(57, "usb_keys", USB_KEYS_TEST=ON),
    *Both(59, "usb_fallback", USB_FALLBACK_TEST=ON),
    *Both(61, "usb_hub", USB_HUB_TEST=ON),
    # OVMF instead of SeaBIOS; skipped when no edk2 x86-64 firmware is found.
    Case("63_x86_64_usb_uefi", "x86_64", USB_TEST=ON, X86_UEFI=ON),
    *Both(64, "nodisk", NODISK_TEST=ON),
    *Both(66, "usb_storage", USB_STORAGE_TEST=ON),
    *Both(68, "usb_storage_hotplug", USB_STORAGE_HOTPLUG_TEST=ON),
    *Both(70, "usb_storage_large", USB_STORAGE_LARGE_TEST=ON),
    # The make_usb_stick.sh image booted from a USB drive; skipped like 63.
    Case("72_x86_64_usb_stick_uefi", "x86_64", USB_STICK_TEST=ON, X86_UEFI=ON),
    *Both(74, "nvme", NVME_TEST=ON),
    *Both(76, "nvme_namespaces", NVME_NAMESPACES_TEST=ON),
    *Both(78, "nvme_without_usb", NVME_TEST=ON, KERNEL_USB="off"),
    *Both(80, "ahci", AHCI_TEST=ON),
    *Both(82, "storage_policy", STORAGE_POLICY_TEST=ON, KERNEL_STORAGE="ro"),
    *Both(84, "nvme_release", NVME_TEST=ON, build_type="Release"),
    # The root on NVMe and AHCI.
    *Both(
        86,
        "nvme_root",
        ROOT_DISK_TEST=ON,
        DISK_BUS="nvme",
        KERNEL_ROOT="nvme0n1p3",
    ),
    *Both(
        88, "ahci_root", ROOT_DISK_TEST=ON, DISK_BUS="ahci", KERNEL_ROOT="sda1"
    ),
    *Both(
        90,
        "nvme_4k_root",
        ROOT_DISK_TEST=ON,
        DISK_BUS="nvme4k",
        KERNEL_ROOT="nvme0n1p1",
    ),
    *Both(
        92,
        "nvme_root_readonly",
        ROOT_DISK_TEST=ON,
        DISK_BUS="nvme",
        KERNEL_ROOT="nvme0n1p1",
        KERNEL_STORAGE="ro",
    ),
    *Both(94, "nvme_no_default_root", ROOT_DISK_TEST=ON, DISK_BUS="nvme"),
    *Both(96, "missing_root_disk", ROOT_DISK_TEST=ON, KERNEL_ROOT="nvme0n1p3"),
    *Both(
        98,
        "nvme_unusable_root",
        ROOT_DISK_TEST=ON,
        DISK_BUS="nvme",
        KERNEL_ROOT="nvme0n1p9",
    ),
    # A large screen; AArch64's ramfb firmware lists nothing above 1024x768,
    # so its case takes the 800x600 mode, below the default.
    Case(
        "100_x86_64_screen_size",
        "x86_64",
        SCREEN_SIZE_TEST=ON,
        SCREEN="1920x1080",
    ),
    Case(
        "101_aarch64_screen_size",
        "aarch64",
        SCREEN_SIZE_TEST=ON,
        SCREEN="800x600",
    ),
    Case("102_x86_64_usb_tablet", "x86_64", USB_TABLET_TEST=ON),
    Case("103_aarch64_usb_tablet", "aarch64", USB_TABLET_TEST=ON),
]


def Environment(arch, build_type, settings=None, out_dir=None):
    environment = os.environ.copy()
    for toggle in TOGGLES:
        environment[toggle] = "OFF"
    for key in (
        "POWER_TEST",
        "QEMU_CPU",
        "DISK_LAYOUT",
        "OUT_DIR",
        "KERNEL_ROOT",
        "DISK_BUS",
        "KERNEL_USB",
        "KERNEL_USBHID",
        "KERNEL_PS2",
        "KERNEL_STORAGE",
        "SCREEN",
    ):
        environment.pop(key, None)
    environment.update(settings or {})
    environment["ARCH"] = arch
    environment["BUILD_TYPE"] = build_type
    environment["KERNEL_TESTS"] = "ON"
    if out_dir is not None:
        environment["OUT_DIR"] = out_dir
    return environment


def RunLogged(command, environment, log_path):
    with open(log_path, "w", encoding="utf-8") as log:
        return subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        ).returncode


def Build(arch, build_type):
    log_path = os.path.join(MATRIX_DIR, f"build-{arch}-{build_type}.log")
    started = time.monotonic()
    status = RunLogged(
        ["bash", BUILD_SCRIPT, "--arch", arch, "build"],
        Environment(arch, build_type),
        log_path,
    )
    seconds = time.monotonic() - started
    if status != 0:
        print(f"FAIL  build {arch} {build_type} (log: {log_path})")
        return False
    print(f"ok    build {arch} {build_type} ({seconds:.0f} s)", flush=True)
    return True


def FailureLines(log_path):
    with open(log_path, encoding="utf-8", errors="replace") as log:
        lines = log.read().splitlines()
    picked = [
        line
        for line in lines
        if line.startswith(("FAIL", "ERROR", "[ERROR]", "Traceback"))
    ]
    return picked or lines[-5:]


def Skipped(log_path):
    with open(log_path, encoding="utf-8", errors="replace") as log:
        return any("SKIP:" in line for line in log)


def RunCase(case, print_lock):
    out_dir = os.path.join(MATRIX_DIR, case.name)
    subprocess.run(["rm", "-rf", out_dir], check=False)
    os.makedirs(out_dir)
    log_path = os.path.join(out_dir, "log.txt")
    started = time.monotonic()
    status = RunLogged(
        ["bash", BUILD_SCRIPT, "--arch", case.arch, "smoke"],
        Environment(case.arch, case.build_type, case.settings, out_dir),
        log_path,
    )
    seconds = time.monotonic() - started
    with print_lock:
        if status == 0 and Skipped(log_path):
            print(
                f"skip  {case.name}: no UEFI firmware (see its log)",
                flush=True,
            )
        elif status == 0:
            print(f"ok    {case.name} ({seconds:.0f} s)", flush=True)
        else:
            session = os.path.join(out_dir, "smoke-output.txt")
            print(f"FAIL  {case.name} ({seconds:.0f} s)")
            for line in FailureLines(log_path):
                print(f"      {line}")
            print(
                f"      log: {os.path.relpath(log_path, REPO_ROOT)}, "
                f"session: {os.path.relpath(session, REPO_ROOT)}",
                flush=True,
            )
    return status == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=int(os.environ.get("MATRIX_JOBS", "3")),
        help="cases run at once (default 3, or MATRIX_JOBS)",
    )
    parser.add_argument(
        "-k",
        "--select",
        action="append",
        default=[],
        help="run only cases whose name contains this; may repeat",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="use the builds already made"
    )
    parser.add_argument(
        "--list", action="store_true", help="print the case names and stop"
    )
    args = parser.parse_args()
    cases = [
        case
        for case in CASES
        if not args.select or any(text in case.name for text in args.select)
    ]
    if not cases:
        parser.error("no case matches")
    if args.list:
        print("\n".join(case.name for case in cases))
        return 0
    os.makedirs(MATRIX_DIR, exist_ok=True)
    started = time.monotonic()
    if not args.no_build:
        builds = sorted({(case.arch, case.build_type) for case in cases})
        if not all(Build(arch, build_type) for arch, build_type in builds):
            return 1
    print_lock = threading.Lock()
    ordered = sorted(cases, key=lambda case: -case.Weight())
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(
            pool.map(lambda case: RunCase(case, print_lock), ordered)
        )
    failures = results.count(False)
    print(f"Ran {len(cases)} cases in {time.monotonic() - started:.1f} s")
    print(f"FAILED (failures={failures})" if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
