import os

from smoke.boot import DHCP_LEASED_MESSAGE, PANIC_MARKER
from smoke.build_tree import ProgramBuilt
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.network.checks import CheckNetwork
from smoke.cases.network.script import (
    FORWARDED_GUEST_PORT,
    HTTPD_GUEST_PORT,
    NETWORK_SHELL_INPUT,
    NETWORK_SWIFT_INPUT,
)
from smoke.hosts.dns import DnsServer
from smoke.hosts.echo import EchoServer
from smoke.hosts.forwarded_client import ForwardedClient
from smoke.hosts.http import HttpServer
from smoke.hosts.httpd_client import HttpdClient
from smoke.hosts.loopback import FreeTcpPort
from smoke.hosts.lossy_relay import LossyRelay
from smoke.hosts.ntp import NtpServer
from smoke.qemu import NETDEV
from smoke.script import WaitFor, WaitUntil
from smoke.terminal import ANSI_ESCAPE_BYTES


class Network(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    passed = (
        "ARP and neighbor discovery resolved their hosts; DHCP, "
        "DNS over IPv4 and IPv6, pings by name, TCP clients and servers, "
        "HTTP clients and servers, and NTP worked"
    )

    def __init__(self, args):
        super().__init__(args)
        self.servers = []
        self.swift_built = False
        # What the harness's servers had seen when the link went down and
        # before it came back up.
        self.traffic_marks = []

    def Start(self):
        self.forwarded_client = ForwardedClient(FreeTcpPort())
        self.httpd_client = HttpdClient(
            FreeTcpPort(),
            os.path.join(self.args.build_dir, "initramfs", "system", "vi"),
        )
        self.lossy_echo_server = EchoServer()
        self.relay = LossyRelay(self.lossy_echo_server.port)
        self.dns_server = DnsServer()
        self.echo_server = EchoServer()
        self.http_server = HttpServer()
        self.ntp_server = NtpServer()
        self.servers = [
            self.dns_server,
            self.echo_server,
            self.http_server,
            self.ntp_server,
            self.lossy_echo_server,
            self.relay,
        ]

    def NetdevOptions(self):
        return (
            f",hostfwd=tcp:127.0.0.1:{self.forwarded_client.port}-"
            f":{FORWARDED_GUEST_PORT}"
            f",hostfwd=tcp:127.0.0.1:{self.httpd_client.port}-"
            f":{HTTPD_GUEST_PORT}"
        )

    def QemuArguments(self):
        return self.relay.QemuArguments(NETDEV)

    def Launched(self):
        self.relay.Start()

    def Script(self, session):
        self.swift_built = ProgramBuilt(
            self.args.build_dir, self.args.arch, "hello"
        )
        if not self.swift_built:
            print("NOTE: Swift programs were not built; skipping their checks")
        lines = (
            NETWORK_SHELL_INPUT.replace(
                "{fetch_json}", NETWORK_SWIFT_INPUT if self.swift_built else ""
            )
            .replace("{port}", str(self.dns_server.port))
            .replace("{echo_port}", str(self.echo_server.port))
            .replace("{lossy_echo_port}", str(self.lossy_echo_server.port))
            .replace("{udp_echo_port}", str(self.echo_server.udp_port))
            .replace("{http_port}", str(self.http_server.port))
            .replace("{ntp_port}", str(self.ntp_server.port))
            .replace("{ntp_kiss_port}", str(self.ntp_server.kiss_port))
            # Nothing listens on a port that was just free.
            .replace("{closed_port}", str(FreeTcpPort()))
            .encode()
            .splitlines(keepends=True)
        )

        def MarkTraffic():
            self.traffic_marks.append(
                (
                    dict(self.dns_server.queries),
                    dict(self.dns_server.stream_queries),
                    dict(self.echo_server.connections),
                    dict(self.echo_server.datagrams),
                    dict(self.lossy_echo_server.connections),
                    {
                        family: len(hosts)
                        for family, hosts in self.http_server.hosts.items()
                    },
                    dict(self.ntp_server.requests),
                )
            )
            return True

        def LeasedAgain():
            clean = ANSI_ESCAPE_BYTES.sub(b"", session.captured)
            return clean.count(DHCP_LEASED_MESSAGE) >= 2

        waits = {
            b"{wait_for_httpd_client}\n": WaitUntil(
                self.httpd_client.done.is_set
            ),
            b"{wait_for_renewal}\n": WaitFor("dhcp: renewed"),
            b"{mark_traffic}\n": WaitUntil(MarkTraffic),
            b"{wait_for_new_lease}\n": WaitUntil(LeasedAgain),
        }
        # init's DHCP client leases in the background while the shell starts.
        return [WaitFor("dhcp: leased")] + [
            waits.get(line, line) for line in lines
        ]

    # The last command's last line.
    def Markers(self):
        return (b"dns fec0::3", PANIC_MARKER.encode())

    def OnOutput(self, session, clean_output):
        if f"nc -l {FORWARDED_GUEST_PORT}".encode() in clean_output:
            self.forwarded_client.Start()
        if b"httpd /tmp/www" in clean_output:
            self.httpd_client.Start()

    def Stop(self):
        for server in self.servers:
            server.Close()

    def Check(self, result):
        return CheckNetwork(self, result)
