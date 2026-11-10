# A scripted session is a list of these items and plain byte lines; a plain
# line is typed once a prompt asks for it.

CONTROL_C = b"\x03"
CONTROL_Z = b"\x1a"
# Ctrl+C for a command that shows no prompt of its own, sent while it runs.
INTERRUPT_RUNNING = object()
# Ctrl+Z, sent the same way.
SUSPEND_RUNNING = object()


class TypeWhen:
    """Keys typed once text appears, for a program that shows no prompt."""

    def __init__(self, text, keys):
        self.text = text.encode()
        self.keys = keys


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


def Lines(text):
    return text.splitlines(keepends=True)
