import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

LIMITS_INPUT = (
    b"memtest\nmemtest overflow\nmemtest exhaust\nmemtest exhaust\n"
    b"sleep 1.1\nhardening\naslr\naslr\n"
    # Programs copied to the disk and /tmp run by PATH, by path and from the
    # working directory, under the same limits as those in /system.
    b"cp /system/upper /bin/shout\necho from the disk | shout\n"
    b"cp /system/cat /bin/kitty\nkitty /proc/self/status\n"
    b'cp /system/echo /tmp/say\n/tmp/say "two  words" a b\n'
    b"cp /system/aslr /tmp/aslr\n/tmp/aslr\ncd /tmp\n./aslr\ncd\n"
    b"cp /system/memtest /bin/memtest\n/bin/memtest exhaust\n"
    b"/bin/nothing\n/system/hello.txt\n/tmp\n"
    # The descriptor limits: read, raise, refused changes, and back to 256.
    b"ulimit -n\nulimit -n 300\nulimit -n\nulimit -Hn\nulimit -n 2000\n"
    b"ulimit -Hn 100\nulimit -x\nulimit -n 256\nulimit -n\n"
)
EXPECTED_LIMITS_OUTPUT = (
    "memtest: stack and heap ok",
    # memtest overflow ends at the stack guard page with ExitStatusFault.
    "shell: program terminated by a CPU exception",
    "sleep: slept 1.1 s",
    "hardening: 82 checks passed",
    "FROM THE DISK",
    "Name: kitty",
    "\ntwo  words a b",
    "shell: /bin/nothing: No such file or directory",
    "shell: /system/hello.txt: Exec format error",
    "shell: /tmp: Is a directory",
    "ulimit: Operation not permitted",
    "ulimit: Invalid argument",
    "usage: ulimit -n [limit], ulimit -Hn [limit]",
)
ULIMIT_OUTPUT = re.compile(
    r"^256\r?$(?s:.*?)^300\r?$(?s:.*?)^1024\r?$"
    r"(?s:.*?)^ulimit: Operation not permitted\r?$"
    r"(?s:.*?)^ulimit: Invalid argument\r?$"
    r"(?s:.*?)^usage: ulimit(?s:.*?)^256\r?$",
    re.MULTILINE,
)
EXHAUST_OUTPUT = re.compile(
    r"^memtest: page limit reached after (\d+) heap pages, twice\r?$",
    re.MULTILINE,
)
# The 16384-page limit also covers the stack, the arguments page, and the
# image, which is about 20 pages with the C library linked in.
MINIMUM_EXHAUST_PAGES = 16300
ASLR_OUTPUT = re.compile(
    r"^aslr: image=(\d+) stack=(\d+) args=(\d+) heap=(\d+)\r?$", re.MULTILINE
)


def Check(result):
    # memtest exhaust runs twice from /system and once from the disk.
    exhausted = [int(pages) for pages in EXHAUST_OUTPUT.findall(result.output)]
    if (
        len(exhausted) != 3
        or len(set(exhausted)) != 1
        or exhausted[0] < MINIMUM_EXHAUST_PAGES
    ):
        return f"page-limit exhaustion runs reported {exhausted}"
    # aslr runs twice from /system and twice from /tmp.
    layouts = ASLR_OUTPUT.findall(result.output)
    if len(layouts) != 4 or any(
        len(set(field)) != len(field) for field in zip(*layouts)
    ):
        return f"aslr runs did not get distinct layouts: {layouts}"
    if ULIMIT_OUTPUT.search(result.output) is None:
        return "ulimit did not print its limits and refusals in order"
    return None


PART = Part(Lines(LIMITS_INPUT), EXPECTED_LIMITS_OUTPUT, Check)
