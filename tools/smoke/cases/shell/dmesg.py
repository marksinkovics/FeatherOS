import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

FIRST_LINES = (
    "KERN/I: Limine transferred control to the kernel",
    "KERN/I: Framebuffer console initialized",
)

# The ring starts with the first line the kernel logged; a line written after
# the timer started (CPU 1 online, from smp.cpp) and one from the PCI scan are
# in it too. Each output is matched after the command that printed it, since
# the boot log has the same lines.
DMESG_INPUT = (
    b"dmesg | head -n 2\n"
    b"cat /proc/kmsg | head -n 1\n"
    b'grep -c "CPU 1 online" /proc/kmsg\n'
    b'dmesg | grep -c "KERN/I: PCI:"\n'
    b"dmesg -h\n"
)
EXPECTED_DMESG = (
    r"dmesg \| head -n 2\r?\n"
    + r"\r?\n".join(re.escape(line) for line in FIRST_LINES)
    + r"\r?$",
    r"cat /proc/kmsg \| head -n 1\r?\n" + re.escape(FIRST_LINES[0]) + r"\r?$",
    r'grep -c "CPU 1 online" /proc/kmsg\r?\n1\r?$',
    r'dmesg \| grep -c "KERN/I: PCI:"\r?\n[1-9][0-9]*\r?$',
    r"^usage: dmesg\r?\nPrints the kernel log",
)


def Check(result):
    missing = [
        pattern
        for pattern in EXPECTED_DMESG
        if not re.search(pattern, result.output, re.MULTILINE)
    ]
    if missing:
        return f"dmesg and /proc/kmsg did not print {missing}"
    return None


PART = Part(Lines(DMESG_INPUT), (), Check)
