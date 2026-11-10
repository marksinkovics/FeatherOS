import socket
import threading
import time

HTTPD_HELLO = b"SERVED FROM THE GUEST\n"
HTTPD_DOCS_INDEX = b"DOCS INDEX PAGE\n"


def ParseHttpResponses(data, methods):
    """Splits data into one (status, headers, body) per request method, with
    lower-case header names; raises ValueError for anything left over."""
    responses = []
    for method in methods:
        head, separator, data = data.partition(b"\r\n\r\n")
        if not separator:
            raise ValueError(f"incomplete response head {head[:60]!r}")
        lines = head.decode().split("\r\n")
        status = int(lines[0].split(" ")[1])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(": ")
            headers[name.lower()] = value
        length = 0 if method == "HEAD" else int(headers["content-length"])
        responses.append((status, headers, data[:length]))
        data = data[length:]
    if data:
        raise ValueError(f"bytes after the responses: {data[:60]!r}")
    return responses


class HttpdClient:
    """Sends requests to the guest's httpd through QEMU's port forwarding,
    retrying the first until the guest answers, and keeps the first response
    that differs from what it expects."""

    def __init__(self, port, program_path):
        self.port = port
        self.program_path = program_path
        self.failure = None
        self.done = threading.Event()
        self.thread = None

    def Start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.Run, daemon=True)
            self.thread.start()

    def Send(self, request):
        with socket.create_connection(
            ("127.0.0.1", self.port), timeout=10
        ) as connection:
            connection.sendall(request)
            data = b""
            while chunk := connection.recv(65536):
                data += chunk
            return data

    def Run(self):
        try:
            self.Check()
        except (OSError, ValueError, KeyError, IndexError) as error:
            self.failure = f"{type(error).__name__}: {error}"
        finally:
            self.done.set()

    @staticmethod
    def Expect(name, response, status, body=None, headers=None, absent=()):
        actual_status, actual_headers, actual_body = response
        if actual_status != status:
            raise ValueError(f"{name}: status {actual_status}, not {status}")
        if body is not None and actual_body != body:
            raise ValueError(f"{name}: body {actual_body[:60]!r}")
        for header, value in (headers or {}).items():
            if actual_headers.get(header) != value:
                raise ValueError(
                    f"{name}: {header} is {actual_headers.get(header)!r}"
                )
        for header in absent:
            if header in actual_headers:
                raise ValueError(f"{name}: unexpected {header} header")

    def Check(self):
        # Two requests in one send: the second waits behind the first's
        # response, and only it closes the connection.
        pipelined = (
            b"GET /hello.txt HTTP/1.1\r\nHost: guest\r\n\r\n"
            b"HEAD /hello.txt HTTP/1.1\r\nHost: guest\r\n"
            b"Connection: close\r\n\r\n"
        )
        deadline = time.monotonic() + 60
        data = b""
        while not data:
            if time.monotonic() > deadline:
                raise OSError("the guest's httpd never answered")
            # A guest not listening yet resets the connection, which QEMU
            # passes on as an empty one.
            try:
                data = self.Send(pipelined)
            except OSError:
                time.sleep(0.2)
        get, head = ParseHttpResponses(data, ("GET", "HEAD"))
        text = {"content-type": "text/plain; charset=utf-8"}
        self.Expect(
            "pipelined GET", get, 200, HTTPD_HELLO, text, ("connection",)
        )
        self.Expect(
            "pipelined HEAD",
            head,
            200,
            b"",
            {"content-length": str(len(HTTPD_HELLO)), "connection": "close"},
        )
        with open(self.program_path, "rb") as program:
            program_bytes = program.read()
        close = b"Host: guest\r\nConnection: close\r\n\r\n"
        cases = (
            (
                "escape",
                b"GET /../../system/init.conf HTTP/1.1\r\n" + close,
                404,
                None,
                {},
            ),
            (
                "encoded escape",
                b"GET /%2e%2e/%2E%2E/system/init.conf HTTP/1.1\r\n" + close,
                404,
                None,
                {},
            ),
            (
                "POST",
                b"POST /hello.txt HTTP/1.1\r\nHost: guest\r\n"
                b"Content-Length: 5\r\n\r\nhello",
                501,
                None,
                {"connection": "close"},
            ),
            (
                "HTTP/1.0",
                b"GET /hello.txt HTTP/1.0\r\n\r\n",
                200,
                HTTPD_HELLO,
                {"connection": "close"},
            ),
            (
                "no Host",
                b"GET /hello.txt HTTP/1.1\r\n\r\n",
                400,
                None,
                {"connection": "close"},
            ),
            ("HTTP/2.0", b"GET / HTTP/2.0\r\n" + close, 505, None, {}),
            (
                "directory",
                b"GET /docs HTTP/1.1\r\n" + close,
                301,
                None,
                {"location": "/docs/"},
            ),
            (
                "absolute form",
                b"GET http://guest/docs/ HTTP/1.1\r\n" + close,
                200,
                HTTPD_DOCS_INDEX,
                {"content-type": "text/html; charset=utf-8"},
            ),
            (
                "program",
                b"GET /vi HTTP/1.1\r\n" + close,
                200,
                program_bytes,
                {"content-type": "application/octet-stream"},
            ),
        )
        for name, request, status, body, headers in cases:
            (response,) = ParseHttpResponses(self.Send(request), ("GET",))
            self.Expect(name, response, status, body, headers)
