import datetime
import os
import select
import subprocess
import time

from smoke.script import (
    CONTROL_C,
    CONTROL_Z,
    INTERRUPT_RUNNING,
    SUSPEND_RUNNING,
    TypeWhen,
    WaitFor,
    WaitUntil,
)
from smoke.terminal import (
    ANSI_ESCAPE,
    ANSI_ESCAPE_BYTES,
    CONSOLE_REPORT,
    LEAVE_ALTERNATE_SCREEN,
    LINE_PROMPT,
)

CONTROL_KEYS = {INTERRUPT_RUNNING: CONTROL_C, SUSPEND_RUNNING: CONTROL_Z}
# Ctrl+C or Ctrl+Z for a running command waits this long after the shell
# echoed the command's newline, so the command has started and holds the
# terminal; sent earlier, it would reach the shell instead.
COMMAND_START_SECONDS = 0.5


class Session:
    def __init__(self, process):
        self.process = process
        self.captured = bytearray()


class Result:
    def __init__(self, arch, captured, started, exited_by_itself, limit_note):
        self.arch = arch
        self.captured = bytes(captured)
        self.output = ANSI_ESCAPE.sub(
            "", self.captured.decode("utf-8", errors="replace")
        )
        self.started = started
        self.exited_by_itself = exited_by_itself
        self.limit_note = limit_note


def Type(process, keys):
    process.stdin.write(keys)
    process.stdin.flush()


def Run(command, case, arch, session_path):
    started = datetime.datetime.now(datetime.timezone.utc)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE if case.serial_input else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    case.Launched()
    session = Session(process)
    captured = session.captured
    pending_lines = list(case.Script(session))
    markers = case.Markers()
    deadline = time.monotonic() + case.timeout
    lines_sent = 0
    markers_before_end = {}
    # Where the output stood when the line before a pending Ctrl+C or Ctrl+Z
    # was typed, and when that key is due once the line's echo ended.
    control_from = None
    control_due = None
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
        try:
            if control_due is not None and time.monotonic() >= control_due:
                Type(process, CONTROL_KEYS[pending_lines.pop(0)])
                control_from = None
                control_due = None
        except OSError:
            pending_lines = []
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
        if (
            control_from is not None
            and control_due is None
            and b"\n" in captured[control_from:]
        ):
            control_due = time.monotonic() + COMMAND_START_SECONDS
        clean_output = ANSI_ESCAPE_BYTES.sub(
            b"",
            CONSOLE_REPORT.sub(b"", captured).replace(
                LEAVE_ALTERNATE_SCREEN, b"\n"
            ),
        )
        prompt_count = len(LINE_PROMPT.findall(clean_output))
        case.OnOutput(session, clean_output)
        try:
            while (
                pending_lines
                and isinstance(pending_lines[0], (TypeWhen, WaitFor))
                and pending_lines[0].text in clean_output
            ):
                line = pending_lines.pop(0)
                if isinstance(line, TypeWhen):
                    Type(process, line.keys)
            # A whole line at a time: QEMU takes bytes from the pipe only as
            # fast as the guest's UART has room, and a prompt asks for each.
            while (
                pending_lines
                and isinstance(pending_lines[0], bytes)
                and lines_sent < prompt_count
            ):
                Type(process, pending_lines.pop(0))
                lines_sent += 1
                markers_before_end = {
                    marker: clean_output.count(marker) for marker in markers
                }
                if pending_lines and pending_lines[0] in CONTROL_KEYS:
                    control_from = len(captured)
                    break
        except OSError:
            pending_lines = []
        # The end marker may also appear earlier in a scripted session, so
        # only a new one after the last typed line ends it. Counting copies
        # holds even when stripping escape codes shifts earlier output.
        if not pending_lines and any(
            clean_output.count(marker) > markers_before_end.get(marker, 0)
            for marker in markers
        ):
            break
        if case.Finished(clean_output):
            break

    # QEMU closes its output as it exits, which can come before the
    # process can be reaped.
    exited_by_itself = output_closed or process.poll() is not None
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
    # Missing output may mean the time ran out rather than the guest hanging.
    limit_note = (
        f" (stopped at the {case.timeout} s limit)" if timed_out else ""
    )
    result = Result(arch, captured, started, exited_by_itself, limit_note)
    # The whole session, so a failure can be read without running again.
    with open(session_path, "w", encoding="utf-8") as saved:
        saved.write(result.output)
    return result
