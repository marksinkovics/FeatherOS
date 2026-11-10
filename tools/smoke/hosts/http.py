import socket
import threading

from smoke.hosts.loopback import BindLoopbackPair

HTTP_LINE_COUNT = 10000
HTTP_LINES = "".join(f"line {number}\n" for number in range(HTTP_LINE_COUNT))


class HttpServer:
    """Answers HTTP/1.1 GET requests by path on the host's loopback, over
    IPv4 and IPv6 on one port, closing each connection after its response;
    keeps the Host header of each request with the version it came over."""

    def __init__(self):
        self.port, ipv4, ipv6 = BindLoopbackPair(socket.SOCK_STREAM)
        self.listeners = {socket.AF_INET: ipv4, socket.AF_INET6: ipv6}
        self.hosts = {socket.AF_INET: [], socket.AF_INET6: []}
        for family, listener in self.listeners.items():
            listener.listen()
            threading.Thread(
                target=self.Serve, args=(family, listener), daemon=True
            ).start()

    def Serve(self, family, listener):
        while True:
            try:
                connection, _ = listener.accept()
            except OSError:
                return
            with connection:
                connection.settimeout(10)
                try:
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = connection.recv(4096)
                        if not chunk:
                            break
                        request += chunk
                    head = request.split(b"\r\n\r\n")[0].decode()
                    lines = head.split("\r\n")
                    _, target, _ = lines[0].split(" ")
                    headers = dict(
                        line.split(": ", 1)
                        for line in lines[1:]
                        if ": " in line
                    )
                    self.hosts[family].append(headers.get("Host"))
                    connection.sendall(self.Respond(target))
                    connection.shutdown(socket.SHUT_WR)
                    while connection.recv(4096):
                        pass
                except (OSError, ValueError):
                    pass

    def Respond(self, target):
        path, _, query = target.partition("?")

        def Complete(status, headers, body=b""):
            return (
                f"HTTP/1.1 {status}\r\n{headers}"
                f"Content-Length: {len(body)}\r\n\r\n"
            ).encode() + body

        if path == "/hello.txt":
            return Complete(
                "200 OK",
                "Content-Type: text/plain\r\n",
                b"greetings from the host\n",
            )
        if path == "/chunked":
            return (
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                b"7\r\nchunks \r\n7;note=x\r\narrive \r\n"
                b"9\r\nin order\n\r\n0\r\nX-Trailer: done\r\n\r\n"
            )
        if path == "/moved":
            return Complete("302 Found", "Location: landing?via=moved\r\n")
        if path == "/elsewhere":
            return Complete(
                "301 Moved Permanently",
                f"Location: http://gateway.featheros.test:{self.port}"
                "/landing?via=elsewhere\r\n",
            )
        if path == "/landing":
            via = query.partition("=")[2]
            return Complete("200 OK", "", f"landed via {via}\n".encode())
        if path == "/status.json":
            return Complete(
                "200 OK",
                "Content-Type: application/json\r\n",
                b'{"message": "parsed from the host", "checks": '
                b'[{"name": "udp", "passed": true}, '
                b'{"name": "tcp", "passed": true}]}\n',
            )
        if path == "/lines":
            # No length: the body ends when the connection closes.
            return b"HTTP/1.1 200 OK\r\n\r\n" + HTTP_LINES.encode()
        return Complete("404 Not Found", "", b"no such page\n")

    def Close(self):
        for listener in self.listeners.values():
            listener.close()
