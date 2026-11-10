import re

ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
ANSI_ESCAPE_BYTES = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|[@-_])")
# A program leaving the alternate screen returns to the shell's line, as a
# newline would, which prompt detection relies on.
LEAVE_ALTERNATE_SCREEN = b"\x1b[?1049l"
KEYS_READY = "keys: press keys; q quits"
# The shell's prompt shows its working directory, such as "/home> ".
SHELL_PROMPT = re.compile(rb"^/[^\n>]*> ", re.MULTILINE)
# Each scripted shell line is typed only once a prompt asks for it, so
# typed-ahead input never overflows the kernel's input queue.
LINE_PROMPT = re.compile(
    rb"^(?:(?:upper|calc|/[^\n>]*)?> |keys: press keys; q quits\r?$)",
    re.MULTILINE,
)
# A kernel built for the keyboard test reports, on serial only, which console
# is shown and each console's foreground program whenever they change.
CONSOLE_REPORT_PREFIX = b"KERN/I: Consoles: "
CONSOLE_REPORT = re.compile(rb"KERN/I: Consoles: [^\r\n]*\r?\n")
CONSOLE_REPORT_TEXT = re.compile(r"KERN/I: Consoles: [^\r\n]*\r?\n")
