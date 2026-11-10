import socket
import threading
import time


class ForwardedClient:
    """Connects to a guest server through QEMU's port forwarding, retrying
    until the guest accepts, sends a line and keeps what the guest sends
    back."""

    REQUEST = b"from host\n"

    def __init__(self, port):
        self.port = port
        self.received = b""
        self.thread = None

    def Start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.Run, daemon=True)
            self.thread.start()

    def Run(self):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=5
                ) as connection:
                    connection.sendall(self.REQUEST)
                    data = b""
                    while chunk := connection.recv(4096):
                        data += chunk
                    # A guest not listening yet resets the connection, which
                    # QEMU passes on as an empty one.
                    if data:
                        self.received = data
                        return
            except OSError:
                pass
            time.sleep(0.2)
