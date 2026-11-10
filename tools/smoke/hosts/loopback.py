import socket


def BindLoopbackPorts(kinds):
    """Binds a socket of each kind to one free port on both 127.0.0.1 and
    ::1; returns the port and the sockets by family and kind."""
    for _ in range(20):
        sockets = {}
        port = 0
        try:
            for kind in kinds:
                for family, address in (
                    (socket.AF_INET, "127.0.0.1"),
                    (socket.AF_INET6, "::1"),
                ):
                    bound = socket.socket(family, kind)
                    sockets[family, kind] = bound
                    bound.bind((address, port))
                    port = bound.getsockname()[1]
            return port, sockets
        except OSError:
            for bound in sockets.values():
                bound.close()
    raise OSError("no port free on both loopback addresses")


def BindLoopbackPair(kind):
    """Binds sockets of kind to one free port on 127.0.0.1 and ::1."""
    port, sockets = BindLoopbackPorts((kind,))
    return port, sockets[socket.AF_INET, kind], sockets[socket.AF_INET6, kind]


def ReceiveExactly(connection, count):
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def FreeTcpPort(address="127.0.0.1"):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((address, 0))
        return probe.getsockname()[1]
