from smoke.hosts.http import HTTP_LINE_COUNT, HTTP_LINES
from smoke.hosts.httpd_client import HTTPD_DOCS_INDEX, HTTPD_HELLO

EXPECTED_ARP = (
    "KERN/NETWORK: ARP resolved gateway 10.0.2.2 to 52:55:0a:00:02:02"
)
EXPECTED_NETWORK = "KERN/NETWORK: NDP resolved fec0::2 to 52:56:00:00:00:02"
EXPECTED_NETWORK_SWIFT_OUTPUT = [
    "parsed from the host",
    '{"name":"tcp","passed":true}',
]
LINK_DOWN_MESSAGE = "KERN/I: Network: link down"
LINK_UP_MESSAGE = "KERN/I: Network: link up"
# Between the two messages, with the same placeholders as the input.
EXPECTED_LINK_DOWN_OUTPUT = [
    "link down",
    # The DHCP client looks at its address each second.
    "dhcp: the address is gone; leasing again",
    "ipv4 none",
    "ping: sendto: Network is down",
    "nc: 10.0.2.2 {lossy_echo_port}: Network is down",
    "nc: 10.0.2.2 {echo_port}: Network is down",
    "nc: fec0::2 {echo_port}: Network is down",
    # A UDP socket connects without sending, so its first datagram fails.
    "nc: Network is down",
    "fetch: 10.0.2.2 {http_port}: Network is down",
    "64 bytes from 127.0.0.1: icmp_seq=1 time=",
    "KEPT LOCAL",
    "LOOPBACK CLIENT",
]
EXPECTED_LINK_UP_OUTPUT = [
    "dhcp: leased 10.0.2.15/24 via 10.0.2.2 for 86400 s",
    "BACK UP",
    "SIX BACK",
    "link up",
    "ipv4 10.0.2.15/24 via 10.0.2.2",
    "ipv6 fec0::5054:ff:fe12:3456/64 via fe80::2",
]
EXPECTED_NETWORK_SHELL_OUTPUT = [
    "ipv4 10.0.2.15/24 via 10.0.2.2",
    "ipv6 fe80::5054:ff:fe12:3456",
    "ipv6 fec0::5054:ff:fe12:3456/64 via fe80::2",
    "dns 10.0.2.3",
    # From slirp's router advertisement.
    "dns fec0::3",
    "localhost has address 127.0.0.1",
    "localhost has IPv6 address ::1",
    "ip6-localhost has IPv6 address ::1",
    "PING FeatherOS. (127.0.1.1): 56 data bytes",
    "64 bytes from 127.0.1.1: icmp_seq=1 time=",
    "PING ip6-loopback (::1): 56 data bytes",
    "64 bytes from ::1: icmp_seq=1 time=",
    "127.0.1.1 has name featheros",
    "::1 has name localhost",
    "alias.featheros.test has address 10.0.2.2",
    "alias.featheros.test has IPv6 address fec0::2",
    "10.0.2.2 has name gateway.featheros.test",
    "fec0::2 has name gateway.featheros.test",
    "host: 10.0.2.99: Name or service not known",
    "host: ip6-localhost: Name or service not known",
    "host: nothing.featheros.test: Name or service not known",
    "six.featheros.test has IPv6 address fec0::2",
    # The first and last of the 8 addresses getaddrinfo keeps.
    "big.featheros.test has address 10.0.3.1",
    "big.featheros.test has address 10.0.3.8",
    "big.featheros.test has IPv6 address fec0::3:1",
    "big.featheros.test has IPv6 address fec0::3:8",
    "PING gateway.featheros.test (10.0.2.2): 56 data bytes",
    "64 bytes from 10.0.2.2: icmp_seq=1 time=",
    "gateway.featheros.test has IPv6 address fec0::2",
    "PING gateway.featheros.test (fec0::2): 56 data bytes",
    "64 bytes from fec0::2: icmp_seq=1 time=",
    "OVER IPV4",
    "OVER IPV6",
    "Connection refused",
    "FROM HOST",
    "DATAGRAM FOUR",
    "DATAGRAM SIX",
    "nc: 127.0.0.1 9: Connection refused",
    "nc: ::1 9: Connection refused",
    "CLIENT SAYS HI",
    "SERVER SAYS HI",
    "SIX CLIENT",
    "SIX SERVER",
    "SECOND CLIENT",
    "echod: 127.0.0.1 connected, 1 open",
    "echod: ::1 connected, 2 open",
    "echod: ::1 closed after 14 bytes, 1 open",
    # What the first client sends is sleep's report, "sleep: slept 3 s".
    "echod: 127.0.0.1 closed after 17 bytes, 0 open",
    "greetings from the host",
    "chunks arrive in order",
    "landed via moved",
    "landed via elsewhere",
    "fetch: 404 Not Found",
    f"fetch: saved {len(HTTP_LINES)} bytes to /tmp/lines",
    f"{HTTP_LINE_COUNT} /tmp/lines",
    f"line {HTTP_LINE_COUNT - 1}",
    "fetch: https://featheros.test/: only http URLs are supported",
    HTTPD_HELLO.decode().strip(),
    HTTPD_DOCS_INDEX.decode().strip(),
    "SPACED OUT",
    '<li><a href="docs/">docs/</a></li>',
    '<li><a href="with%20space.txt">with space.txt</a></li>',
    "httpd: serving /tmp/www on port 80",
    f"127.0.0.1 GET /hello.txt 200 {len(HTTPD_HELLO)}",
    "::1 GET /docs 301 ",
    f"::1 GET /docs/ 200 {len(HTTPD_DOCS_INDEX)}",
    "::1 GET /with%20space.txt 200 11",
    "127.0.0.1 GET /nothing 404 ",
    # slirp hands forwarded connections over from the gateway's address.
    f"10.0.2.2 GET /hello.txt 200 {len(HTTPD_HELLO)}",
    "10.0.2.2 HEAD /hello.txt 200 0",
    "10.0.2.2 POST /hello.txt 501 ",
    "10.0.2.2 GET / 505 ",
    "ntp: fec0::2 offset +",
    "ntp: clock set to 2031-01-01 00:0",
    "ntp: 10.0.2.2 refused with kiss code RATE",
    "dhcp: leased 10.0.2.15/24 via 10.0.2.2 for 86400 s",
    "dhcp: renewed 10.0.2.15/24 via 10.0.2.2 for 86400 s",
]
# Before the clock is set it is years behind the server's; after, the server
# and date agree with it to well under a second.
EXPECTED_NTP_PATTERNS = [
    r"ntp: 10\.0\.2\.2 offset \+\d{6,}\.\d{3} s, delay \d+\.\d{3} s, "
    r"stratum 2",
    r"^2031-01-01 00:0\d:\d\d UTC\r?$",
    r"ntp: 10\.0\.2\.2 offset [+-]0\.\d{3} s, delay 0\.\d{3} s, stratum 2",
]
