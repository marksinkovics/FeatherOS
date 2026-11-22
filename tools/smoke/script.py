# A scripted session is a list of these items and plain byte lines; a plain
# line is typed once a prompt asks for it.

CONTROL_C = b"\x03"
CONTROL_Z = b"\x1a"
# Ctrl+C for a command that shows no prompt of its own, sent while it runs.
# After a WaitFor it is sent at once, else shortly after the command's echo.
INTERRUPT_RUNNING = object()
# Ctrl+Z, sent the same way.
SUSPEND_RUNNING = object()


class TypeWhen:
    """Keys typed once text appears, for a program that shows no prompt. A
    fresh one waits for a copy that appears after the line before it was
    typed, not for one the session already holds."""

    def __init__(self, text, keys, fresh=False):
        self.text = text.encode()
        self.keys = keys
        self.fresh = fresh


class WaitFor:
    """Holds back the lines after it until text appears, for a background
    program that reports on its own."""

    def __init__(self, text, fresh=False):
        self.text = text.encode()
        self.fresh = fresh


class WaitUntil:
    """Holds back the lines after it until ready() is true, for work outside
    the guest that later lines depend on."""

    def __init__(self, ready):
        self.ready = ready


def Lines(text):
    return text.splitlines(keepends=True)
