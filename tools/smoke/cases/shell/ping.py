from smoke.cases.shell.part import Part
from smoke.script import Lines

# slirp's gateway answers pings, and the stack answers its own address and
# loopback, for both IP versions.
PING_INPUT = (
    b"ping -c 2 10.0.2.2\nping -c 1 10.0.2.15\nping -c 1 127.0.0.1\n"
    b"ping 10.0.2\nping -c 1 fec0::2\nping -c 1 ::1\n"
    b"ping -c 1 fec0::5054:ff:fe12:3456\nping -6 10.0.2.2\n"
)
EXPECTED_PING_OUTPUT = (
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
)

PART = Part(Lines(PING_INPUT), EXPECTED_PING_OUTPUT)
