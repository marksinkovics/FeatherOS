import datetime
import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# Covers argv, foreground input in a child, and nonzero exit statuses.
PROGRAMS_INPUT = (
    b"echo hello   feather os\nupper\nmixed Case\ndate\nuptime\nfanout 6\n"
    b'date --help\nprimes -h\nfiletest\nlibctest one "two words"\n'
    # libctest reads these at its "> " prompts: read(), fgets(), scanf().
    b"typed into stdin\nread by fgets\n42 answer\nenv\n"
)
EXPECTED_PROGRAMS_OUTPUT = (
    "usage: date",
    "usage: primes <workers 1-16> [limit]",
    "filetest: 33 checks passed",
    "libctest: standard output works",
    "libctest: standard error works",
    "libctest: printf works 7",
    "libctest: dprintf 3",
    "libctest: fprintf to stderr",
    "libctest: perror: No such file or directory",
    "libctest: puts works",
    "Assertion `argc == 1' failed.",
    "spawned by posix_spawn",
    "spawned from the disk",
    "SPAWNED=yes",
    "libctest: /dev/tty works",
    "libctest: written through dup",
    "libctest: stdout is back after posix_spawn",
    "libctest: exit handlers ran in order",
    # env prints the variables init.conf sets, which the shell passes on.
    "HOME=/home",
    "PATH=/system:/bin",
    "hello feather os",
    "upper> ",
    "MIXED CASE",
    "fanout: 6 children exited",
)
# libctest checks the x87 long double functions only on x86-64.
LIBCTEST_CHECKS = {"x86_64": 529, "aarch64": 517}
# `date` prints only the timestamp, unlike the boot log's "Wall clock:" line.
DATE_OUTPUT = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\r?$", re.MULTILINE
)
UPTIME_OUTPUT = re.compile(r"^uptime: \d+:\d{2}:\d{2}\r?$", re.MULTILINE)
# QEMU's RTC follows the host clock; allow for boot time and slow hosts.
MAXIMUM_CLOCK_SKEW_SECONDS = 120


def Check(result):
    libctest = f"libctest: {LIBCTEST_CHECKS[result.arch]} checks passed"
    if libctest not in result.output:
        return f"shell output is missing {[libctest]}{result.limit_note}"
    if UPTIME_OUTPUT.search(result.output) is None:
        return "uptime did not print H:MM:SS"
    date_match = DATE_OUTPUT.search(result.output)
    if date_match is None:
        return "date did not print a timestamp"
    guest_time = datetime.datetime.strptime(
        date_match.group(1), "%Y-%m-%d %H:%M:%S"
    ).replace(tzinfo=datetime.timezone.utc)
    # date ran at some point of the session, which takes minutes.
    session_ended = datetime.datetime.now(datetime.timezone.utc)
    skew = max(
        (result.started - guest_time).total_seconds(),
        (guest_time - session_ended).total_seconds(),
        0,
    )
    if skew > MAXIMUM_CLOCK_SKEW_SECONDS:
        return (
            f"date printed {date_match.group(1)} UTC, "
            f"{skew:.0f} seconds outside the session's host clock"
        )
    return None


PART = Part(Lines(PROGRAMS_INPUT), EXPECTED_PROGRAMS_OUTPUT, Check)
