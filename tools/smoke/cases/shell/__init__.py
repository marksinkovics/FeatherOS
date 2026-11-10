import re

from smoke.boot import PANIC_MARKER
from smoke.build_tree import ProgramBuilt
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.shell import (
    calculator,
    cppbench,
    editing,
    files,
    interrupt,
    job_control,
    jobs,
    keys,
    limits,
    ping,
    pipes,
    programs,
    restart,
    script_command,
    swift,
    swift_concurrency,
    swift_cxx,
    swift_full,
    text_tools,
    top,
    vi,
    workers,
)

# In the order they are typed; later parts rely on files and directories that
# earlier ones made. interrupt's last line must stay last, since its output
# ends the session.
PARTS = (
    swift.PART,
    swift_concurrency.PART,
    swift_cxx.PART,
    swift_full.PART,
    programs.PART,
    files.PART,
    ping.PART,
    text_tools.PART,
    pipes.PART,
    jobs.PART,
    restart.PART,
    limits.PART,
    workers.PART,
    cppbench.PART,
    calculator.PART,
    editing.PART,
    vi.PART,
    top.PART,
    keys.PART,
    script_command.PART,
    job_control.PART,
    interrupt.PART,
)
# The shell exits with one of these when it cannot read a command.
READ_FAILURE = re.compile(
    r"^shell: (?:lost the console|could not read input).*$", re.MULTILINE
)


class Shell(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.parts = []

    def Script(self, session):
        build_dir = self.args.build_dir
        swift_built = ProgramBuilt(
            build_dir, self.args.arch, swift.PART.requires
        )
        script = []
        for part in PARTS:
            if part.requires is not None and not ProgramBuilt(
                build_dir, self.args.arch, part.requires
            ):
                if part is swift.PART or swift_built:
                    print(f"NOTE: {part.missing_note}")
                continue
            self.parts.append(part)
            script += part.script
        return script

    def Markers(self):
        return (interrupt.END_MARKER.encode(), PANIC_MARKER.encode())

    def Check(self, result):
        output = result.output
        read_failure = READ_FAILURE.search(output)
        if read_failure:
            return read_failure.group(0).strip()
        missing = [
            text
            for part in self.parts
            for text in part.expected
            if text not in output
        ]
        if missing:
            return f"shell output is missing {missing}{result.limit_note}"
        for part in self.parts:
            failure = part.check(result) if part.check else None
            if failure is not None:
                return failure
        # A prompt may precede the echo on the same line.
        interrupts = len(re.findall(r"\^C\r?$", output, re.MULTILINE))
        expected_interrupts = sum(part.interrupts for part in self.parts)
        if (
            interrupts != expected_interrupts
            or "primes below 1000000000" in output
            or "slept 100 s" in output
        ):
            return (
                f"Ctrl+C was echoed {interrupts} times, expected "
                f"{expected_interrupts}, or an interrupted command finished"
            )
        suspends = len(re.findall(r"\^Z\r?$", output, re.MULTILINE))
        expected_suspends = sum(part.suspends for part in self.parts)
        if suspends != expected_suspends:
            return (
                f"Ctrl+Z was echoed {suspends} times, expected "
                f"{expected_suspends}"
            )
        return None

    def PassMessage(self, result):
        date = programs.DATE_OUTPUT.search(result.output).group(1)
        return (
            "shell ran programs with arguments, foreground input, "
            f"uptime, Ctrl+C, Ctrl+Z, and date ({date} UTC)"
        )
