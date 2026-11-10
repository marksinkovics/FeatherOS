import json
import os
import re
import tempfile

from smoke.boot import DHCP_LEASED_MESSAGE, PANIC_MARKER
from smoke.cases.base import INPUT_TIMEOUT_SECONDS, Case
from smoke.qemu import OpenQmpChannel, SendkeyNames, SendQmpKeys
from smoke.terminal import (
    CONSOLE_REPORT,
    CONSOLE_REPORT_PREFIX,
    CONSOLE_REPORT_TEXT,
    KEYS_READY,
    SHELL_PROMPT,
)

# Ctrl+C at the shell prompt only discards the line; "ab" is then an unknown
# command, and keys takes raw input from the same keyboard. What is typed
# while the second console is shown stays out of the first one's lines.
INPUT_LINE_KEYS = (
    "ctrl-c",
    "a",
    "b",
    "x",
    "backspace",
    "ret",
    "alt-f2",
    "q",
    "w",
    "alt-f1",
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
# Once keys has run, every console runs a job of its own: each step waits for
# a report, then sends its keys. Ctrl+C first drops the "qw" typed on the
# second console earlier; later Ctrl+Z stops only that console's sleep, fg
# continues it and Ctrl+C ends it. Nothing typed on the other consoles may
# reach serial.
CONSOLE_STEPS = (
    ("tty1 shown; keys, shell, shell, shell", ()),
    (
        "tty1 shown; shell, shell, shell, shell",
        ("alt-f2", "ctrl-c", *SendkeyNames("sleep 60\n")),
    ),
    (
        "tty2 shown; shell, sleep, shell, shell",
        ("alt-f3", *SendkeyNames("top\n")),
    ),
    (
        "tty3 shown; shell, sleep, top, shell",
        ("alt-f4", *SendkeyNames("vi\n")),
    ),
    ("tty4 shown; shell, sleep, top, vi", ("alt-f2", "ctrl-z")),
    ("tty2 shown; shell, shell, top, vi", SendkeyNames("fg\n")),
    ("tty2 shown; shell, sleep, top, vi", ("ctrl-c",)),
    ("tty2 shown; shell, shell, top, vi", ("alt-f1",)),
    (
        "tty1 shown; shell, shell, top, vi",
        SendkeyNames("echo consoles done | upper\n"),
    ),
)
CONSOLES_DONE = "CONSOLES DONE"
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
INPUT_ECHO_SEQUENCE = b"abx\x08 \x08\r\n"


class Keyboard(Case):
    """Types on QEMU's keyboard through QMP rather than on serial."""

    timeout = INPUT_TIMEOUT_SECONDS
    passed = (
        "keyboard input, Ctrl+C, backspace, Enter, raw special "
        "keys, and a job on each of four consoles were handled"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.directory.name, "qmp.sock")
        self.connection = None
        self.channel = None
        self.command_id = 2
        self.input_sent = False
        self.raw_keys_sent = False
        self.prompt_redrawn = False
        # Where in the output the next console report is looked for.
        self.console_position = 0
        self.console_step = 0
        self.input_error = None

    def QmpSocket(self):
        return self.socket_path

    def Markers(self):
        return (CONSOLES_DONE.encode(), PANIC_MARKER.encode())

    def SendKeys(self, keys):
        self.command_id = SendQmpKeys(self.channel, self.command_id, keys)

    def OnOutput(self, session, clean_output):
        try:
            # The DHCP client's lease messages would land in the middle of
            # the typed line's echo, so typing waits for them and a prompt
            # after them, pressing Return for one if the shell drew its prompt
            # first.
            lease = clean_output.find(DHCP_LEASED_MESSAGE)
            if not self.input_sent and lease >= 0:
                if self.channel is None:
                    self.connection, self.channel = OpenQmpChannel(
                        self.socket_path
                    )
                if SHELL_PROMPT.search(clean_output, lease):
                    self.SendKeys(INPUT_LINE_KEYS)
                    self.input_sent = True
                elif not self.prompt_redrawn and SHELL_PROMPT.search(
                    clean_output
                ):
                    self.SendKeys(("ret",))
                    self.prompt_redrawn = True
            if not self.raw_keys_sent and KEYS_READY.encode() in clean_output:
                self.SendKeys(INPUT_RAW_KEYS)
                self.raw_keys_sent = True
            while self.console_step < len(CONSOLE_STEPS):
                report, keys = CONSOLE_STEPS[self.console_step]
                found = session.captured.find(
                    CONSOLE_REPORT_PREFIX + report.encode() + b"\r\n",
                    self.console_position,
                )
                if found < 0:
                    break
                self.console_position = found + 1
                self.SendKeys(keys)
                self.console_step += 1
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            self.input_error = str(error)
            self.input_sent = True
            self.raw_keys_sent = True
            self.console_step = len(CONSOLE_STEPS)

    def Stop(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()
        self.directory.cleanup()

    def Check(self, result):
        if self.input_error is not None:
            return f"could not inject QEMU keyboard input: {self.input_error}"
        # The console reports are whole lines, which may split others.
        shell_output = CONSOLE_REPORT_TEXT.sub("", result.output)
        shell_captured = CONSOLE_REPORT.sub(b"", result.captured)
        if "Unknown command: ab" not in shell_output:
            return "the shell did not run the typed line"
        if len(re.findall(r"^azb\r?$", shell_output, re.MULTILINE)) != 2:
            return "the cursor keys and history did not edit the line"
        if INPUT_ECHO_SEQUENCE not in shell_captured:
            return "terminal input echo/backspace sequence was incorrect"
        if b"> ^C" not in shell_captured:
            return "Ctrl+C was not echoed at the prompt"
        keys = [
            line.strip()
            for line in shell_output.splitlines()
            if line.startswith("keys: ")
        ]
        if keys != EXPECTED_INPUT_KEYS:
            return f"keys reported {keys}"
        if (
            self.console_step < len(CONSOLE_STEPS)
            or CONSOLES_DONE not in result.output
        ):
            waited = (
                CONSOLE_STEPS[self.console_step][0]
                if self.console_step < len(CONSOLE_STEPS)
                else CONSOLES_DONE
            )
            return f"the consoles never reported '{waited}'{result.limit_note}"
        if "sleep 60" in result.output:
            return "keys typed on another console reached serial"
        return None
