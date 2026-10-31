#!/usr/bin/env python3

import argparse
import datetime
import ipaddress
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

READY_MESSAGE = "KERN/I: Kernel initialization complete"
TIMER_STARTED_MESSAGE = "KERN/I: Timer started at 100 Hz"
SHELL_BANNER = "FeatherOS shell. Type 'help' for commands."
VFS_MESSAGE = "KERN/I: VFS: hello.txt read passed"
FAT16_MESSAGE = "KERN/I: VFS: FAT16 BLOCK.TXT read passed"
ELF_LOAD_MESSAGE = "KERN/I: ELF64: test image loaded into user address space"
USER_MODE_MESSAGES = {
    "x86_64": "KERN/I: ELF64: ring 3 console and exit syscalls passed",
    "aarch64": "KERN/I: ELF64: EL0 console and exit syscalls passed",
}
USER_ISOLATION_MESSAGES = {
    "x86_64": "KERN/I: ELF64: ring 3 kernel-memory isolation passed",
    "aarch64": "KERN/I: ELF64: EL0 kernel-memory isolation passed",
}
USER_MODE_OUTPUTS = {
    "x86_64": (
        b"UKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
    "aarch64": (
        b"AKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
}
PANIC_MARKER = "KERN/PANIC:"
EXPECTED_PANIC = "KERN/PANIC: Intentional panic test"
EXPECTED_EXCEPTION = {
    "x86_64": "KERN/EXCEPTION: Divide error",
    "aarch64": "KERN/EXCEPTION: AArch64 undefined instruction",
}
EXPECTED_INTERRUPT = {
    "x86_64": "KERN/IRQ: PIT interrupt acknowledged",
    "aarch64": "KERN/IRQ: GIC software interrupt acknowledged",
}
MAX_CPU_PROTECTIONS = {
    "x86_64": "KERN/I: CPU protections: SMEP SMAP RDRAND",
    "aarch64": "KERN/I: CPU protections: PAN RNDR",
}
EXPECTED_ARCHITECTURE = {
    "x86_64": "KERN/I: Architecture: x86_64",
    "aarch64": "KERN/I: Architecture: aarch64",
}
EXPECTED_MEMORY = "KERN/MEMORY: tests passed"
EXPECTED_TIMER = "KERN/TIMER: periodic ticks passed"
EXPECTED_CALC = "69.25"
EXPECTED_UNKNOWN_COMMAND = "Unknown command: nope"
CALC_INPUT = b"nope\n\ncalc 4 + 66 - (3 / 4)\n"
# Covers argv, foreground input in a child, and nonzero exit statuses.
SHELL_INPUT = (
    b"echo hello   feather os\nupper\nmixed Case\ndate\nuptime\nfanout 6\n"
    b'date --help\nprimes -h\nfiletest\nlibctest one "two words"\n'
    # libctest reads these at its "> " prompts: read(), fgets(), scanf().
    b"typed into stdin\nread by fgets\n42 answer\nenv\n"
    b"mkdir work\ncd work\nwrite a.txt in work\npwd\ncat a.txt\ncd ..\n"
    b"cat work/a.txt\ncd /\nls\ncat hello.txt\ncd nowhere\ncd\npwd\n"
    b"ls /dev\nwrite /tmp/note.txt kept in tmp\ncat /tmp/note.txt\n"
    b"cat /proc/self/status\ncat /proc/meminfo\n"
    # slirp's gateway answers pings, and the stack answers its own address
    # and loopback, for both IP versions.
    b"ping -c 2 10.0.2.2\nping -c 1 10.0.2.15\nping -c 1 127.0.0.1\n"
    b"ping 10.0.2\nping -c 1 fec0::2\nping -c 1 ::1\n"
    b"ping -c 1 fec0::5054:ff:fe12:3456\nping -6 10.0.2.2\n"
    # The text tools, on files and through pipes.
    b"write /tmp/w.txt zeta\nwrite -a /tmp/w.txt alpha\n"
    b"write -a /tmp/w.txt mu\nwrite -a /tmp/w.txt alpha\n"
    b"sort -u /tmp/w.txt\ncat /tmp/w.txt | sort -r\n"
    b"grep -n alpha /tmp/w.txt\ncat /tmp/w.txt | grep -c -i ALPHA\n"
    b"grep -v alpha /tmp/w.txt\nwc /tmp/w.txt\nhead -n 2 /tmp/w.txt\n"
    b"cat /tmp/w.txt | tail -n 2\ncp /tmp/w.txt /tmp/copy.txt\n"
    b"wc -l /tmp/copy.txt\ntouch /tmp/new.txt\nwc -c /tmp/new.txt\n"
    b"write /tmp/n.txt 10\nwrite -a /tmp/n.txt 9\n"
    b"write -a /tmp/n.txt 100\nsort -n /tmp/n.txt\n"
    b"sort -rn /tmp/n.txt\ngrep nothing /tmp/n.txt\n"
    # mv renames within a filesystem, and copies between /tmp and the disk.
    b"mkdir /tmp/box\nmv /tmp/copy.txt /tmp/box\nmv /tmp/box /tmp/crate\n"
    b"wc -l /tmp/crate/copy.txt\nmv /tmp/crate /tmp/crate/inner\n"
    b"mv /tmp/crate/copy.txt work\nwc -l /tmp/crate/copy.txt\n"
    b'mv work/copy.txt "work/Moved Copy.txt"\nwc -w "work/moved copy.txt"\n'
    b"echo first redirected line > /tmp/r.txt\n"
    b"echo second redirected line >> /tmp/r.txt\n"
    b"upper < /tmp/r.txt\ncat < /tmp/r.txt\ncat < /tmp/none\n"
    b"echo through the pipe | upper\nls /dev|upper\n"
    b"echo two stages|upper|cat > /tmp/p.txt\ncat /tmp/p.txt\n"
    b"echo a | | upper\n"
    # A pipeline is one foreground group, so its first command can read the
    # console.
    b"upper | upper\ntwo uppers\n"
    b"upper &\nsleep 1 &\njobs\nwait\nsleep 0.8 &\nfg\n"
    # A background job that starts a program of its own must not take the
    # console from the shell.
    b"primes 1 1000000 > /dev/null &\njobs\n"
    b"echo the shell kept the console\nwait\n"
    b"kill -l\nkill 99999\nkill -TERM 1\n"
    # top must show a busy background worker using most of a CPU, which
    # kill %1 then stops.
    b"ps\nprimes 1 1000000000 > /dev/null &\ntop -n 1 -d 500\nkill %1\nwait\n"
    b"kill %7\n"
    b"exit\n"
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
    b"primes 4 100000\nprimes 16 20000\n"
    b"threads 4 100000\nthreads 15 20000\n"
    b"fptest 8\n"
    b"calc\n1 / 3\n7 / 0\nexit\n"
)


class WholeWrite(bytes):
    """Input written in one piece, so escape sequences arrive together."""


# Line editing at the shell prompt: Home and Right, Up to recall, Ctrl+U and
# Ctrl+W, Delete, and a lone Escape, which the editor ignores.
SHELL_EDIT_INPUT = (
    WholeWrite(b"eco edited line\x1b[H\x1b[C\x1b[Ch\n"),
    WholeWrite(b"\x1b[A\n"),
    WholeWrite(b"echo wrong\x15echo right words\x17\x17after-kill\n"),
    WholeWrite(b"echo deleteXX\x1b[D\x1b[D\x1b[3~\x1b[3~ done\n"),
    WholeWrite(b"echo esc\x1b ok\n"),
    b"history\n",
    b"clear\n",
)
CLEAR_SEQUENCE = b"\x1b[H\x1b[2J"
# Program output lines, which the edited input lines echoed before them do not
# match, and how often each must appear.
EXPECTED_SHELL_EDITS = {
    "edited line": 2,
    "after-kill": 1,
    "delete done": 1,
    "esc ok": 1,
}
EXPECTED_HISTORY_LINE = re.compile(
    r"^ +\d+  echo edited line\r?$", re.MULTILINE
)


class TypeWhen:
    """Keys typed once text appears, for a program that shows no prompt."""

    def __init__(self, text, keys):
        self.text = text.encode()
        self.keys = keys


# vi creates a file, edits it with inserts, motions, a yank and put, undo, a
# search and r, and writes it; a second session refuses :q after a change and
# leaves with :q!. upper prints the file each time, in capitals that vi's own
# screen never shows.
SHELL_VI_INPUT = (
    b"vi /tmp/vi.txt\n",
    TypeWhen(
        '"/tmp/vi.txt" [New]',
        b"ihello world\x1bothird line\x1b0Osecond line\x1bgg$xuGyypddggJu"
        b"/third\nrT:wq\n",
    ),
    b"upper < /tmp/vi.txt\n",
    b"cd /tmp\n",
    b"vi vi.txt\n",
    TypeWhen('"vi.txt" 3L, 35B', b"x:q\n:q!\n"),
    b"upper < vi.txt\n",
    b"cd\n",
)
# Without -n, top redraws in place on the alternate screen until q.
SHELL_TOP_INPUT = (b"top -d 200\n", TypeWhen("; q quits", b"q"))
# Each pattern spans the lines one text-tool command printed, in order.
EXPECTED_TEXT_TOOLS = (
    r"^alpha\r?\nmu\r?\nzeta\r?$",
    r"^zeta\r?\nmu\r?\nalpha\r?\nalpha\r?$",
    r"^2:alpha\r?\n4:alpha\r?$",
    r"^2\r?$",
    r"^zeta\r?\nmu\r?$",
    r"^4 4 20 /tmp/w\.txt\r?$",
    r"^zeta\r?\nalpha\r?$",
    r"^mu\r?\nalpha\r?$",
    r"^4 /tmp/copy\.txt\r?$",
    r"^0 /tmp/new\.txt\r?$",
    r"^9\r?\n10\r?\n100\r?$",
    r"^100\r?\n10\r?\n9\r?$",
    r"^4 /tmp/crate/copy\.txt\r?$",
    r"^mv: /tmp/crate: Invalid argument\r?$",
    r"^wc: /tmp/crate/copy\.txt: No such file or directory\r?$",
    r"^4 work/moved copy\.txt\r?$",
)
EXPECTED_VI_LINES = {"HELLO WORLD": 2, "SECOND LINE": 2, "THIRD LINE": 2}
KEYS_READY = "keys: press keys; q quits"
# keys reads raw input: every special key, Ctrl+C as a byte, then q to quit.
SHELL_KEYS_INPUT = (
    b"keys\n",
    WholeWrite(
        b"\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[2~\x1b[3~\x1b[5~\x1b[6~"
        b"\x1b[Z\x1bOA\x1b[1~\t\x01\x03\x7f \rxq"
    ),
)
EXPECTED_KEYS = [
    KEYS_READY,
    "keys: up",
    "keys: down",
    "keys: right",
    "keys: left",
    "keys: home",
    "keys: end",
    "keys: insert",
    "keys: delete",
    "keys: page-up",
    "keys: page-down",
    "keys: shift-tab",
    "keys: up",
    "keys: home",
    "keys: tab",
    "keys: ctrl-a",
    "keys: ctrl-c",
    "keys: backspace",
    "keys: space",
    "keys: enter",
    "keys: x",
    "keys: done",
]
# Ctrl+C for a command that shows no prompt of its own, sent while it runs.
INTERRUPT_RUNNING = object()


class WaitFor:
    """Holds back the lines after it until text appears, for a background
    program that reports on its own."""

    def __init__(self, text):
        self.text = text.encode()


class WaitUntil:
    """Holds back the lines after it until ready() is true, for work outside
    the guest that later lines depend on."""

    def __init__(self, ready):
        self.ready = ready


INTERRUPT_DELAY_SECONDS = 1.5
CONTROL_C = b"\x03"
# Interrupts a CPU-bound job, a sleep, programs blocked reading input, and the
# shell's own prompt (which only discards the line). The shell test's last
# command must stay last because its output ends the run.
SHELL_INTERRUPT_INPUT = (
    b"primes 1 1000000000\n",
    INTERRUPT_RUNNING,
    # The reader of an interrupted writer sees its input end and exits too.
    b"primes 1 1000000000 | upper\n",
    INTERRUPT_RUNNING,
    # Ctrl+C reaches every command in the foreground group.
    b"sleep 100 | sleep 100\n",
    INTERRUPT_RUNNING,
    b"sleep 100\n",
    INTERRUPT_RUNNING,
    b"upper\n",
    CONTROL_C,
    b"calc\n",
    CONTROL_C,
    CONTROL_C,
    b"calc 1 +\n",
)
EXPECTED_INTERRUPTS = 7
# `date` prints only the timestamp, unlike the boot log's "Wall clock:" line.
DATE_OUTPUT = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) UTC\r?$", re.MULTILINE
)
# pwd prints the directory alone; prompts follow it with "> ".
WORKING_DIRECTORY_OUTPUT = re.compile(r"^(/home(?:/work)?)\r?$", re.MULTILINE)
WORK_FILE_OUTPUT = re.compile(r"^in work\r?$", re.MULTILINE)
UPTIME_OUTPUT = re.compile(r"^uptime: \d+:\d{2}:\d{2}\r?$", re.MULTILINE)
THREADS_WORKER_OUTPUT = re.compile(
    r"^threads: thread \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)
PRIMES_WORKER_OUTPUT = re.compile(
    r"^primes: worker \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)
EXHAUST_OUTPUT = re.compile(
    r"^memtest: page limit reached after (\d+) heap pages, twice\r?$",
    re.MULTILINE,
)
# The 1024-page limit also covers the stack, the arguments page, and the
# image, which is about 20 pages with the C library linked in.
MINIMUM_EXHAUST_PAGES = 960
ASLR_OUTPUT = re.compile(
    r"^aslr: image=(\d+) stack=(\d+) args=(\d+) heap=(\d+)\r?$", re.MULTILINE
)
# QEMU's RTC follows the host clock; allow for boot time and slow hosts.
MAXIMUM_CLOCK_SKEW_SECONDS = 120
EXPECTED_SHELL_OUTPUT = (
    "init: FeatherOS is starting its services",
    # init restarts the console service after the shell's exit command.
    "init: shell exited with status 0",
    "init: restarting shell",
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
    "libctest: 433 checks passed",
    "libctest: written through dup",
    "libctest: stdout is back after posix_spawn",
    "libctest: exit handlers ran in order",
    "No write since last change (add ! to override)",
    "; q quits",
    # env prints the variables init.conf sets, which the shell passes on.
    "HOME=/home",
    "PATH=/system:/bin",
    "system/",
    "etc/",
    "home/",
    "FeatherOS initramfs is ready.",
    "cd: nowhere: no such directory",
    # /dev, /tmp, and /proc; the console shows tabs as spaces.
    "random                  0",
    "kept in tmp",
    "Name: cat",
    "MemTotal: ",
    "64 bytes from 10.0.2.2: icmp_seq=2 time=",
    "2 packets transmitted, 2 received, 0% packet loss",
    "64 bytes from 10.0.2.15: icmp_seq=1 time=",
    "64 bytes from 127.0.0.1: icmp_seq=1 time=",
    # A name of only digits and dots is a mistyped address, not a lookup.
    "ping: 10.0.2: Name or service not known",
    "64 bytes from fec0::2: icmp_seq=1 time=",
    "64 bytes from ::1: icmp_seq=1 time=",
    "64 bytes from fec0::5054:ff:fe12:3456: icmp_seq=1 time=",
    "ping: 10.0.2.2: not an IPv6 address",
    # Redirection; cat's lines start lines of their own, unlike the typed
    # commands that contain the same words.
    "FIRST REDIRECTED LINE",
    "SECOND REDIRECTED LINE",
    "\nfirst redirected line",
    "\nsecond redirected line",
    "shell: /tmp/none: No such file or directory",
    # Pipelines; the typed commands are lower case.
    "THROUGH THE PIPE",
    "RANDOM",
    "\nTWO STAGES",
    "shell: a pipeline has an empty command",
    "TWO UPPERS",
    # Background jobs: upper cannot read the console there and fails.
    "Running  sleep 1 &",
    "Done  sleep 1 &",
    "Exit 1  upper &",
    "\nsleep 0.8 &",
    "\nthe shell kept the console",
    "Done  primes 1 1000000 > /dev/null &",
    " 9 KILL",
    "kill: 99999: no such process",
    "kill: 1: Operation not permitted",
    "  PID  PPID  PGID S CPU     TIME NAME",
    "  PID S  %CPU CPU     TIME NAME",
    "top: up ",
    "Terminated  primes 1 1000000000 > /dev/null &",
    "kill: %7: no such job",
    "hello feather os",
    "upper> ",
    "MIXED CASE",
    "fanout: 6 children exited",
    "memtest: stack and heap ok",
    # memtest overflow ends at the stack guard page with ExitStatusFault.
    "shell: program terminated by a CPU exception",
    "sleep: slept 1.1 s",
    "hardening: 67 checks passed",
    "FROM THE DISK",
    "Name: kitty",
    "\ntwo  words a b",
    "shell: /bin/nothing: No such file or directory",
    "shell: /system/hello.txt: Exec format error",
    "shell: /tmp: Is a directory",
    "fptest: 8 of 8 workers kept their registers",
    "primes: 9592 primes below 100000 with 4 workers in ",
    # Sixteen workers keep sixteen pipes open at once.
    "primes: 2262 primes below 20000 with 16 workers in ",
    # Threads of one process find the same counts as worker processes.
    "threads: 9592 primes below 100000 with 4 threads in ",
    "threads: 2262 primes below 20000 with 15 threads in ",
    "calc> ",
    "0.333333",
    "calc: division by zero",
    "calc: unexpected end of expression",
    "exit status 1",
)
EXPECTED_SCHEDULER = (
    "KERN/SCHEDULER: preemption, wait, exit, reap, blocking, sleep, "
    "and priorities passed"
)
EXPECTED_USER_PREEMPTION = "KERN/SCHEDULER: user-mode preemption passed"
EXPECTED_SMP = (
    "KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact; "
    "busy CPUs counted"
)
POWER_MESSAGES = {
    "shutdown": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Powering off",
    ),
    "reboot": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Restarting",
    ),
}
STORAGE_MESSAGE = (
    "KERN/STORAGE: sector and FAT16 file writes verified; boot count "
)
PERSISTENT_FILE_LINES = 200
# Shell commands for each STORAGE_TEST boot: the first writes notes.txt, and
# the second must still find it after the restart.
STORAGE_SHELL_INPUT = {
    1: b"write notes.txt first line\nwrite -a notes.txt second line\n"
    b"cat notes.txt\nmkdir docs\nwrite docs/plan.txt nested hello\n"
    b'cat "/read me first.txt"\nwrite "My Plans.txt" long name test\n'
    b"ls docs\nls\n",
    2: b"cat notes.txt\nrm notes.txt\ncat notes.txt\nrm notes.txt\n"
    b"cat docs/plan.txt\nrmdir docs\nrm docs/plan.txt\nrmdir docs\n"
    b'cat "my plans.txt"\nrm "MY PLANS.TXT"\nls docs\nls\n',
}
NOTES_CONTENTS = b"first line\nsecond line\n"
PLAN_CONTENTS = b"nested hello\n"
LONG_NAME_CONTENTS = b"long name test\n"
NOTES_OUTPUT = re.compile(r"^first line\r?\nsecond line\r?$", re.MULTILINE)
# The disk also holds BLOCK.TXT and the kernel test's PERSIST.TXT.
# The shell runs in /home; the disk's root also holds BLOCK.TXT, the kernel
# test's PERSIST.TXT, and Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
        "plan.txt",
        "notes.txt",
        "My Plans.txt",
        "docs/",
        "1 directory, 2 files, ",
    ),
    2: (
        "cat: cannot open notes.txt",
        "rm: notes.txt: no such file",
        "\nnested hello",
        "rmdir: docs: not an empty directory",
        "\nlong name test",
        "ls: docs: no such directory",
        "0 files, ",
    ),
}
USER_PREEMPTION_OUTPUTS = {"x86_64": b"UIu", "aarch64": b"AIa"}
# Ctrl+C at the shell prompt only discards the line; "ab" is then an unknown
# command, and keys takes raw input from the same keyboard.
INPUT_LINE_KEYS = (
    "ctrl-c",
    "a",
    "b",
    "x",
    "backspace",
    "ret",
    # Left then z gives "echo azb"; Up recalls it.
    "e",
    "c",
    "h",
    "o",
    "spc",
    "a",
    "b",
    "left",
    "z",
    "ret",
    "up",
    "ret",
    "k",
    "e",
    "y",
    "s",
    "ret",
)
INPUT_RAW_KEYS = ("up", "home", "delete", "esc", "ctrl-a", "shift-tab", "q")
EXPECTED_INPUT_KEYS = [
    KEYS_READY,
    "keys: up",
    "keys: home",
    "keys: delete",
    "keys: escape",
    "keys: ctrl-a",
    "keys: shift-tab",
    "keys: done",
]
# The shell's prompt shows its working directory, such as "/home> ".
SHELL_PROMPT = re.compile(rb"^/[^\n>]*> ", re.MULTILINE)
# Each scripted shell line is typed only once a prompt asks for it, so
# typed-ahead input never overflows the kernel's input queue.
LINE_PROMPT = re.compile(
    rb"^(?:(?:upper|calc|/[^\n>]*)?> |keys: press keys; q quits\r?$)",
    re.MULTILINE,
)
INPUT_ECHO_SEQUENCE = b"abx\x08 \x08\r\n"
QEMU_TIMEOUT_SECONDS = 10
# The scripted shell session runs many programs; AArch64 emulation is slower,
# and takes about 90 s for the whole session.
SHELL_TIMEOUT_SECONDS = 180
QEMU_CPU_COUNT = 4
SMP_MESSAGE = f"KERN/I: SMP: {QEMU_CPU_COUNT} CPUs online with IPIs and timers"
# QEMU gives the first NIC 52:54:00:12:34:56, and slirp answers ARP for
# 10.0.2.2 as 52:55:0a:00:02:02. Its router fe80::2 advertises fec0::/64, and
# it answers neighbor solicitations for fec0::2 as 52:56 and the address's
# last 32 bits.
NETWORK_MESSAGE = "KERN/I: Network: virtio-net 52:54:00:12:34:56"
# init's DHCP client leases slirp's only address.
NETWORK_DHCP_MESSAGE = "KERN/I: Network: IPv4 10.0.2.15/24 via 10.0.2.2"
DHCP_LEASED_MESSAGE = b"dhcp: leased "
NETWORK_IPV6_MESSAGE = (
    "KERN/I: Network: IPv6 fe80::5054:ff:fe12:3456 and "
    "fec0::5054:ff:fe12:3456/64 via fe80::2"
)
EXPECTED_ARP = (
    "KERN/NETWORK: ARP resolved gateway 10.0.2.2 to 52:55:0a:00:02:02"
)
EXPECTED_NETWORK = "KERN/NETWORK: NDP resolved fec0::2 to 52:56:00:00:00:02"
# The test's name server listens on the host's loopback, which the guest
# reaches as 10.0.2.2 and fec0::2. The {port} placeholders become its port.
NETWORK_DNS_ADDRESSES = {
    "gateway.featheros.test": {
        1: [bytes((10, 0, 2, 2))],
        28: [socket.inet_pton(socket.AF_INET6, "fec0::2")],
    },
    # Too many records for a datagram, so the answer comes over TCP.
    "big.featheros.test": {
        1: [bytes((10, 0, 3, number)) for number in range(1, 41)],
        28: [
            socket.inet_pton(socket.AF_INET6, f"fec0::3:{number}")
            for number in range(1, 41)
        ],
    },
}
NETWORK_DNS_ALIASES = {
    "alias.featheros.test": "gateway.featheros.test",
    "six.featheros.test": "gateway.featheros.test",
}
# PTR records, which getnameinfo asks for.
NETWORK_DNS_POINTERS = {
    ipaddress.ip_address(address).reverse_pointer: "gateway.featheros.test"
    for address in ("10.0.2.2", "fec0::2")
}
# QEMU forwards a port on the host's loopback to this one in the guest.
FORWARDED_GUEST_PORT = 8080
# And another to httpd's default port.
HTTPD_GUEST_PORT = 80
HTTPD_HELLO = b"SERVED FROM THE GUEST\n"
HTTPD_DOCS_INDEX = b"DOCS INDEX PAGE\n"
NETWORK_SHELL_INPUT = (
    "net\nhost localhost\n"
    # Names only /etc/hosts knows, which never reach a name server.
    "host ip6-localhost\nping -c 1 FeatherOS.\nping -c 1 ip6-loopback\n"
    "host 127.0.1.1\nhost ::1\n"
    # Name servers in /etc/resolv.conf come before the kernel's.
    "echo nameserver 10.0.2.2#{port} > /etc/resolv.conf\n"
    "host alias.featheros.test\n"
    # Addresses' names come from PTR records under in-addr.arpa and ip6.arpa.
    "host 10.0.2.2\nhost fec0::2\nhost 10.0.2.99\n"
    # /etc/hosts has no IPv4 address for it, so the harness's server says no.
    "host -4 ip6-localhost\n"
    "host nothing.featheros.test\nping -c 1 gateway.featheros.test\n"
    # The server truncates this answer over UDP, so it is asked over TCP.
    "host -4 big.featheros.test\n"
    # Nothing answers on port 9, so the lookup waits out one short timeout
    # before asking the next server.
    "echo nameserver 10.0.2.2#9 > /etc/resolv.conf\n"
    "echo nameserver fec0::2#{port} >> /etc/resolv.conf\n"
    "echo options timeout:1 attempts:1 >> /etc/resolv.conf\n"
    "host -6 six.featheros.test\n"
    # Without name servers in the file, the kernel's list is asked.
    "echo options attempts:2 > /etc/resolv.conf\n"
    "net dns fec0::2#{port}\nhost -6 gateway.featheros.test\n"
    "host -6 big.featheros.test\n"
    "ping -6 -c 1 gateway.featheros.test\n"
    # TCP: the host's echo server answers in upper case, so its replies differ
    # from the typed lines, and so do loopback servers and clients that pipe
    # what they receive through upper.
    "echo over ipv4 | nc 10.0.2.2 {echo_port}\n"
    "echo over ipv6 | nc -6 gateway.featheros.test {echo_port}\n"
    "nc 10.0.2.2 {closed_port}\n"
    f"echo from guest | nc -l {FORWARDED_GUEST_PORT} | upper\n"
    # UDP: the echo server answers datagrams in upper case too. Nothing in
    # the guest listens on port 9, so its ICMP port unreachable errors make
    # the connected sockets fail.
    "echo datagram four | nc -u 10.0.2.2 {udp_echo_port}\n"
    "echo datagram six | nc -u -6 gateway.featheros.test {udp_echo_port}\n"
    "echo nobody | nc -u 127.0.0.1 9\necho nobody | nc -u ::1 9\n"
    "echo server says hi | nc -l 7000 | upper &\nsleep 0.5\n"
    "echo client says hi | nc 127.0.0.1 7000 | upper\n"
    "echo six server | nc -l -6 7001 | upper &\nsleep 0.5\n"
    "echo six client | nc ::1 7001 | upper\n"
    # A program file is large enough to fill windows in both directions; the
    # echo server keeps every byte that cat sends.
    "cat /system/vi | wc -c\n"
    "cat /system/vi | nc 10.0.2.2 {echo_port} | wc -c\n"
    # On its own echo port, the harness's relay drops and reorders a few
    # segments each way, which both sides' TCP must recover from.
    "cat /system/vi | nc 10.0.2.2 {lossy_echo_port} | wc -c\n"
    "cat /system/vi | nc -6 gateway.featheros.test {lossy_echo_port} | wc -c\n"
    # echod serves every client from one thread with non-blocking sockets:
    # the first client holds its connection open while the second is answered.
    "echod > /tmp/echod.log &\nsleep 0.5\n"
    "sleep 3 | nc 127.0.0.1 7 &\nsleep 0.5\n"
    "echo second client | nc ::1 7 | upper\n"
    "sleep 3\n"
    # Over loopback both ends scale their windows: five copies of libctest, a
    # few megabytes, grow the rings past the unscaled 64 KiB each way.
    "cat libctest >> /tmp/b\ncat libctest >> /tmp/b\n"
    "cat libctest >> /tmp/b\ncat libctest >> /tmp/b\n"
    "cat libctest >> /tmp/b\ncat /tmp/b | wc -c\n"
    "cat /tmp/b | nc 127.0.0.1 7 | wc -c\n"
    "cat /tmp/b | nc ::1 7 | wc -c\nrm /tmp/b\n"
    "kill %1\ncat /tmp/echod.log\n"
    # HTTP: the host's server answers by path, over both versions.
    "fetch 10.0.2.2:{http_port}/hello.txt\n"
    "fetch http://[fec0::2]:{http_port}/chunked\n"
    "fetch http://10.0.2.2:{http_port}/moved\n"
    "fetch -6 http://gateway.featheros.test:{http_port}/elsewhere\n"
    "fetch http://10.0.2.2:{http_port}/missing\n"
    "fetch -o /tmp/lines http://10.0.2.2:{http_port}/lines\n"
    "wc -l /tmp/lines\ntail -n 1 /tmp/lines\n"
    "fetch https://featheros.test/\n"
    # httpd serves a directory in /tmp to fetch over both versions and,
    # through port forwarding, to the harness's HttpdClient. The files hold
    # upper-case text, so their contents differ from the typed commands.
    "mkdir /tmp/www\nmkdir /tmp/www/docs\n"
    "echo served from the guest | upper > /tmp/www/hello.txt\n"
    "echo docs index page | upper > /tmp/www/docs/index.html\n"
    'echo spaced out | upper > "/tmp/www/with space.txt"\n'
    "cp /system/vi /tmp/www/vi\n"
    "httpd /tmp/www > /tmp/httpd.log &\nsleep 0.5\n"
    "fetch 127.0.0.1/hello.txt\n"
    "fetch http://[::1]/docs\n"
    "fetch http://[::1]/with%20space.txt\n"
    "fetch 127.0.0.1/\n"
    "fetch 127.0.0.1/nothing\n"
    "fetch -o /tmp/vi.copy http://[::1]/vi\n"
    "cat /tmp/vi.copy | wc -c\n"
    "{wait_for_httpd_client}\n"
    "kill %1\ncat /tmp/httpd.log\n"
    # NTP: the host's server keeps a clock that starts in 2031. The first
    # query only measures; the second sets the clock, after which the offset
    # is close to zero. The server's other port refuses with a kiss.
    "ntp -n -p {ntp_port} 10.0.2.2\n"
    "ntp -6 -p {ntp_port} gateway.featheros.test\n"
    "date\nntp -n -p {ntp_port} 10.0.2.2\n"
    "ntp -p {ntp_kiss_port} 10.0.2.2\n"
    # The running DHCP client renews when asked, which restores the DHCP name
    # server.
    "dhcp -r\n{wait_for_renewal}\nnet\n"
    # The kill switch: while the link is down nothing reaches the harness's
    # servers and every send to them fails at once, but loopback works. Up,
    # IPv6 comes back at once and IPv4 once the DHCP client, which noticed
    # its address gone, sees the link back and leases again.
    # The connection held open through the lossy relay ends with the link.
    "sleep 5 | nc 10.0.2.2 {lossy_echo_port} &\nsleep 0.5\n"
    "net down\n{mark_traffic}\nnet\n"
    "ping -c 1 10.0.2.2\nping -6 -c 1 fec0::2\n"
    "echo cut off | nc 10.0.2.2 {echo_port}\n"
    "echo cut off | nc -6 fec0::2 {echo_port}\n"
    "echo cut off | nc -u 10.0.2.2 {udp_echo_port}\n"
    "fetch 10.0.2.2:{http_port}/hello.txt\n"
    "ping -c 1 127.0.0.1\n"
    "echo kept local | nc -l 7002 | upper &\nsleep 0.5\n"
    "echo loopback client | nc 127.0.0.1 7002 | upper\n"
    "{mark_traffic}\nnet up\n{wait_for_new_lease}\n"
    "echo back up | nc 10.0.2.2 {echo_port}\n"
    "echo six back | nc -6 fec0::2 {echo_port}\n"
    "net\n"
)
LINK_DOWN_MESSAGE = "KERN/I: Network: link down"
LINK_UP_MESSAGE = "KERN/I: Network: link up"
# Between the two messages, with the same placeholders as the input.
EXPECTED_LINK_DOWN_OUTPUT = [
    "link down",
    # The DHCP client looks at its address each second.
    "dhcp: the address is gone; leasing again",
    "ipv4 none",
    "ping: sendto: Network is down",
    "nc: 10.0.2.2 {lossy_echo_port}: Network is down",
    "nc: 10.0.2.2 {echo_port}: Network is down",
    "nc: fec0::2 {echo_port}: Network is down",
    # A UDP socket connects without sending, so its first datagram fails.
    "nc: Network is down",
    "fetch: 10.0.2.2 {http_port}: Network is down",
    "64 bytes from 127.0.0.1: icmp_seq=1 time=",
    "KEPT LOCAL",
    "LOOPBACK CLIENT",
]
EXPECTED_LINK_UP_OUTPUT = [
    "dhcp: leased 10.0.2.15/24 via 10.0.2.2 for 86400 s",
    "BACK UP",
    "SIX BACK",
    "link up",
    "ipv4 10.0.2.15/24 via 10.0.2.2",
    "ipv6 fec0::5054:ff:fe12:3456/64 via fe80::2",
]
HTTP_LINE_COUNT = 10000
HTTP_LINES = "".join(f"line {number}\n" for number in range(HTTP_LINE_COUNT))
EXPECTED_NETWORK_SHELL_OUTPUT = [
    "ipv4 10.0.2.15/24 via 10.0.2.2",
    "ipv6 fe80::5054:ff:fe12:3456",
    "ipv6 fec0::5054:ff:fe12:3456/64 via fe80::2",
    "dns 10.0.2.3",
    # From slirp's router advertisement.
    "dns fec0::3",
    "localhost has address 127.0.0.1",
    "localhost has IPv6 address ::1",
    "ip6-localhost has IPv6 address ::1",
    "PING FeatherOS. (127.0.1.1): 56 data bytes",
    "64 bytes from 127.0.1.1: icmp_seq=1 time=",
    "PING ip6-loopback (::1): 56 data bytes",
    "64 bytes from ::1: icmp_seq=1 time=",
    "127.0.1.1 has name featheros",
    "::1 has name localhost",
    "alias.featheros.test has address 10.0.2.2",
    "alias.featheros.test has IPv6 address fec0::2",
    "10.0.2.2 has name gateway.featheros.test",
    "fec0::2 has name gateway.featheros.test",
    "host: 10.0.2.99: Name or service not known",
    "host: ip6-localhost: Name or service not known",
    "host: nothing.featheros.test: Name or service not known",
    "six.featheros.test has IPv6 address fec0::2",
    # The first and last of the 8 addresses getaddrinfo keeps.
    "big.featheros.test has address 10.0.3.1",
    "big.featheros.test has address 10.0.3.8",
    "big.featheros.test has IPv6 address fec0::3:1",
    "big.featheros.test has IPv6 address fec0::3:8",
    "PING gateway.featheros.test (10.0.2.2): 56 data bytes",
    "64 bytes from 10.0.2.2: icmp_seq=1 time=",
    "gateway.featheros.test has IPv6 address fec0::2",
    "PING gateway.featheros.test (fec0::2): 56 data bytes",
    "64 bytes from fec0::2: icmp_seq=1 time=",
    "OVER IPV4",
    "OVER IPV6",
    "Connection refused",
    "FROM HOST",
    "DATAGRAM FOUR",
    "DATAGRAM SIX",
    "nc: 127.0.0.1 9: Connection refused",
    "nc: ::1 9: Connection refused",
    "CLIENT SAYS HI",
    "SERVER SAYS HI",
    "SIX CLIENT",
    "SIX SERVER",
    "SECOND CLIENT",
    "echod: 127.0.0.1 connected, 1 open",
    "echod: ::1 connected, 2 open",
    "echod: ::1 closed after 14 bytes, 1 open",
    # What the first client sends is sleep's report, "sleep: slept 3 s".
    "echod: 127.0.0.1 closed after 17 bytes, 0 open",
    "greetings from the host",
    "chunks arrive in order",
    "landed via moved",
    "landed via elsewhere",
    "fetch: 404 Not Found",
    f"fetch: saved {len(HTTP_LINES)} bytes to /tmp/lines",
    f"{HTTP_LINE_COUNT} /tmp/lines",
    f"line {HTTP_LINE_COUNT - 1}",
    "fetch: https://featheros.test/: only http URLs are supported",
    HTTPD_HELLO.decode().strip(),
    HTTPD_DOCS_INDEX.decode().strip(),
    "SPACED OUT",
    '<li><a href="docs/">docs/</a></li>',
    '<li><a href="with%20space.txt">with space.txt</a></li>',
    "httpd: serving /tmp/www on port 80",
    f"127.0.0.1 GET /hello.txt 200 {len(HTTPD_HELLO)}",
    "::1 GET /docs 301 ",
    f"::1 GET /docs/ 200 {len(HTTPD_DOCS_INDEX)}",
    "::1 GET /with%20space.txt 200 11",
    "127.0.0.1 GET /nothing 404 ",
    # slirp hands forwarded connections over from the gateway's address.
    f"10.0.2.2 GET /hello.txt 200 {len(HTTPD_HELLO)}",
    "10.0.2.2 HEAD /hello.txt 200 0",
    "10.0.2.2 POST /hello.txt 501 ",
    "10.0.2.2 GET / 505 ",
    "ntp: fec0::2 offset +",
    "ntp: clock set to 2031-01-01 00:0",
    "ntp: 10.0.2.2 refused with kiss code RATE",
    "dhcp: leased 10.0.2.15/24 via 10.0.2.2 for 86400 s",
    "dhcp: renewed 10.0.2.15/24 via 10.0.2.2 for 86400 s",
]
# Before the clock is set it is years behind the server's; after, the server
# and date agree with it to well under a second.
EXPECTED_NTP_PATTERNS = [
    r"ntp: 10\.0\.2\.2 offset \+\d{6,}\.\d{3} s, delay \d+\.\d{3} s, "
    r"stratum 2",
    r"^2031-01-01 00:0\d:\d\d UTC\r?$",
    r"ntp: 10\.0\.2\.2 offset [+-]0\.\d{3} s, delay 0\.\d{3} s, stratum 2",
]
ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
ANSI_ESCAPE_BYTES = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
# A program leaving the alternate screen returns to the shell's line, as a
# newline would, which prompt detection relies on.
LEAVE_ALTERNATE_SCREEN = b"\x1b[?1049l"


def EncodeDnsName(name):
    return (
        b"".join(
            bytes((len(label),)) + label.encode() for label in name.split(".")
        )
        + b"\0"
    )


def BindLoopbackPorts(kinds):
    """Binds a socket of each kind to one free port on both 127.0.0.1 and
    ::1; returns the port and the sockets by family and kind."""
    for _ in range(20):
        sockets = {}
        port = 0
        try:
            for kind in kinds:
                for family, address in (
                    (socket.AF_INET, "127.0.0.1"),
                    (socket.AF_INET6, "::1"),
                ):
                    bound = socket.socket(family, kind)
                    sockets[family, kind] = bound
                    bound.bind((address, port))
                    port = bound.getsockname()[1]
            return port, sockets
        except OSError:
            for bound in sockets.values():
                bound.close()
    raise OSError("no port free on both loopback addresses")


def BindLoopbackPair(kind):
    """Binds sockets of kind to one free port on 127.0.0.1 and ::1."""
    port, sockets = BindLoopbackPorts((kind,))
    return port, sockets[socket.AF_INET, kind], sockets[socket.AF_INET6, kind]


def ReceiveExactly(connection, count):
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


class DnsServer:
    """Answers A and AAAA queries from NETWORK_DNS_ADDRESSES and
    NETWORK_DNS_ALIASES, and PTR queries from NETWORK_DNS_POINTERS, on the
    host's loopback, over IPv4 and IPv6 on one port for UDP and TCP, and
    counts the queries each version carried over each. A UDP answer over 512
    bytes is truncated to its question, with the TC bit set, as servers do
    without EDNS."""

    def __init__(self):
        self.port, sockets = BindLoopbackPorts(
            (socket.SOCK_DGRAM, socket.SOCK_STREAM)
        )
        self.sockets = list(sockets.values())
        self.queries = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.stream_queries = {socket.AF_INET: 0, socket.AF_INET6: 0}
        for (family, kind), listener in sockets.items():
            if kind == socket.SOCK_STREAM:
                listener.listen()
                target = self.ServeStreams
            else:
                target = self.Serve
            threading.Thread(
                target=target, args=(family, listener), daemon=True
            ).start()

    def Serve(self, family, listener):
        while True:
            try:
                query, sender = listener.recvfrom(512)
            except OSError:
                return
            response = self.Answer(query)
            if response is not None:
                self.queries[family] += 1
                if len(response) > 512:
                    response = self.Truncate(response)
                listener.sendto(response, sender)

    def ServeStreams(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            with connection:
                connection.settimeout(10)
                try:
                    # Each message follows its length in two bytes.
                    while prefix := ReceiveExactly(connection, 2):
                        length = struct.unpack("!H", prefix)[0]
                        query = ReceiveExactly(connection, length)
                        response = query and self.Answer(query)
                        if not response:
                            break
                        self.stream_queries[family] += 1
                        connection.sendall(
                            struct.pack("!H", len(response)) + response
                        )
                except OSError:
                    pass

    @staticmethod
    def Truncate(response):
        offset = 12
        while response[offset] != 0:
            offset += 1 + response[offset]
        identifier, flags = struct.unpack("!HH", response[:4])
        header = struct.pack("!HHHHHH", identifier, flags | 0x0200, 1, 0, 0, 0)
        return header + response[12 : offset + 5]

    @staticmethod
    def Answer(query):
        if len(query) < 12:
            return None
        identifier, _, questions = struct.unpack("!HHH", query[:6])
        labels = []
        offset = 12
        while offset < len(query) and query[offset] != 0:
            length = query[offset]
            labels.append(query[offset + 1 : offset + 1 + length].decode())
            offset += 1 + length
        offset += 1
        if questions != 1 or offset + 4 > len(query):
            return None
        record_type = struct.unpack("!H", query[offset : offset + 2])[0]
        question = query[12 : offset + 4]
        name = ".".join(labels).lower()
        answers = []
        target = NETWORK_DNS_ALIASES.get(name)
        if target is not None:
            answers.append(
                b"\xc0\x0c"
                + struct.pack("!HHIH", 5, 1, 60, len(EncodeDnsName(target)))
                + EncodeDnsName(target)
            )
        owner = target or name
        records = NETWORK_DNS_ADDRESSES.get(owner)
        pointer = NETWORK_DNS_POINTERS.get(owner)
        if pointer is not None:
            records = {12: [EncodeDnsName(pointer)]}
        if records is None:
            # NXDOMAIN, with recursion desired and available.
            header = struct.pack("!HHHHHH", identifier, 0x8183, 1, 0, 0, 0)
            return header + question
        for data in records.get(record_type, []):
            owner_name = (
                EncodeDnsName(owner) if target is not None else b"\xc0\x0c"
            )
            answers.append(
                owner_name
                + struct.pack("!HHIH", record_type, 1, 60, len(data))
                + data
            )
        header = struct.pack(
            "!HHHHHH", identifier, 0x8180, 1, len(answers), 0, 0
        )
        return header + question + b"".join(answers)

    def Close(self):
        for listener in self.sockets:
            listener.close()


# The test's NTP server keeps this time, advancing from when it starts.
NTP_SERVER_START = datetime.datetime(2031, 1, 1, tzinfo=datetime.timezone.utc)
# Seconds from the NTP epoch, 1900, to the Unix one.
NTP_UNIX_EPOCH = 2208988800


class NtpServer:
    """Answers NTP client requests on the host's loopback by a clock that
    starts at NTP_SERVER_START, over IPv4 and IPv6 on one port, and refuses
    every request to kiss_port with a RATE kiss; counts the answered requests
    each version carried."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_DGRAM)
        kiss = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        kiss.bind(("127.0.0.1", 0))
        self.kiss_port = kiss.getsockname()[1]
        self.started = time.monotonic()
        self.requests = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.sockets = [
            (socket.AF_INET, ipv4, False),
            (socket.AF_INET6, ipv6, False),
            (socket.AF_INET, kiss, True),
        ]
        for family, listener, refuse in self.sockets:
            threading.Thread(
                target=self.Serve, args=(family, listener, refuse), daemon=True
            ).start()

    def Now(self):
        seconds = (
            NTP_SERVER_START.timestamp()
            + NTP_UNIX_EPOCH
            + time.monotonic()
            - self.started
        )
        return int(seconds * (1 << 32))

    def Serve(self, family, listener, refuse):
        while True:
            try:
                request, sender = listener.recvfrom(512)
            except OSError:
                return
            # Only client packets: leap indicator, version, and mode 3.
            if len(request) < 48 or request[0] & 7 != 3:
                continue
            received = self.Now()
            version = request[0] >> 3 & 7
            poll = request[2]
            origin = request[40:48]
            if refuse:
                # A kiss is unsynchronized, stratum 0, with its code in the
                # reference ID; only the origin timestamp matters.
                reply = struct.pack(
                    "!BBBbII4sQ8sQQ",
                    3 << 6 | version << 3 | 4,
                    0,
                    poll,
                    -20,
                    0,
                    0,
                    b"RATE",
                    0,
                    origin,
                    0,
                    0,
                )
            else:
                reply = struct.pack(
                    "!BBBbII4sQ8sQQ",
                    version << 3 | 4,
                    2,
                    poll,
                    -20,
                    0,
                    1 << 8,
                    bytes((127, 0, 0, 1)),
                    received - (60 << 32),
                    origin,
                    received,
                    self.Now(),
                )
                self.requests[family] += 1
            listener.sendto(reply, sender)

    def Close(self):
        for _, listener, _ in self.sockets:
            listener.close()


def FreeTcpPort(address="127.0.0.1"):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((address, 0))
        return probe.getsockname()[1]


class EchoServer:
    """Echoes TCP connections in upper case on the host's loopback, over IPv4
    and IPv6 on one port, until the client finishes sending, and UDP
    datagrams on another port; counts the connections and datagrams each
    version carried."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_STREAM)
        self.udp_port, udp_ipv4, udp_ipv6 = BindLoopbackPair(socket.SOCK_DGRAM)
        self.listeners = {socket.AF_INET: ipv4, socket.AF_INET6: ipv6}
        self.datagram_sockets = {
            socket.AF_INET: udp_ipv4,
            socket.AF_INET6: udp_ipv6,
        }
        self.connections = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.datagrams = {socket.AF_INET: 0, socket.AF_INET6: 0}
        for family, listener in self.listeners.items():
            listener.listen()
            threading.Thread(
                target=self.Serve, args=(family, listener), daemon=True
            ).start()
        for family, receiver in self.datagram_sockets.items():
            threading.Thread(
                target=self.ServeDatagrams,
                args=(family, receiver),
                daemon=True,
            ).start()

    def ServeDatagrams(self, family, receiver):
        while True:
            try:
                datagram, sender = receiver.recvfrom(2048)
                self.datagrams[family] += 1
                receiver.sendto(datagram.upper(), sender)
            except OSError:
                return

    def Serve(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            self.connections[family] += 1
            with connection:
                connection.settimeout(10)
                try:
                    while chunk := connection.recv(4096):
                        connection.sendall(chunk.upper())
                except OSError:
                    pass

    def Close(self):
        for listener in self.listeners.values():
            listener.close()
        for receiver in self.datagram_sockets.values():
            receiver.close()


class HttpServer:
    """Answers HTTP/1.1 GET requests by path on the host's loopback, over
    IPv4 and IPv6 on one port, closing each connection after its response;
    keeps the Host header of each request with the version it came over."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_STREAM)
        self.listeners = {socket.AF_INET: ipv4, socket.AF_INET6: ipv6}
        self.hosts = {socket.AF_INET: [], socket.AF_INET6: []}
        for family, listener in self.listeners.items():
            listener.listen()
            threading.Thread(
                target=self.Serve, args=(family, listener), daemon=True
            ).start()

    def Serve(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            with connection:
                connection.settimeout(10)
                try:
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = connection.recv(4096)
                        if not chunk:
                            break
                        request += chunk
                    head = request.split(b"\r\n\r\n")[0].decode()
                    lines = head.split("\r\n")
                    _, target, _ = lines[0].split(" ")
                    headers = dict(
                        line.split(": ", 1)
                        for line in lines[1:]
                        if ": " in line
                    )
                    self.hosts[family].append(headers.get("Host"))
                    connection.sendall(self.Respond(target))
                    connection.shutdown(socket.SHUT_WR)
                    while connection.recv(4096):
                        pass
                except (OSError, ValueError):
                    pass

    def Respond(self, target):
        path, _, query = target.partition("?")

        def Complete(status, headers, body=b""):
            return (
                f"HTTP/1.1 {status}\r\n{headers}"
                f"Content-Length: {len(body)}\r\n\r\n"
            ).encode() + body

        if path == "/hello.txt":
            return Complete(
                "200 OK",
                "Content-Type: text/plain\r\n",
                b"greetings from the host\n",
            )
        if path == "/chunked":
            return (
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                b"7\r\nchunks \r\n7;note=x\r\narrive \r\n"
                b"9\r\nin order\n\r\n0\r\nX-Trailer: done\r\n\r\n"
            )
        if path == "/moved":
            return Complete("302 Found", "Location: landing?via=moved\r\n")
        if path == "/elsewhere":
            return Complete(
                "301 Moved Permanently",
                f"Location: http://gateway.featheros.test:{self.port}"
                "/landing?via=elsewhere\r\n",
            )
        if path == "/landing":
            via = query.partition("=")[2]
            return Complete("200 OK", "", f"landed via {via}\n".encode())
        if path == "/lines":
            # No length: the body ends when the connection closes.
            return b"HTTP/1.1 200 OK\r\n\r\n" + HTTP_LINES.encode()
        return Complete("404 Not Found", "", b"no such page\n")

    def Close(self):
        for listener in self.listeners.values():
            listener.close()


class LossyRelay:
    """Carries the guest's Ethernet frames between QEMU's user-mode network
    and its network device through filter-redirector objects, since QEMU
    itself never loses or reorders a packet. On TCP connections to port, it
    drops the DROPPED new data segments of each direction the first time they
    pass, and holds back the HELD one until the next frame has passed. It
    counts what it did by IP version and direction."""

    DROPPED = (5, 7)
    HELD = 12
    # A held frame goes on alone if nothing follows it for this long.
    HOLD_SECONDS = 0.05

    def __init__(self, port):
        self.port = port
        self.directory = tempfile.TemporaryDirectory(prefix="fr-")
        self.dropped = {}
        self.reordered = {}
        self.listeners = {}
        for name in ("rx_out", "rx_in", "tx_out", "tx_in"):
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(os.path.join(self.directory.name, name))
            listener.listen(1)
            self.listeners[name] = listener

    def QemuArguments(self, netdev):
        arguments = []
        for name in self.listeners:
            path = os.path.join(self.directory.name, name)
            arguments += ["-chardev", f"socket,id=relay_{name},path={path}"]
        # rx carries what the guest sends, tx what QEMU sends the guest.
        for queue in ("rx", "tx"):
            arguments += [
                "-object",
                f"filter-redirector,id=relay_{queue},netdev={netdev},"
                f"queue={queue},outdev=relay_{queue}_out,"
                f"indev=relay_{queue}_in",
            ]
        return arguments

    def Start(self):
        for queue, toward_guest in (("rx", False), ("tx", True)):
            threading.Thread(
                target=self.Relay, args=(queue, toward_guest), daemon=True
            ).start()

    @staticmethod
    def ParseSegment(frame):
        """Returns (family, flow, sequence, payload length) for a TCP
        segment in an Ethernet frame, or None."""
        ethertype = int.from_bytes(frame[12:14], "big")
        packet = frame[14:]
        if ethertype == 0x0800 and len(packet) >= 20 and packet[9] == 6:
            family = socket.AF_INET
            addresses = packet[12:20]
            end = int.from_bytes(packet[2:4], "big")
            segment = packet[(packet[0] & 0xF) * 4 : end]
        elif ethertype == 0x86DD and len(packet) >= 40 and packet[6] == 6:
            family = socket.AF_INET6
            addresses = packet[8:40]
            end = 40 + int.from_bytes(packet[4:6], "big")
            segment = packet[40:end]
        else:
            return None
        if len(segment) < 20:
            return None
        flow = (addresses, segment[0:4])
        sequence = int.from_bytes(segment[4:8], "big")
        payload = len(segment) - (segment[12] >> 4) * 4
        return family, flow, sequence, payload

    def Relay(self, queue, toward_guest):
        source, _ = self.listeners[f"{queue}_out"].accept()
        destination, _ = self.listeners[f"{queue}_in"].accept()
        flows = {}
        held = None
        held_until = 0
        received = bytearray()
        while True:
            wait = (
                max(0, held_until - time.monotonic())
                if held is not None
                else None
            )
            if not select.select([source], [], [], wait)[0]:
                destination.sendall(held)
                held = None
                continue
            chunk = source.recv(65536)
            if not chunk:
                return
            received += chunk
            # QEMU frames each packet with its length, big-endian.
            while len(received) >= 4:
                length = int.from_bytes(received[:4], "big")
                if len(received) < 4 + length:
                    break
                frame = bytes(received[4 : 4 + length])
                del received[: 4 + length]
                family, verdict = self.Judge(frame, flows)
                key = (family, toward_guest)
                if verdict == "drop":
                    self.dropped[key] = self.dropped.get(key, 0) + 1
                    continue
                if verdict == "hold":
                    held = struct.pack(">I", length) + frame
                    held_until = time.monotonic() + self.HOLD_SECONDS
                    continue
                destination.sendall(struct.pack(">I", length) + frame)
                if held is not None:
                    destination.sendall(held)
                    held = None
                    self.reordered[key] = self.reordered.get(key, 0) + 1

    def Judge(self, frame, flows):
        """Returns (family, "drop" | "hold" | "pass"), with no family for a
        frame that is not a TCP segment."""
        parsed = self.ParseSegment(frame)
        if parsed is None:
            return (None, "pass")
        family, flow, sequence, payload = parsed
        ports = struct.unpack(">HH", flow[1])
        if self.port not in ports or payload == 0:
            return (family, "pass")
        end, count = flows.get(flow, (sequence, 0))
        # Only data past what the flow sent before counts; a retransmission
        # always passes.
        if (sequence - end) % (1 << 32) >= 1 << 31:
            return (family, "pass")
        count += 1
        flows[flow] = ((sequence + payload) % (1 << 32), count)
        if count in self.DROPPED:
            return (family, "drop")
        if count == self.HELD:
            return (family, "hold")
        return (family, "pass")

    def Close(self):
        for listener in self.listeners.values():
            listener.close()
        self.directory.cleanup()


class ForwardedClient:
    """Connects to a guest server through QEMU's port forwarding, retrying
    until the guest accepts, sends a line and keeps what the guest sends
    back."""

    REQUEST = b"from host\n"

    def __init__(self, port):
        self.port = port
        self.received = b""
        self.thread = None

    def Start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.Run, daemon=True)
            self.thread.start()

    def Run(self):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=5
                ) as connection:
                    connection.sendall(self.REQUEST)
                    data = b""
                    while chunk := connection.recv(4096):
                        data += chunk
                    # A guest not listening yet resets the connection, which
                    # QEMU passes on as an empty one.
                    if data:
                        self.received = data
                        return
            except OSError:
                pass
            time.sleep(0.2)


def ParseHttpResponses(data, methods):
    """Splits data into one (status, headers, body) per request method, with
    lower-case header names; raises ValueError for anything left over."""
    responses = []
    for method in methods:
        head, separator, data = data.partition(b"\r\n\r\n")
        if not separator:
            raise ValueError(f"incomplete response head {head[:60]!r}")
        lines = head.decode().split("\r\n")
        status = int(lines[0].split(" ")[1])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(": ")
            headers[name.lower()] = value
        length = 0 if method == "HEAD" else int(headers["content-length"])
        responses.append((status, headers, data[:length]))
        data = data[length:]
    if data:
        raise ValueError(f"bytes after the responses: {data[:60]!r}")
    return responses


class HttpdClient:
    """Sends requests to the guest's httpd through QEMU's port forwarding,
    retrying the first until the guest answers, and keeps the first response
    that differs from what it expects."""

    def __init__(self, port, program_path):
        self.port = port
        self.program_path = program_path
        self.failure = None
        self.done = threading.Event()
        self.thread = None

    def Start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.Run, daemon=True)
            self.thread.start()

    def Send(self, request):
        with socket.create_connection(
            ("127.0.0.1", self.port), timeout=10
        ) as connection:
            connection.sendall(request)
            data = b""
            while chunk := connection.recv(65536):
                data += chunk
            return data

    def Run(self):
        try:
            self.Check()
        except (OSError, ValueError, KeyError, IndexError) as error:
            self.failure = f"{type(error).__name__}: {error}"
        finally:
            self.done.set()

    @staticmethod
    def Expect(name, response, status, body=None, headers=None, absent=()):
        actual_status, actual_headers, actual_body = response
        if actual_status != status:
            raise ValueError(f"{name}: status {actual_status}, not {status}")
        if body is not None and actual_body != body:
            raise ValueError(f"{name}: body {actual_body[:60]!r}")
        for header, value in (headers or {}).items():
            if actual_headers.get(header) != value:
                raise ValueError(
                    f"{name}: {header} is {actual_headers.get(header)!r}"
                )
        for header in absent:
            if header in actual_headers:
                raise ValueError(f"{name}: unexpected {header} header")

    def Check(self):
        # Two requests in one send: the second waits behind the first's
        # response, and only it closes the connection.
        pipelined = (
            b"GET /hello.txt HTTP/1.1\r\nHost: guest\r\n\r\n"
            b"HEAD /hello.txt HTTP/1.1\r\nHost: guest\r\n"
            b"Connection: close\r\n\r\n"
        )
        deadline = time.monotonic() + 60
        data = b""
        while not data:
            if time.monotonic() > deadline:
                raise OSError("the guest's httpd never answered")
            # A guest not listening yet resets the connection, which QEMU
            # passes on as an empty one.
            try:
                data = self.Send(pipelined)
            except OSError:
                time.sleep(0.2)
        get, head = ParseHttpResponses(data, ("GET", "HEAD"))
        text = {"content-type": "text/plain; charset=utf-8"}
        self.Expect(
            "pipelined GET", get, 200, HTTPD_HELLO, text, ("connection",)
        )
        self.Expect(
            "pipelined HEAD",
            head,
            200,
            b"",
            {"content-length": str(len(HTTPD_HELLO)), "connection": "close"},
        )
        with open(self.program_path, "rb") as program:
            program_bytes = program.read()
        close = b"Host: guest\r\nConnection: close\r\n\r\n"
        cases = (
            (
                "escape",
                b"GET /../../system/init.conf HTTP/1.1\r\n" + close,
                404,
                None,
                {},
            ),
            (
                "encoded escape",
                b"GET /%2e%2e/%2E%2E/system/init.conf HTTP/1.1\r\n" + close,
                404,
                None,
                {},
            ),
            (
                "POST",
                b"POST /hello.txt HTTP/1.1\r\nHost: guest\r\n"
                b"Content-Length: 5\r\n\r\nhello",
                501,
                None,
                {"connection": "close"},
            ),
            (
                "HTTP/1.0",
                b"GET /hello.txt HTTP/1.0\r\n\r\n",
                200,
                HTTPD_HELLO,
                {"connection": "close"},
            ),
            (
                "no Host",
                b"GET /hello.txt HTTP/1.1\r\n\r\n",
                400,
                None,
                {"connection": "close"},
            ),
            ("HTTP/2.0", b"GET / HTTP/2.0\r\n" + close, 505, None, {}),
            (
                "directory",
                b"GET /docs HTTP/1.1\r\n" + close,
                301,
                None,
                {"location": "/docs/"},
            ),
            (
                "absolute form",
                b"GET http://guest/docs/ HTTP/1.1\r\n" + close,
                200,
                HTTPD_DOCS_INDEX,
                {"content-type": "text/html; charset=utf-8"},
            ),
            (
                "program",
                b"GET /vi HTTP/1.1\r\n" + close,
                200,
                program_bytes,
                {"content-type": "application/octet-stream"},
            ),
        )
        for name, request, status, body, headers in cases:
            (response,) = ParseHttpResponses(self.Send(request), ("GET",))
            self.Expect(name, response, status, body, headers)


def PartitionOffset(disk):
    """Byte offset of the first partition, from a GPT or the MBR."""
    mbr = disk.read(512)
    if mbr[446 + 4] != 0xEE:
        return struct.unpack_from("<I", mbr, 446 + 8)[0] * 512
    disk.seek(512)
    header = disk.read(512)
    disk.seek(struct.unpack_from("<Q", header, 72)[0] * 512)
    return struct.unpack_from("<Q", disk.read(128), 32)[0] * 512


def VerifyDiskImage(disk_path, boot_count):
    """Checks the guest's FAT16 writes with mtools, a separate FAT reader."""
    with open(disk_path, "rb") as disk:
        partition_offset = PartitionOffset(disk)
        disk.seek(partition_offset)
        boot_sector = disk.read(512)
        reserved_sectors = struct.unpack_from("<H", boot_sector, 14)[0]
        fat_count = boot_sector[16]
        fat_bytes = struct.unpack_from("<H", boot_sector, 22)[0] * 512
        disk.seek(partition_offset + reserved_sectors * 512)
        fats = [disk.read(fat_bytes) for _ in range(fat_count)]
    if any(fat != fats[0] for fat in fats):
        return "the FAT copies differ"

    image = f"{disk_path}@@{partition_offset}"
    environment = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    expected = "".join(
        f"FeatherOS boot {boot_count} line {line}\n"
        for line in range(PERSISTENT_FILE_LINES)
    ).encode()
    contents = subprocess.run(
        ["mtype", "-i", image, "::PERSIST.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if contents.returncode != 0 or contents.stdout != expected:
        return "mtools read different PERSIST.TXT contents"
    listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if listing.returncode != 0 or "SCRATCH" in listing.stdout:
        return "mtools still lists the removed scratch file"
    notes = subprocess.run(
        ["mtype", "-i", image, "::home/notes.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    plan = subprocess.run(
        ["mtype", "-i", image, "::home/docs/plan.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and notes.stdout != NOTES_CONTENTS:
        return "mtools read different home/notes.txt contents"
    long_name = subprocess.run(
        ["mtype", "-i", image, "::home/My Plans.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and plan.stdout != PLAN_CONTENTS:
        return "mtools read different home/docs/plan.txt contents"
    if boot_count == 1 and long_name.stdout != LONG_NAME_CONTENTS:
        return "mtools read different 'My Plans.txt' contents"
    home_listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::home"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    listed = home_listing.stdout.upper()
    if boot_count == 2 and any(
        name in listed for name in ("NOTES", "DOCS", "MY PLANS")
    ):
        return "mtools still lists a file or directory the shell removed"
    return None


def SendQmpCommand(channel, command_id, execute, arguments=None):
    request = {"execute": execute, "id": command_id}
    if arguments is not None:
        request["arguments"] = arguments
    channel.write(json.dumps(request).encode() + b"\r\n")
    channel.flush()
    while True:
        response = json.loads(channel.readline())
        if response.get("id") == command_id:
            if "error" in response:
                raise RuntimeError(response["error"].get("desc", "QMP error"))
            return response


def SendQmpKeys(channel, first_id, keys):
    for offset, key in enumerate(keys):
        SendQmpCommand(
            channel,
            first_id + offset,
            "human-monitor-command",
            {"command-line": f"sendkey {key}"},
        )
    return first_id + len(keys)


def OpenQmpChannel(socket_path):
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5)
    connection.connect(socket_path)
    channel = connection.makefile("rwb")
    greeting = json.loads(channel.readline())
    if "QMP" not in greeting:
        raise RuntimeError("QEMU did not send a QMP greeting")
    SendQmpCommand(channel, 1, "qmp_capabilities")
    return connection, channel


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
        "--cpu", help="QEMU CPU model; 'max' also requires its CPU protections"
    )
    parser.add_argument(
        "--expect-panic",
        action="store_true",
        help="Expect the intentional panic test",
    )
    parser.add_argument(
        "--expect-exception",
        action="store_true",
        help="Expect an intentional CPU exception",
    )
    parser.add_argument(
        "--expect-interrupt",
        action="store_true",
        help="Expect the architecture-specific hardware interrupt test",
    )
    parser.add_argument(
        "--expect-memory",
        action="store_true",
        help="Expect the physical and virtual memory tests to pass",
    )
    parser.add_argument(
        "--expect-timer",
        action="store_true",
        help="Expect the architecture-specific periodic timer test",
    )
    parser.add_argument(
        "--expect-scheduler",
        action="store_true",
        help="Expect cooperative task switching to pass",
    )
    parser.add_argument(
        "--expect-smp",
        action="store_true",
        help="Expect tasks to run concurrently on several CPUs",
    )
    parser.add_argument(
        "--expect-network",
        action="store_true",
        help="Expect the network device to resolve the gateway with ARP",
    )
    parser.add_argument(
        "--expect-power",
        choices=("shutdown", "reboot"),
        help="Run the shell's shutdown or reboot and expect QEMU to exit or "
        "boot again",
    )
    parser.add_argument(
        "--expect-storage-boot",
        type=int,
        default=0,
        help="Expect a verified disk write that counts this many boots",
    )
    parser.add_argument(
        "--expect-user-preemption",
        action="store_true",
        help="Expect timer-driven user-task switching to pass",
    )
    parser.add_argument(
        "--expect-input",
        action="store_true",
        help="Send serial input and expect the kernel to accept a line",
    )
    parser.add_argument(
        "--expect-calc",
        action="store_true",
        help="Send a calculator command and expect its result",
    )
    parser.add_argument(
        "--expect-shell",
        action="store_true",
        help="Run shell commands and expect their output",
    )
    parser.add_argument("iso", help="Path to the bootable FeatherOS ISO")
    args = parser.parse_args()
    if (
        sum(
            (
                args.expect_panic,
                args.expect_exception,
                args.expect_interrupt,
                args.expect_memory,
                args.expect_timer,
                args.expect_scheduler,
                args.expect_smp,
                args.expect_network,
                args.expect_user_preemption,
                args.expect_input,
                args.expect_calc,
                args.expect_shell,
            )
        )
        > 1
    ):
        parser.error("select at most one expected result")

    qemu_name = (
        "qemu-system-aarch64"
        if args.arch == "aarch64"
        else "qemu-system-x86_64"
    )
    qemu = shutil.which(qemu_name)
    if qemu is None:
        print(f"ERROR: {qemu_name} was not found on PATH", file=sys.stderr)
        return 1

    command = [qemu]
    use_qmp_keyboard = args.expect_input
    qmp_directory = (
        tempfile.TemporaryDirectory(prefix="fq-") if use_qmp_keyboard else None
    )
    qmp_socket_path = (
        os.path.join(qmp_directory.name, "qmp.sock")
        if qmp_directory is not None
        else None
    )
    if args.arch == "aarch64":
        if not args.firmware:
            print("ERROR: --firmware is required for AArch64", file=sys.stderr)
            return 1
        command.extend(
            [
                "-machine",
                "virt",
                "-cpu",
                args.cpu or "cortex-a72",
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
        if args.cpu:
            command.extend(["-cpu", args.cpu])
    forwarded_client = (
        ForwardedClient(FreeTcpPort()) if args.expect_network else None
    )
    httpd_client = None
    netdev = "user,id=featheros_net"
    if forwarded_client is not None:
        httpd_client = HttpdClient(
            FreeTcpPort(),
            os.path.join(
                os.path.dirname(args.disk), "initramfs", "system", "vi"
            ),
        )
        netdev += (
            f",hostfwd=tcp:127.0.0.1:{forwarded_client.port}-"
            f":{FORWARDED_GUEST_PORT}"
            f",hostfwd=tcp:127.0.0.1:{httpd_client.port}-"
            f":{HTTPD_GUEST_PORT}"
        )
    command.extend(
        [
            "-drive",
            f"if=none,id=featheros_disk,file={args.disk},format=raw",
            "-device",
            "virtio-blk-device,drive=featheros_disk"
            if args.arch == "aarch64"
            else "virtio-blk-pci,drive=featheros_disk,disable-legacy=off",
            # QEMU's user-mode network: the guest is 10.0.2.15 behind 10.0.2.2.
            "-netdev",
            netdev,
            "-device",
            "virtio-net-device,netdev=featheros_net"
            if args.arch == "aarch64"
            # An empty romfile skips the network boot ROM.
            else "virtio-net-pci,netdev=featheros_net,disable-legacy=off,"
            "romfile=",
        ]
    )
    lossy_echo_server = EchoServer() if args.expect_network else None
    relay = LossyRelay(lossy_echo_server.port) if args.expect_network else None
    if relay is not None:
        command.extend(relay.QemuArguments("featheros_net"))
    command.extend(
        [
            "-display",
            "none",
            "-serial",
            "stdio",
            "-monitor",
            "none",
            *(() if args.expect_power == "reboot" else ("-no-reboot",)),
            *(() if args.expect_power == "shutdown" else ("-no-shutdown",)),
            "-cdrom",
            args.iso,
        ]
    )
    if qmp_socket_path is not None:
        command.extend(["-qmp", f"unix:{qmp_socket_path},server=on,wait=off"])

    dns_server = DnsServer() if args.expect_network else None
    echo_server = EchoServer() if args.expect_network else None
    http_server = HttpServer() if args.expect_network else None
    ntp_server = NtpServer() if args.expect_network else None
    uses_serial_input = (
        args.expect_calc
        or args.expect_shell
        or args.expect_network
        or args.expect_storage_boot
        or args.expect_power
        or (args.expect_input and not use_qmp_keyboard)
    )
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE if uses_serial_input else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    if relay is not None:
        relay.Start()
    captured = bytearray()
    # What the harness's servers had seen when the link went down and before
    # it came back up.
    traffic_marks = []
    timeout = (
        SHELL_TIMEOUT_SECONDS
        if args.expect_shell
        or args.expect_network
        or args.expect_storage_boot
        or args.expect_power
        else QEMU_TIMEOUT_SECONDS
    )
    deadline = time.monotonic() + timeout
    input_sent = False
    raw_keys_sent = False
    prompt_redrawn = False
    qmp_command_id = 2
    pending_lines = []
    if args.expect_calc:
        pending_lines = CALC_INPUT.splitlines(keepends=True)
    elif args.expect_shell:
        pending_lines = (
            SHELL_INPUT.splitlines(keepends=True)
            + list(SHELL_EDIT_INPUT)
            + list(SHELL_VI_INPUT)
            + list(SHELL_TOP_INPUT)
            + list(SHELL_KEYS_INPUT)
            + list(SHELL_INTERRUPT_INPUT)
        )
    elif args.expect_network:
        pending_lines = (
            NETWORK_SHELL_INPUT.replace("{port}", str(dns_server.port))
            .replace("{echo_port}", str(echo_server.port))
            .replace("{lossy_echo_port}", str(lossy_echo_server.port))
            .replace("{udp_echo_port}", str(echo_server.udp_port))
            .replace("{http_port}", str(http_server.port))
            .replace("{ntp_port}", str(ntp_server.port))
            .replace("{ntp_kiss_port}", str(ntp_server.kiss_port))
            # Nothing listens on a port that was just free.
            .replace("{closed_port}", str(FreeTcpPort()))
            .encode()
            .splitlines(keepends=True)
        )

        def MarkTraffic():
            traffic_marks.append(
                (
                    dict(dns_server.queries),
                    dict(dns_server.stream_queries),
                    dict(echo_server.connections),
                    dict(echo_server.datagrams),
                    dict(lossy_echo_server.connections),
                    {
                        family: len(hosts)
                        for family, hosts in http_server.hosts.items()
                    },
                    dict(ntp_server.requests),
                )
            )
            return True

        def LeasedAgain():
            clean = ANSI_ESCAPE_BYTES.sub(b"", captured)
            return clean.count(DHCP_LEASED_MESSAGE) >= 2

        waits = {
            b"{wait_for_httpd_client}\n": WaitUntil(httpd_client.done.is_set),
            b"{wait_for_renewal}\n": WaitFor("dhcp: renewed"),
            b"{mark_traffic}\n": WaitUntil(MarkTraffic),
            b"{wait_for_new_lease}\n": WaitUntil(LeasedAgain),
        }
        # init's DHCP client leases in the background while the shell starts.
        pending_lines = [WaitFor("dhcp: leased")] + [
            waits.get(line, line) for line in pending_lines
        ]
    elif args.expect_storage_boot:
        pending_lines = STORAGE_SHELL_INPUT[
            args.expect_storage_boot
        ].splitlines(keepends=True)
    elif args.expect_power:
        pending_lines = [f"{args.expect_power}\n".encode()]
    lines_sent = 0
    markers_before_end = {}
    qmp_connection = None
    qmp_channel = None
    input_error = None
    if any(
        (
            args.expect_panic,
            args.expect_exception,
            args.expect_interrupt,
            args.expect_memory,
            args.expect_timer,
            args.expect_scheduler,
            args.expect_smp,
            args.expect_network,
            args.expect_storage_boot,
            args.expect_power,
            args.expect_user_preemption,
            args.expect_input,
            args.expect_calc,
            args.expect_shell,
        )
    ):
        if args.expect_panic:
            markers = (EXPECTED_PANIC.encode(),)
        elif args.expect_exception:
            markers = (EXPECTED_EXCEPTION[args.arch].encode(),)
        elif args.expect_interrupt:
            markers = (EXPECTED_INTERRUPT[args.arch].encode(),)
        elif args.expect_memory:
            markers = (EXPECTED_MEMORY.encode(),)
        elif args.expect_scheduler:
            markers = (EXPECTED_SCHEDULER.encode(),)
        elif args.expect_smp:
            markers = (EXPECTED_SMP.encode(), PANIC_MARKER.encode())
        elif args.expect_network:
            # The last command's last line.
            markers = (b"dns fec0::3", PANIC_MARKER.encode())
        elif args.expect_power:
            # A shutdown ends when QEMU exits; a reboot when the shell is back.
            markers = (PANIC_MARKER.encode(),)
        elif args.expect_storage_boot:
            markers = (
                STORAGE_SHELL_OUTPUT[args.expect_storage_boot][-1].encode(),
                PANIC_MARKER.encode(),
            )
        elif args.expect_user_preemption:
            markers = (EXPECTED_USER_PREEMPTION.encode(),)
        elif args.expect_input:
            markers = (EXPECTED_INPUT_KEYS[-1].encode(), PANIC_MARKER.encode())
        elif args.expect_calc:
            markers = (EXPECTED_CALC.encode(), PANIC_MARKER.encode())
        elif args.expect_shell:
            markers = (
                EXPECTED_SHELL_OUTPUT[-1].encode(),
                PANIC_MARKER.encode(),
            )
        else:
            markers = (EXPECTED_TIMER.encode(),)
    else:
        markers = (SHELL_BANNER.encode(), PANIC_MARKER.encode())
        # init's DHCP client leases beside the shell, so its log can come
        # after the banner.
        pending_lines = [WaitFor(NETWORK_DHCP_MESSAGE)]

    output_closed = False
    timed_out = False
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            timed_out = True
            break
        readable, _, _ = select.select(
            [process.stdout], [], [], min(0.1, remaining)
        )
        waited = (
            pending_lines
            and isinstance(pending_lines[0], WaitUntil)
            and pending_lines[0].ready()
        )
        if waited:
            pending_lines.pop(0)
        if not readable and not waited:
            continue

        if readable:
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                output_closed = True
                break
            captured.extend(chunk)
        clean_output = ANSI_ESCAPE_BYTES.sub(
            b"", captured.replace(LEAVE_ALTERNATE_SCREEN, b"\n")
        )
        prompt_count = len(LINE_PROMPT.findall(clean_output))
        if (
            forwarded_client is not None
            and f"nc -l {FORWARDED_GUEST_PORT}".encode() in clean_output
        ):
            forwarded_client.Start()
        if httpd_client is not None and b"httpd /tmp/www" in clean_output:
            httpd_client.Start()
        try:
            while (
                pending_lines
                and isinstance(pending_lines[0], (TypeWhen, WaitFor))
                and pending_lines[0].text in clean_output
            ):
                line = pending_lines.pop(0)
                if isinstance(line, TypeWhen):
                    process.stdin.write(line.keys)
                    process.stdin.flush()
            while (
                pending_lines
                and not isinstance(
                    pending_lines[0], (TypeWhen, WaitFor, WaitUntil)
                )
                and lines_sent < prompt_count
            ):
                line = pending_lines.pop(0)
                pieces = (
                    [line]
                    if isinstance(line, WholeWrite)
                    else [bytes((character,)) for character in line]
                )
                for piece in pieces:
                    process.stdin.write(piece)
                    process.stdin.flush()
                    time.sleep(0.02)
                lines_sent += 1
                markers_before_end = {
                    marker: clean_output.count(marker) for marker in markers
                }
                while pending_lines and pending_lines[0] is INTERRUPT_RUNNING:
                    pending_lines.pop(0)
                    time.sleep(INTERRUPT_DELAY_SECONDS)
                    process.stdin.write(CONTROL_C)
                    process.stdin.flush()
        except OSError as error:
            input_error = str(error)
            pending_lines = []
        try:
            # The DHCP client's lease messages would land in the middle of
            # the typed line's echo, so typing waits for them and a prompt
            # after them, pressing Return for one if the shell drew its prompt
            # first.
            lease = clean_output.find(DHCP_LEASED_MESSAGE)
            if args.expect_input and not input_sent and lease >= 0:
                if qmp_channel is None:
                    qmp_connection, qmp_channel = OpenQmpChannel(
                        qmp_socket_path
                    )
                if SHELL_PROMPT.search(clean_output, lease):
                    qmp_command_id = SendQmpKeys(
                        qmp_channel, qmp_command_id, INPUT_LINE_KEYS
                    )
                    input_sent = True
                elif not prompt_redrawn and SHELL_PROMPT.search(clean_output):
                    qmp_command_id = SendQmpKeys(
                        qmp_channel, qmp_command_id, ("ret",)
                    )
                    prompt_redrawn = True
            if (
                args.expect_input
                and not raw_keys_sent
                and KEYS_READY.encode() in clean_output
            ):
                SendQmpKeys(qmp_channel, qmp_command_id, INPUT_RAW_KEYS)
                raw_keys_sent = True
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            input_error = str(error)
            input_sent = True
            raw_keys_sent = True
        # The end marker may also appear earlier in a scripted session, so
        # only a new one after the last typed line ends it. Counting copies
        # holds even when stripping escape codes shifts earlier output.
        if not pending_lines and any(
            clean_output.count(marker) > markers_before_end.get(marker, 0)
            for marker in markers
        ):
            break
        if (
            args.expect_power == "reboot"
            and clean_output.count(SHELL_BANNER.encode()) >= 2
        ):
            break

    # QEMU closes its output as it exits, which can come before the
    # process can be reaped.
    exited_by_itself = output_closed or process.poll() is not None
    # Missing output may mean the time ran out rather than the guest hanging.
    limit_note = f" (stopped at the {timeout} s limit)" if timed_out else ""
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    captured.extend(process.stdout.read() or b"")
    if process.stdin is not None:
        process.stdin.close()
    if qmp_channel is not None:
        qmp_channel.close()
    if qmp_connection is not None:
        qmp_connection.close()
    if qmp_directory is not None:
        qmp_directory.cleanup()
    output = captured.decode("utf-8", errors="replace")

    output = ANSI_ESCAPE.sub("", output)
    # The whole session, so a failure can be read without running again.
    output_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "build",
        args.arch,
        "smoke-output.txt",
    )
    with open(output_path, "w", encoding="utf-8") as saved:
        saved.write(output)
    serial_lines = [line for line in output.splitlines() if "KERN/" in line]
    if serial_lines:
        print("\n".join(serial_lines))
    if args.cpu == "max" and MAX_CPU_PROTECTIONS[args.arch] not in output:
        print(
            "FAIL: kernel did not enable the protections of QEMU's max CPU",
            file=sys.stderr,
        )
        return 1
    if args.expect_panic:
        if EXPECTED_PANIC not in output:
            print(
                "FAIL: expected panic record was not observed", file=sys.stderr
            )
            return 1
        print("PASS: expected panic was reported over serial")
        return 0
    if args.expect_exception:
        if EXPECTED_EXCEPTION[args.arch] not in output:
            print(
                "FAIL: expected CPU exception was not reported",
                file=sys.stderr,
            )
            return 1
        print("PASS: expected CPU exception was reported over serial")
        return 0
    if args.expect_interrupt:
        if EXPECTED_INTERRUPT[args.arch] not in output:
            print(
                "FAIL: expected hardware interrupt was not acknowledged",
                file=sys.stderr,
            )
            return 1
        print("PASS: hardware interrupt was acknowledged over serial")
        return 0
    if args.expect_memory:
        if EXPECTED_MEMORY not in output:
            print("FAIL: memory tests did not report success", file=sys.stderr)
            return 1
        print("PASS: memory tests reported success over serial")
        return 0
    if args.expect_timer:
        if EXPECTED_TIMER not in output:
            print(
                "FAIL: periodic timer test did not report success",
                file=sys.stderr,
            )
            return 1
        print("PASS: periodic timer test reported success over serial")
        return 0
    if args.expect_scheduler:
        if EXPECTED_SCHEDULER not in output:
            failure_message = "FAIL: scheduler preemption test did not pass"
            print(failure_message, file=sys.stderr)
            return 1
        print("PASS: timer preemption and task lifecycle passed")
        return 0
    if args.expect_power:
        expected = POWER_MESSAGES[args.expect_power]
        missing = [text for text in expected if text not in output]
        boots = output.count(
            "KERN/I: Limine transferred control to the kernel"
        )
        if missing or PANIC_MARKER in output:
            print(
                f"FAIL: power output is missing {missing}{limit_note}",
                file=sys.stderr,
            )
            return 1
        if args.expect_power == "shutdown" and not exited_by_itself:
            print("FAIL: QEMU kept running after shutdown", file=sys.stderr)
            return 1
        if args.expect_power == "reboot" and boots < 2:
            print("FAIL: the system did not boot again", file=sys.stderr)
            return 1
        outcome = (
            "powered off"
            if args.expect_power == "shutdown"
            else "booted again"
        )
        print(
            f"PASS: {args.expect_power} stopped processes, flushed the disk, "
            f"and {outcome}"
        )
        return 0
    if args.expect_storage_boot:
        expected = f"{STORAGE_MESSAGE}{args.expect_storage_boot}"
        if (
            re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
            is None
        ):
            print(f"FAIL: expected '{expected}'", file=sys.stderr)
            return 1
        missing = [
            text
            for text in STORAGE_SHELL_OUTPUT[args.expect_storage_boot]
            if text not in output
        ]
        if missing or NOTES_OUTPUT.search(output) is None:
            print(
                "FAIL: file tools output is missing "
                f"{missing or 'the cat output'}{limit_note}",
                file=sys.stderr,
            )
            return 1
        disk_error = VerifyDiskImage(args.disk, args.expect_storage_boot)
        if disk_error is not None:
            print(f"FAIL: disk image check: {disk_error}", file=sys.stderr)
            return 1
        print(
            "PASS: sector, FAT16, and shell file-tool writes verified in the "
            f"guest and with mtools; boot {args.expect_storage_boot} persisted"
        )
        return 0
    if args.expect_smp:
        if EXPECTED_SMP not in output:
            print(
                "FAIL: multi-CPU scheduling test did not pass", file=sys.stderr
            )
            return 1
        print("PASS: tasks ran concurrently on several CPUs")
        return 0
    if dns_server is not None:
        dns_server.Close()
    if echo_server is not None:
        echo_server.Close()
    if http_server is not None:
        http_server.Close()
    if ntp_server is not None:
        ntp_server.Close()
    if lossy_echo_server is not None:
        lossy_echo_server.Close()
    if relay is not None:
        relay.Close()
    if args.expect_network:
        if EXPECTED_ARP not in output:
            print("FAIL: ARP did not resolve the gateway", file=sys.stderr)
            return 1
        if EXPECTED_NETWORK not in output:
            print(
                "FAIL: neighbor discovery did not resolve fec0::2",
                file=sys.stderr,
            )
            return 1
        missing = [
            line
            for line in EXPECTED_NETWORK_SHELL_OUTPUT
            if line not in output
        ]
        if missing:
            print(
                f"FAIL: network shell output is missing {missing[0]!r}"
                f"{limit_note}",
                file=sys.stderr,
            )
            return 1
        for family, version in (
            (socket.AF_INET, "IPv4"),
            (socket.AF_INET6, "IPv6"),
        ):
            if dns_server.queries[family] == 0:
                print(
                    f"FAIL: no DNS query reached the test server over "
                    f"{version}",
                    file=sys.stderr,
                )
                return 1
            if dns_server.stream_queries[family] == 0:
                print(
                    f"FAIL: no DNS query reached the test server over TCP "
                    f"and {version}",
                    file=sys.stderr,
                )
                return 1
            if echo_server.connections[family] == 0:
                print(
                    f"FAIL: no TCP connection reached the echo server over "
                    f"{version}",
                    file=sys.stderr,
                )
                return 1
            if echo_server.datagrams[family] == 0:
                print(
                    f"FAIL: no UDP datagram reached the echo server over "
                    f"{version}",
                    file=sys.stderr,
                )
                return 1
            if ntp_server.requests[family] == 0:
                print(
                    f"FAIL: no NTP request reached the test server over "
                    f"{version}",
                    file=sys.stderr,
                )
                return 1
        down_start = output.rfind(LINK_DOWN_MESSAGE)
        down_end = output.find(LINK_UP_MESSAGE, max(down_start, 0))
        if down_start < 0 or down_end < 0:
            print(
                f"FAIL: net down and net up did not both log{limit_note}",
                file=sys.stderr,
            )
            return 1
        down_output = output[down_start:down_end]
        up_output = output[down_end + len(LINK_UP_MESSAGE) :]
        ports = {
            "{echo_port}": str(echo_server.port),
            "{lossy_echo_port}": str(lossy_echo_server.port),
            "{http_port}": str(http_server.port),
        }
        for expected, section, state in (
            (EXPECTED_LINK_DOWN_OUTPUT, down_output, "down"),
            (EXPECTED_LINK_UP_OUTPUT, up_output, "up"),
        ):
            for line in expected:
                for placeholder, port in ports.items():
                    line = line.replace(placeholder, port)
                if line not in section:
                    print(
                        f"FAIL: with the link {state}, the output is missing "
                        f"{line!r}{limit_note}",
                        file=sys.stderr,
                    )
                    return 1
        if (
            down_output.count("ping: sendto: Network is down") != 2
            or "ipv6 fe80::" in down_output
        ):
            print(
                "FAIL: with the link down, a ping went out or IPv6 stayed",
                file=sys.stderr,
            )
            return 1
        if len(traffic_marks) != 2 or traffic_marks[0] != traffic_marks[1]:
            print(
                f"FAIL: the harness's servers saw traffic while the link was "
                f"down: {traffic_marks}",
                file=sys.stderr,
            )
            return 1
        for pattern in EXPECTED_NTP_PATTERNS:
            if re.search(pattern, output, re.MULTILINE) is None:
                print(
                    f"FAIL: network shell output does not match {pattern!r}"
                    f"{limit_note}",
                    file=sys.stderr,
                )
                return 1
        for family, host in (
            (socket.AF_INET, f"10.0.2.2:{http_server.port}"),
            (socket.AF_INET6, f"[fec0::2]:{http_server.port}"),
            (socket.AF_INET6, f"gateway.featheros.test:{http_server.port}"),
        ):
            if host not in http_server.hosts[family]:
                print(
                    f"FAIL: no HTTP request for Host {host} reached the "
                    f"test server: {http_server.hosts}",
                    file=sys.stderr,
                )
                return 1
        counts = {}
        for name, count in re.findall(
            r"cat (\S+) [^\r\n]*\| wc -c\r?\n(\d+)\r?$", output, re.MULTILINE
        ):
            counts.setdefault("bulk" if name == "/tmp/b" else "vi", []).append(
                count
            )
        for name, expected in (("vi", 5), ("bulk", 3)):
            found = counts.get(name, [])
            if len(found) != expected or len(set(found)) != 1:
                print(
                    f"FAIL: {name} byte counts sent and echoed back differ: "
                    f"{found}{limit_note}",
                    file=sys.stderr,
                )
                return 1
        for family, version in (
            (socket.AF_INET, "IPv4"),
            (socket.AF_INET6, "IPv6"),
        ):
            for toward_guest, direction in ((False, "from"), (True, "to")):
                key = (family, toward_guest)
                dropped = relay.dropped.get(key, 0)
                reordered = relay.reordered.get(key, 0)
                if dropped != len(LossyRelay.DROPPED) or reordered != 1:
                    print(
                        f"FAIL: the relay dropped {dropped} and reordered "
                        f"{reordered} TCP segments {direction} the guest over "
                        f"{version}{limit_note}",
                        file=sys.stderr,
                    )
                    return 1
        if forwarded_client.received != b"from guest\n":
            print(
                "FAIL: the guest's server sent "
                f"{forwarded_client.received!r} through port forwarding",
                file=sys.stderr,
            )
            return 1
        if not httpd_client.done.is_set() or httpd_client.failure:
            print(
                "FAIL: the guest's httpd through port forwarding: "
                f"{httpd_client.failure or 'the client did not finish'}",
                file=sys.stderr,
            )
            return 1
        print(
            "PASS: ARP and neighbor discovery resolved their hosts; DHCP, "
            "DNS over IPv4 and IPv6, pings by name, TCP clients and servers, "
            "HTTP clients and servers, and NTP worked"
        )
        return 0
    if args.expect_calc:
        if EXPECTED_CALC not in output:
            print(
                "FAIL: calculator command result was not observed",
                file=sys.stderr,
            )
            return 1
        if output.count(EXPECTED_UNKNOWN_COMMAND) != 1:
            print(
                "FAIL: expected exactly one unknown-command message",
                file=sys.stderr,
            )
            return 1
        print("PASS: unknown command rejected; calculator returned 69.25")
        return 0
    if args.expect_shell:
        # The shell exits with one of these when it cannot read a command.
        read_failure = re.search(
            r"^shell: (?:lost the console|could not read input).*$",
            output,
            re.MULTILINE,
        )
        if read_failure:
            print(f"FAIL: {read_failure.group(0).strip()}", file=sys.stderr)
            return 1
        missing = [
            text for text in EXPECTED_SHELL_OUTPUT if text not in output
        ]
        if missing:
            print(
                f"FAIL: shell output is missing {missing}{limit_note}",
                file=sys.stderr,
            )
            return 1
        edits = {
            text: len(re.findall(rf"^{text}\r?$", output, re.MULTILINE))
            for text in EXPECTED_SHELL_EDITS
        }
        if edits != EXPECTED_SHELL_EDITS or not EXPECTED_HISTORY_LINE.search(
            output
        ):
            print(f"FAIL: line editing printed {edits}", file=sys.stderr)
            return 1
        vi_lines = {
            text: len(re.findall(rf"^{text}\r?$", output, re.MULTILINE))
            for text in EXPECTED_VI_LINES
        }
        if vi_lines != EXPECTED_VI_LINES:
            print(f"FAIL: vi wrote {vi_lines}", file=sys.stderr)
            return 1
        missing_tools = [
            pattern
            for pattern in EXPECTED_TEXT_TOOLS
            if not re.search(pattern, output, re.MULTILINE)
        ]
        if missing_tools:
            print(
                f"FAIL: text tools did not print {missing_tools}",
                file=sys.stderr,
            )
            return 1
        if CLEAR_SEQUENCE not in captured:
            print("FAIL: clear did not clear the screen", file=sys.stderr)
            return 1
        keys = [
            line.strip()
            for line in output.splitlines()
            if line.startswith("keys: ")
        ]
        if keys != EXPECTED_KEYS:
            print(f"FAIL: keys reported {keys}", file=sys.stderr)
            return 1
        # memtest exhaust runs twice from /system and once from the disk.
        exhausted = [int(pages) for pages in EXHAUST_OUTPUT.findall(output)]
        if (
            len(exhausted) != 3
            or len(set(exhausted)) != 1
            or exhausted[0] < MINIMUM_EXHAUST_PAGES
        ):
            print(
                f"FAIL: page-limit exhaustion runs reported {exhausted}",
                file=sys.stderr,
            )
            return 1
        # aslr runs twice from /system and twice from /tmp.
        layouts = ASLR_OUTPUT.findall(output)
        if len(layouts) != 4 or any(
            len(set(field)) != len(field) for field in zip(*layouts)
        ):
            print(
                f"FAIL: aslr runs did not get distinct layouts: {layouts}",
                file=sys.stderr,
            )
            return 1
        busy = [
            int(percent)
            for percent in re.findall(
                r"^\s*\d+ R\s+(\d+)\.\d\s+\d+\s+\S+ primes\r?$",
                output,
                re.MULTILINE,
            )
        ]
        if not busy or max(busy) < 50:
            print(
                f"FAIL: top did not show a busy primes worker: {busy}",
                file=sys.stderr,
            )
            return 1
        # primes 4 and primes 16 report one line per worker.
        if len(PRIMES_WORKER_OUTPUT.findall(output)) != 20:
            print("FAIL: primes did not report every worker", file=sys.stderr)
            return 1
        if len(THREADS_WORKER_OUTPUT.findall(output)) != 19:
            print("FAIL: threads did not report every thread", file=sys.stderr)
            return 1
        # A prompt may precede the echo on the same line.
        interrupts = len(re.findall(r"\^C\r?$", output, re.MULTILINE))
        if (
            interrupts != EXPECTED_INTERRUPTS
            or "primes below 1000000000" in output
            or "slept 100 s" in output
        ):
            print(
                f"FAIL: Ctrl+C was echoed {interrupts} times, expected "
                f"{EXPECTED_INTERRUPTS}, or an interrupted command finished",
                file=sys.stderr,
            )
            return 1
        if WORKING_DIRECTORY_OUTPUT.findall(output) != ["/home/work", "/home"]:
            print(
                "FAIL: pwd did not follow cd: "
                f"{WORKING_DIRECTORY_OUTPUT.findall(output)}",
                file=sys.stderr,
            )
            return 1
        if len(WORK_FILE_OUTPUT.findall(output)) != 2:
            print(
                "FAIL: a file was not readable through a relative path",
                file=sys.stderr,
            )
            return 1
        if UPTIME_OUTPUT.search(output) is None:
            print("FAIL: uptime did not print H:MM:SS", file=sys.stderr)
            return 1
        date_match = DATE_OUTPUT.search(output)
        if date_match is None:
            print("FAIL: date did not print a timestamp", file=sys.stderr)
            return 1
        guest_time = datetime.datetime.strptime(
            date_match.group(1), "%Y-%m-%d %H:%M:%S"
        ).replace(tzinfo=datetime.timezone.utc)
        skew = abs(
            datetime.datetime.now(datetime.timezone.utc) - guest_time
        ).total_seconds()
        if skew > MAXIMUM_CLOCK_SKEW_SECONDS:
            print(
                f"FAIL: date printed {date_match.group(1)} UTC, "
                f"{skew:.0f} seconds from the host clock",
                file=sys.stderr,
            )
            return 1
        print(
            "PASS: shell ran programs with arguments, foreground input, "
            f"uptime, Ctrl+C, and date ({date_match.group(1)} UTC)"
        )
        return 0
    if args.expect_user_preemption:
        if EXPECTED_USER_PREEMPTION not in output:
            print(
                "FAIL: user-mode timer preemption did not pass",
                file=sys.stderr,
            )
            return 1
        if USER_PREEMPTION_OUTPUTS[args.arch] not in captured:
            print(
                "FAIL: user-program preemption did not interleave",
                file=sys.stderr,
            )
            return 1
        print("PASS: user programs interleaved under timer preemption")
        return 0
    if args.expect_input:
        if input_error is not None:
            print(
                f"FAIL: could not inject QEMU keyboard input: {input_error}",
                file=sys.stderr,
            )
            return 1
        if "Unknown command: ab" not in output:
            print(
                "FAIL: the shell did not run the typed line", file=sys.stderr
            )
            return 1
        if len(re.findall(r"^azb\r?$", output, re.MULTILINE)) != 2:
            print(
                "FAIL: the cursor keys and history did not edit the line",
                file=sys.stderr,
            )
            return 1
        if INPUT_ECHO_SEQUENCE not in captured:
            print(
                "FAIL: terminal input echo/backspace sequence was incorrect",
                file=sys.stderr,
            )
            return 1
        if b"> ^C" not in captured:
            print("FAIL: Ctrl+C was not echoed at the prompt", file=sys.stderr)
            return 1
        keys = [
            line.strip()
            for line in output.splitlines()
            if line.startswith("keys: ")
        ]
        if keys != EXPECTED_INPUT_KEYS:
            print(f"FAIL: keys reported {keys}", file=sys.stderr)
            return 1
        print(
            "PASS: keyboard input, Ctrl+C, backspace, Enter, and raw special "
            "keys were handled"
        )
        return 0
    if PANIC_MARKER in output:
        print("FAIL: kernel panicked during startup", file=sys.stderr)
        return 1
    if USER_MODE_MESSAGES[args.arch] not in output:
        failure = f"FAIL: {args.arch} user-mode test did not complete"
        print(failure, file=sys.stderr)
        return 1
    if USER_MODE_OUTPUTS[args.arch] not in captured:
        print(
            "FAIL: user-mode console syscall output was not observed",
            file=sys.stderr,
        )
        return 1
    if READY_MESSAGE not in output:
        print(
            "FAIL: kernel did not report ready within "
            f"{QEMU_TIMEOUT_SECONDS}s",
            file=sys.stderr,
        )
        diagnostics = [
            line.strip()
            for line in output.splitlines()
            if "error:" in line.lower() or "error " in line.lower()
        ]
        for line in diagnostics[-8:]:
            print(line[:240], file=sys.stderr)
        return 1
    if TIMER_STARTED_MESSAGE not in output:
        print("FAIL: production timer did not start", file=sys.stderr)
        return 1
    if SMP_MESSAGE not in output:
        print(
            f"FAIL: kernel did not start all {QEMU_CPU_COUNT} CPUs",
            file=sys.stderr,
        )
        return 1
    if NETWORK_MESSAGE not in output:
        print("FAIL: the virtio-net device did not come up", file=sys.stderr)
        return 1
    if NETWORK_IPV6_MESSAGE not in output:
        print("FAIL: IPv6 autoconfiguration did not finish", file=sys.stderr)
        return 1
    if NETWORK_DHCP_MESSAGE not in output:
        print("FAIL: DHCP did not configure IPv4", file=sys.stderr)
        return 1
    serial_line_set = set(output.splitlines())
    missing_cpus = [
        cpu
        for cpu in range(1, QEMU_CPU_COUNT)
        if f"KERN/I: CPU {cpu} online" not in serial_line_set
    ]
    if missing_cpus:
        print(
            f"FAIL: CPUs {missing_cpus} did not report a complete online line",
            file=sys.stderr,
        )
        return 1
    if VFS_MESSAGE not in output:
        print(
            "FAIL: kernel did not read a file through the VFS", file=sys.stderr
        )
        return 1
    if FAT16_MESSAGE not in output:
        print(
            "FAIL: kernel did not read a file from the FAT16 partition",
            file=sys.stderr,
        )
        return 1
    if ELF_LOAD_MESSAGE not in output:
        print(
            "FAIL: kernel did not load the ELF into a user address space",
            file=sys.stderr,
        )
        return 1
    if SHELL_BANNER not in output:
        print("FAIL: the shell did not start", file=sys.stderr)
        return 1
    if EXPECTED_ARCHITECTURE[args.arch] not in output:
        print("FAIL: kernel did not report its architecture", file=sys.stderr)
        return 1
    if (
        re.search(
            r"KERN/I: Memory map coverage: \d+ MiB, usable RAM: \d+ MiB",
            output,
        )
        is None
    ):
        print(
            "FAIL: kernel did not report total and usable memory",
            file=sys.stderr,
        )
        return 1

    print("PASS: kernel reached the ready state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
