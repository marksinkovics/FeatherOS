from smoke.boot import NETWORK_DHCP_MESSAGE, PANIC_MARKER, SHELL_BANNER
from smoke.script import WaitFor

QEMU_TIMEOUT_SECONDS = 10
# The scripted shell session runs many programs and types each character 20 ms
# apart; the whole session takes about 3 minutes on either architecture.
SHELL_TIMEOUT_SECONDS = 300
# The keyboard test switches through every console after its own keys.
INPUT_TIMEOUT_SECONDS = 60


class Case:
    """One mode of the smoke test. The harness calls Start before QEMU runs
    and Stop after it ends, even on failure; Check returns a failure message
    or None."""

    timeout = QEMU_TIMEOUT_SECONDS
    serial_input = False
    reboots = False
    shuts_down = False
    passed = ""

    def __init__(self, args):
        self.args = args

    def Start(self):
        pass

    def NetdevOptions(self):
        return ""

    def QemuArguments(self):
        return []

    def QmpSocket(self):
        return None

    def Launched(self):
        pass

    def Script(self, session):
        return []

    def Markers(self):
        return (SHELL_BANNER.encode(), PANIC_MARKER.encode())

    def OnOutput(self, session, clean_output):
        pass

    def Finished(self, clean_output):
        return False

    def Stop(self):
        pass

    def Check(self, result):
        return None

    def PassMessage(self, result):
        return self.passed


class BootToShell(Case):
    """Ends once the shell is up and init's DHCP client, which leases beside
    the shell, has logged its address, which can come after the banner."""

    def Script(self, session):
        return [WaitFor(NETWORK_DHCP_MESSAGE)]


class KernelTest(Case):
    """A kernel built with a test hook that reports one line over serial."""

    # A text, or one per architecture.
    expected = ""
    failure = ""
    ends_on_panic = False

    def Expected(self):
        if isinstance(self.expected, dict):
            return self.expected[self.args.arch]
        return self.expected

    def Markers(self):
        markers = (self.Expected().encode(),)
        if self.ends_on_panic:
            markers += (PANIC_MARKER.encode(),)
        return markers

    def Check(self, result):
        if self.Expected() not in result.output:
            return self.failure
        return None
