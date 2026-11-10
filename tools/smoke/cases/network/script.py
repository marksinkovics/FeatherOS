# QEMU forwards a port on the host's loopback to this one in the guest.
FORWARDED_GUEST_PORT = 8080
# And another to httpd's default port.
HTTPD_GUEST_PORT = 80
NETWORK_SHELL_INPUT = (
    "net\nhost localhost\n"
    # Names only /etc/hosts knows, which never reach a name server.
    "host ip6-localhost\nping -c 1 FeatherOS.\nping -c 1 ip6-loopback\n"
    "host 127.0.1.1\nhost ::1\n"
    # Name servers in /etc/resolv.conf come before the kernel's.
    "echo nameserver 10.0.2.2#{port} > /etc/resolv.conf\n"
    "host alias.featheros.test\n"
    # Addresses' names come from PTR records under in-addr.arpa and ip6.arpa.
    "host 10.0.2.2\nhost fec0::2\nhost 10.0.2.99\n"
    # /etc/hosts has no IPv4 address for it, so the harness's server says no.
    "host -4 ip6-localhost\n"
    "host nothing.featheros.test\nping -c 1 gateway.featheros.test\n"
    # The server truncates this answer over UDP, so it is asked over TCP.
    "host -4 big.featheros.test\n"
    # Nothing answers on port 9, so the lookup waits out one short timeout
    # before asking the next server.
    "echo nameserver 10.0.2.2#9 > /etc/resolv.conf\n"
    "echo nameserver fec0::2#{port} >> /etc/resolv.conf\n"
    "echo options timeout:1 attempts:1 >> /etc/resolv.conf\n"
    "host -6 six.featheros.test\n"
    # Without name servers in the file, the kernel's list is asked.
    "echo options attempts:2 > /etc/resolv.conf\n"
    "net dns fec0::2#{port}\nhost -6 gateway.featheros.test\n"
    "host -6 big.featheros.test\n"
    "ping -6 -c 1 gateway.featheros.test\n"
    # TCP: the host's echo server answers in upper case, so its replies differ
    # from the typed lines, and so do loopback servers and clients that pipe
    # what they receive through upper.
    "echo over ipv4 | nc 10.0.2.2 {echo_port}\n"
    "echo over ipv6 | nc -6 gateway.featheros.test {echo_port}\n"
    "nc 10.0.2.2 {closed_port}\n"
    f"echo from guest | nc -l {FORWARDED_GUEST_PORT} | upper\n"
    # UDP: the echo server answers datagrams in upper case too. Nothing in
    # the guest listens on port 9, so its ICMP port unreachable errors make
    # the connected sockets fail.
    "echo datagram four | nc -u 10.0.2.2 {udp_echo_port}\n"
    "echo datagram six | nc -u -6 gateway.featheros.test {udp_echo_port}\n"
    "echo nobody | nc -u 127.0.0.1 9\necho nobody | nc -u ::1 9\n"
    "echo server says hi | nc -l 7000 | upper &\nsleep 0.5\n"
    "echo client says hi | nc 127.0.0.1 7000 | upper\n"
    "echo six server | nc -l -6 7001 | upper &\nsleep 0.5\n"
    "echo six client | nc ::1 7001 | upper\n"
    # A program file is large enough to fill windows in both directions; the
    # echo server keeps every byte that cat sends.
    "cat /system/vi | wc -c\n"
    "cat /system/vi | nc 10.0.2.2 {echo_port} | wc -c\n"
    # On its own echo port, the harness's relay drops and reorders a few
    # segments each way, which both sides' TCP must recover from.
    "cat /system/vi | nc 10.0.2.2 {lossy_echo_port} | wc -c\n"
    "cat /system/vi | nc -6 gateway.featheros.test {lossy_echo_port} | wc -c\n"
    # echod serves every client from one thread with non-blocking sockets:
    # the first client holds its connection open while the second is answered.
    "echod > /tmp/echod.log &\nsleep 0.5\n"
    "sleep 3 | nc 127.0.0.1 7 &\nsleep 0.5\n"
    "echo second client | nc ::1 7 | upper\n"
    "sleep 3\n"
    # Over loopback both ends scale their windows: five copies of libctest, a
    # few megabytes, grow the rings past the unscaled 64 KiB each way.
    "cat libctest >> /tmp/b\ncat libctest >> /tmp/b\n"
    "cat libctest >> /tmp/b\ncat libctest >> /tmp/b\n"
    "cat libctest >> /tmp/b\ncat /tmp/b | wc -c\n"
    "cat /tmp/b | nc 127.0.0.1 7 | wc -c\n"
    "cat /tmp/b | nc ::1 7 | wc -c\nrm /tmp/b\n"
    "kill %1\ncat /tmp/echod.log\n"
    # HTTP: the host's server answers by path, over both versions.
    "fetch 10.0.2.2:{http_port}/hello.txt\n"
    "fetch http://[fec0::2]:{http_port}/chunked\n"
    "fetch http://10.0.2.2:{http_port}/moved\n"
    "fetch -6 http://gateway.featheros.test:{http_port}/elsewhere\n"
    "fetch http://10.0.2.2:{http_port}/missing\n"
    "fetch -o /tmp/lines http://10.0.2.2:{http_port}/lines\n"
    "wc -l /tmp/lines\ntail -n 1 /tmp/lines\n"
    "fetch https://featheros.test/\n"
    "{fetch_json}"
    # httpd serves a directory in /tmp to fetch over both versions and,
    # through port forwarding, to the harness's HttpdClient. The files hold
    # upper-case text, so their contents differ from the typed commands.
    "mkdir /tmp/www\nmkdir /tmp/www/docs\n"
    "echo served from the guest | upper > /tmp/www/hello.txt\n"
    "echo docs index page | upper > /tmp/www/docs/index.html\n"
    'echo spaced out | upper > "/tmp/www/with space.txt"\n'
    "cp /system/vi /tmp/www/vi\n"
    "httpd /tmp/www > /tmp/httpd.log &\nsleep 0.5\n"
    "fetch 127.0.0.1/hello.txt\n"
    "fetch http://[::1]/docs\n"
    "fetch http://[::1]/with%20space.txt\n"
    "fetch 127.0.0.1/\n"
    "fetch 127.0.0.1/nothing\n"
    "fetch -o /tmp/vi.copy http://[::1]/vi\n"
    "cat /tmp/vi.copy | wc -c\n"
    "{wait_for_httpd_client}\n"
    "kill %1\ncat /tmp/httpd.log\n"
    # NTP: the host's server keeps a clock that starts in 2031. The first
    # query only measures; the second sets the clock, after which the offset
    # is close to zero. The server's other port refuses with a kiss.
    "ntp -n -p {ntp_port} 10.0.2.2\n"
    "ntp -6 -p {ntp_port} gateway.featheros.test\n"
    "date\nntp -n -p {ntp_port} 10.0.2.2\n"
    "ntp -p {ntp_kiss_port} 10.0.2.2\n"
    # The running DHCP client renews when asked, which restores the DHCP name
    # server.
    "dhcp -r\n{wait_for_renewal}\nnet\n"
    # The kill switch: while the link is down nothing reaches the harness's
    # servers and every send to them fails at once, but loopback works. Up,
    # IPv6 comes back at once and IPv4 once the DHCP client, which noticed
    # its address gone, sees the link back and leases again.
    # The connection held open through the lossy relay ends with the link.
    "sleep 5 | nc 10.0.2.2 {lossy_echo_port} &\nsleep 0.5\n"
    "net down\n{mark_traffic}\nnet\n"
    "ping -c 1 10.0.2.2\nping -6 -c 1 fec0::2\n"
    "echo cut off | nc 10.0.2.2 {echo_port}\n"
    "echo cut off | nc -6 fec0::2 {echo_port}\n"
    "echo cut off | nc -u 10.0.2.2 {udp_echo_port}\n"
    "fetch 10.0.2.2:{http_port}/hello.txt\n"
    "ping -c 1 127.0.0.1\n"
    "echo kept local | nc -l 7002 | upper &\nsleep 0.5\n"
    "echo loopback client | nc 127.0.0.1 7002 | upper\n"
    "{mark_traffic}\nnet up\n{wait_for_new_lease}\n"
    "echo back up | nc 10.0.2.2 {echo_port}\n"
    "echo six back | nc -6 fec0::2 {echo_port}\n"
    "net\n"
)
# json, written in Swift, reads what fetch prints; skipped like the other Swift
# programs.
NETWORK_SWIFT_INPUT = (
    "fetch 10.0.2.2:{http_port}/status.json | json -r .message\n"
    "fetch http://[fec0::2]:{http_port}/status.json | json -c .checks[1]\n"
)
