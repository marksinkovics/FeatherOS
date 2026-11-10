import re
import socket

from smoke.cases.network.expected import (
    EXPECTED_ARP,
    EXPECTED_LINK_DOWN_OUTPUT,
    EXPECTED_LINK_UP_OUTPUT,
    EXPECTED_NETWORK,
    EXPECTED_NETWORK_SHELL_OUTPUT,
    EXPECTED_NETWORK_SWIFT_OUTPUT,
    EXPECTED_NTP_PATTERNS,
    LINK_DOWN_MESSAGE,
    LINK_UP_MESSAGE,
)
from smoke.hosts.lossy_relay import LossyRelay

VERSIONS = ((socket.AF_INET, "IPv4"), (socket.AF_INET6, "IPv6"))


def CheckServers(case):
    for family, version in VERSIONS:
        if case.dns_server.queries[family] == 0:
            return f"no DNS query reached the test server over {version}"
        if case.dns_server.stream_queries[family] == 0:
            return (
                f"no DNS query reached the test server over TCP and {version}"
            )
        if case.echo_server.connections[family] == 0:
            return f"no TCP connection reached the echo server over {version}"
        if case.echo_server.datagrams[family] == 0:
            return f"no UDP datagram reached the echo server over {version}"
        if case.ntp_server.requests[family] == 0:
            return f"no NTP request reached the test server over {version}"
    return None


def CheckLinkDown(case, result):
    output = result.output
    down_start = output.rfind(LINK_DOWN_MESSAGE)
    down_end = output.find(LINK_UP_MESSAGE, max(down_start, 0))
    if down_start < 0 or down_end < 0:
        return f"net down and net up did not both log{result.limit_note}"
    down_output = output[down_start:down_end]
    up_output = output[down_end + len(LINK_UP_MESSAGE) :]
    ports = {
        "{echo_port}": str(case.echo_server.port),
        "{lossy_echo_port}": str(case.lossy_echo_server.port),
        "{http_port}": str(case.http_server.port),
    }
    for expected, section, state in (
        (EXPECTED_LINK_DOWN_OUTPUT, down_output, "down"),
        (EXPECTED_LINK_UP_OUTPUT, up_output, "up"),
    ):
        for line in expected:
            for placeholder, port in ports.items():
                line = line.replace(placeholder, port)
            if line not in section:
                return (
                    f"with the link {state}, the output is missing "
                    f"{line!r}{result.limit_note}"
                )
    if (
        down_output.count("ping: sendto: Network is down") != 2
        or "ipv6 fe80::" in down_output
    ):
        return "with the link down, a ping went out or IPv6 stayed"
    marks = case.traffic_marks
    if len(marks) != 2 or marks[0] != marks[1]:
        return (
            "the harness's servers saw traffic while the link was "
            f"down: {marks}"
        )
    return None


def CheckTransfers(case, result):
    port = case.http_server.port
    for family, host in (
        (socket.AF_INET, f"10.0.2.2:{port}"),
        (socket.AF_INET6, f"[fec0::2]:{port}"),
        (socket.AF_INET6, f"gateway.featheros.test:{port}"),
    ):
        if host not in case.http_server.hosts[family]:
            return (
                f"no HTTP request for Host {host} reached the "
                f"test server: {case.http_server.hosts}"
            )
    counts = {}
    for name, count in re.findall(
        r"cat (\S+) [^\r\n]*\| wc -c\r?\n(\d+)\r?$",
        result.output,
        re.MULTILINE,
    ):
        counts.setdefault("bulk" if name == "/tmp/b" else "vi", []).append(
            count
        )
    for name, expected in (("vi", 5), ("bulk", 3)):
        found = counts.get(name, [])
        if len(found) != expected or len(set(found)) != 1:
            return (
                f"{name} byte counts sent and echoed back differ: "
                f"{found}{result.limit_note}"
            )
    for family, version in VERSIONS:
        for toward_guest, direction in ((False, "from"), (True, "to")):
            key = (family, toward_guest)
            dropped = case.relay.dropped.get(key, 0)
            reordered = case.relay.reordered.get(key, 0)
            if dropped != len(LossyRelay.DROPPED) or reordered != 1:
                return (
                    f"the relay dropped {dropped} and reordered "
                    f"{reordered} TCP segments {direction} the guest over "
                    f"{version}{result.limit_note}"
                )
    if case.forwarded_client.received != b"from guest\n":
        return (
            "the guest's server sent "
            f"{case.forwarded_client.received!r} through port forwarding"
        )
    httpd_client = case.httpd_client
    if not httpd_client.done.is_set() or httpd_client.failure:
        return (
            "the guest's httpd through port forwarding: "
            f"{httpd_client.failure or 'the client did not finish'}"
        )
    return None


def CheckNetwork(case, result):
    output = result.output
    if EXPECTED_ARP not in output:
        return "ARP did not resolve the gateway"
    if EXPECTED_NETWORK not in output:
        return "neighbor discovery did not resolve fec0::2"
    missing = [
        line
        for line in EXPECTED_NETWORK_SHELL_OUTPUT
        + (EXPECTED_NETWORK_SWIFT_OUTPUT if case.swift_built else [])
        if line not in output
    ]
    if missing:
        return (
            f"network shell output is missing {missing[0]!r}"
            f"{result.limit_note}"
        )
    failure = CheckServers(case) or CheckLinkDown(case, result)
    if failure is not None:
        return failure
    for pattern in EXPECTED_NTP_PATTERNS:
        if re.search(pattern, output, re.MULTILINE) is None:
            return (
                f"network shell output does not match {pattern!r}"
                f"{result.limit_note}"
            )
    return CheckTransfers(case, result)
