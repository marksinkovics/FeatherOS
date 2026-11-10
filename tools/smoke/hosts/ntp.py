import datetime
import socket
import struct
import threading
import time

from smoke.hosts.loopback import BindLoopbackPair

# The test's NTP server keeps this time, advancing from when it starts.
NTP_SERVER_START = datetime.datetime(2031, 1, 1, tzinfo=datetime.timezone.utc)
# Seconds from the NTP epoch, 1900, to the Unix one.
NTP_UNIX_EPOCH = 2208988800


class NtpServer:
    """Answers NTP client requests on the host's loopback by a clock that
    starts at NTP_SERVER_START, over IPv4 and IPv6 on one port, and refuses
    every request to kiss_port with a RATE kiss; counts the answered requests
    each version carried."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_DGRAM)
        kiss = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        kiss.bind(("127.0.0.1", 0))
        self.kiss_port = kiss.getsockname()[1]
        self.started = time.monotonic()
        self.requests = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.sockets = [
            (socket.AF_INET, ipv4, False),
            (socket.AF_INET6, ipv6, False),
            (socket.AF_INET, kiss, True),
        ]
        for family, listener, refuse in self.sockets:
            threading.Thread(
                target=self.Serve, args=(family, listener, refuse), daemon=True
            ).start()

    def Now(self):
        seconds = (
            NTP_SERVER_START.timestamp()
            + NTP_UNIX_EPOCH
            + time.monotonic()
            - self.started
        )
        return int(seconds * (1 << 32))

    def Serve(self, family, listener, refuse):
        while True:
            try:
                request, sender = listener.recvfrom(512)
            except OSError:
                return
            # Only client packets: leap indicator, version, and mode 3.
            if len(request) < 48 or request[0] & 7 != 3:
                continue
            received = self.Now()
            version = request[0] >> 3 & 7
            poll = request[2]
            origin = request[40:48]
            if refuse:
                # A kiss is unsynchronized, stratum 0, with its code in the
                # reference ID; only the origin timestamp matters.
                reply = struct.pack(
                    "!BBBbII4sQ8sQQ",
                    3 << 6 | version << 3 | 4,
                    0,
                    poll,
                    -20,
                    0,
                    0,
                    b"RATE",
                    0,
                    origin,
                    0,
                    0,
                )
            else:
                reply = struct.pack(
                    "!BBBbII4sQ8sQQ",
                    version << 3 | 4,
                    2,
                    poll,
                    -20,
                    0,
                    1 << 8,
                    bytes((127, 0, 0, 1)),
                    received - (60 << 32),
                    origin,
                    received,
                    self.Now(),
                )
                self.requests[family] += 1
            listener.sendto(reply, sender)

    def Close(self):
        for _, listener, _ in self.sockets:
            listener.close()
