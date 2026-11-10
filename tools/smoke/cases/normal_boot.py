import re

from smoke.boot import (
    ELF_LOAD_MESSAGE,
    EXPECTED_ARCHITECTURE,
    FAT16_MESSAGE,
    NETWORK_DHCP_MESSAGE,
    NETWORK_IPV6_MESSAGE,
    NETWORK_MESSAGE,
    PANIC_MARKER,
    QEMU_CPU_COUNT,
    READY_MESSAGE,
    SHELL_BANNER,
    SMP_MESSAGE,
    TIMER_STARTED_MESSAGE,
    USER_MODE_MESSAGES,
    USER_MODE_OUTPUTS,
    VFS_MESSAGE,
)
from smoke.cases.base import QEMU_TIMEOUT_SECONDS, BootToShell


class NormalBoot(BootToShell):
    passed = "kernel reached the ready state"

    def Check(self, result):
        output = result.output
        arch = self.args.arch
        if PANIC_MARKER in output:
            return "kernel panicked during startup"
        if USER_MODE_MESSAGES[arch] not in output:
            return f"{arch} user-mode test did not complete"
        if USER_MODE_OUTPUTS[arch] not in result.captured:
            return "user-mode console syscall output was not observed"
        if READY_MESSAGE not in output:
            diagnostics = [
                line.strip()
                for line in output.splitlines()
                if "error:" in line.lower() or "error " in line.lower()
            ]
            return "\n".join(
                [f"kernel did not report ready within {QEMU_TIMEOUT_SECONDS}s"]
                + [line[:240] for line in diagnostics[-8:]]
            )
        if TIMER_STARTED_MESSAGE not in output:
            return "production timer did not start"
        if SMP_MESSAGE not in output:
            return f"kernel did not start all {QEMU_CPU_COUNT} CPUs"
        if NETWORK_MESSAGE not in output:
            return "the virtio-net device did not come up"
        if NETWORK_IPV6_MESSAGE not in output:
            return "IPv6 autoconfiguration did not finish"
        if NETWORK_DHCP_MESSAGE not in output:
            return "DHCP did not configure IPv4"
        serial_line_set = set(output.splitlines())
        missing_cpus = [
            cpu
            for cpu in range(1, QEMU_CPU_COUNT)
            if f"KERN/I: CPU {cpu} online" not in serial_line_set
        ]
        if missing_cpus:
            return f"CPUs {missing_cpus} did not report a complete online line"
        if VFS_MESSAGE not in output:
            return "kernel did not read a file through the VFS"
        if FAT16_MESSAGE not in output:
            return "kernel did not read a file from the FAT16 partition"
        if ELF_LOAD_MESSAGE not in output:
            return "kernel did not load the ELF into a user address space"
        if SHELL_BANNER not in output:
            return "the shell did not start"
        if EXPECTED_ARCHITECTURE[arch] not in output:
            return "kernel did not report its architecture"
        if (
            re.search(
                r"KERN/I: Memory map coverage: \d+ MiB, usable RAM: \d+ MiB",
                output,
            )
            is None
        ):
            return "kernel did not report total and usable memory"
        return None
