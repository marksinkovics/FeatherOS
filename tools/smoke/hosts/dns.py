import ipaddress
import socket
import struct
import threading

from smoke.hosts.loopback import BindLoopbackPorts, ReceiveExactly

# The test's name server listens on the host's loopback, which the guest
# reaches as 10.0.2.2 and fec0::2. The {port} placeholders become its port.
NETWORK_DNS_ADDRESSES = {
    "gateway.featheros.test": {
        1: [bytes((10, 0, 2, 2))],
        28: [socket.inet_pton(socket.AF_INET6, "fec0::2")],
    },
    # Too many records for a datagram, so the answer comes over TCP.
    "big.featheros.test": {
        1: [bytes((10, 0, 3, number)) for number in range(1, 41)],
        28: [
            socket.inet_pton(socket.AF_INET6, f"fec0::3:{number}")
            for number in range(1, 41)
        ],
    },
}
NETWORK_DNS_ALIASES = {
    "alias.featheros.test": "gateway.featheros.test",
    "six.featheros.test": "gateway.featheros.test",
}
# PTR records, which getnameinfo asks for.
NETWORK_DNS_POINTERS = {
    ipaddress.ip_address(address).reverse_pointer: "gateway.featheros.test"
    for address in ("10.0.2.2", "fec0::2")
}


def EncodeDnsName(name):
    return (
        b"".join(
            bytes((len(label),)) + label.encode() for label in name.split(".")
        )
        + b"\0"
    )


class DnsServer:
    """Answers A and AAAA queries from NETWORK_DNS_ADDRESSES and
    NETWORK_DNS_ALIASES, and PTR queries from NETWORK_DNS_POINTERS, on the
    host's loopback, over IPv4 and IPv6 on one port for UDP and TCP, and
    counts the queries each version carried over each. A UDP answer over 512
    bytes is truncated to its question, with the TC bit set, as servers do
    without EDNS."""

    def __init__(self):
        self.port, sockets = BindLoopbackPorts(
            (socket.SOCK_DGRAM, socket.SOCK_STREAM)
        )
        self.sockets = list(sockets.values())
        self.queries = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.stream_queries = {socket.AF_INET: 0, socket.AF_INET6: 0}
        for (family, kind), listener in sockets.items():
            if kind == socket.SOCK_STREAM:
                listener.listen()
                target = self.ServeStreams
            else:
                target = self.Serve
            threading.Thread(
                target=target, args=(family, listener), daemon=True
            ).start()

    def Serve(self, family, listener):
        while True:
            try:
                query, sender = listener.recvfrom(512)
            except OSError:
                return
            response = self.Answer(query)
            if response is not None:
                self.queries[family] += 1
                if len(response) > 512:
                    response = self.Truncate(response)
                listener.sendto(response, sender)

    def ServeStreams(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            with connection:
                connection.settimeout(10)
                try:
                    # Each message follows its length in two bytes.
                    while prefix := ReceiveExactly(connection, 2):
                        length = struct.unpack("!H", prefix)[0]
                        query = ReceiveExactly(connection, length)
                        response = query and self.Answer(query)
                        if not response:
                            break
                        self.stream_queries[family] += 1
                        connection.sendall(
                            struct.pack("!H", len(response)) + response
                        )
                except OSError:
                    pass

    @staticmethod
    def Truncate(response):
        offset = 12
        while response[offset] != 0:
            offset += 1 + response[offset]
        identifier, flags = struct.unpack("!HH", response[:4])
        header = struct.pack("!HHHHHH", identifier, flags | 0x0200, 1, 0, 0, 0)
        return header + response[12 : offset + 5]

    @staticmethod
    def Answer(query):
        if len(query) < 12:
            return None
        identifier, _, questions = struct.unpack("!HHH", query[:6])
        labels = []
        offset = 12
        while offset < len(query) and query[offset] != 0:
            length = query[offset]
            labels.append(query[offset + 1 : offset + 1 + length].decode())
            offset += 1 + length
        offset += 1
        if questions != 1 or offset + 4 > len(query):
            return None
        record_type = struct.unpack("!H", query[offset : offset + 2])[0]
        question = query[12 : offset + 4]
        name = ".".join(labels).lower()
        answers = []
        target = NETWORK_DNS_ALIASES.get(name)
        if target is not None:
            answers.append(
                b"\xc0\x0c"
                + struct.pack("!HHIH", 5, 1, 60, len(EncodeDnsName(target)))
                + EncodeDnsName(target)
            )
        owner = target or name
        records = NETWORK_DNS_ADDRESSES.get(owner)
        pointer = NETWORK_DNS_POINTERS.get(owner)
        if pointer is not None:
            records = {12: [EncodeDnsName(pointer)]}
        if records is None:
            # NXDOMAIN, with recursion desired and available.
            header = struct.pack("!HHHHHH", identifier, 0x8183, 1, 0, 0, 0)
            return header + question
        for data in records.get(record_type, []):
            owner_name = (
                EncodeDnsName(owner) if target is not None else b"\xc0\x0c"
            )
            answers.append(
                owner_name
                + struct.pack("!HHIH", record_type, 1, 60, len(data))
                + data
            )
        header = struct.pack(
            "!HHHHHH", identifier, 0x8180, 1, len(answers), 0, 0
        )
        return header + question + b"".join(answers)

    def Close(self):
        for listener in self.sockets:
            listener.close()
