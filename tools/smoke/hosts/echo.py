import socket
import threading

from smoke.hosts.loopback import BindLoopbackPair


class EchoServer:
    """Echoes TCP connections in upper case on the host's loopback, over IPv4
    and IPv6 on one port, until the client finishes sending, and UDP
    datagrams on another port; counts the connections and datagrams each
    version carried."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_STREAM)
        self.udp_port, udp_ipv4, udp_ipv6 = BindLoopbackPair(socket.SOCK_DGRAM)
        self.listeners = {socket.AF_INET: ipv4, socket.AF_INET6: ipv6}
        self.datagram_sockets = {
            socket.AF_INET: udp_ipv4,
            socket.AF_INET6: udp_ipv6,
        }
        self.connections = {socket.AF_INET: 0, socket.AF_INET6: 0}
        self.datagrams = {socket.AF_INET: 0, socket.AF_INET6: 0}
        for family, listener in self.listeners.items():
            listener.listen()
            threading.Thread(
                target=self.Serve, args=(family, listener), daemon=True
            ).start()
        for family, receiver in self.datagram_sockets.items():
            threading.Thread(
                target=self.ServeDatagrams,
                args=(family, receiver),
                daemon=True,
            ).start()

    def ServeDatagrams(self, family, receiver):
        while True:
            try:
                datagram, sender = receiver.recvfrom(2048)
                self.datagrams[family] += 1
                receiver.sendto(datagram.upper(), sender)
            except OSError:
                return

    def Serve(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            self.connections[family] += 1
            with connection:
                connection.settimeout(10)
                try:
                    while chunk := connection.recv(4096):
                        connection.sendall(chunk.upper())
                except OSError:
                    pass

    def Close(self):
        for listener in self.listeners.values():
            listener.close()
        for receiver in self.datagram_sockets.values():
            receiver.close()
