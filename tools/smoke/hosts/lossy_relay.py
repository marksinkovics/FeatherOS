import os
import select
import socket
import struct
import tempfile
import threading
import time


class LossyRelay:
    """Carries the guest's Ethernet frames between QEMU's user-mode network
    and its network device through filter-redirector objects, since QEMU
    itself never loses or reorders a packet. On TCP connections to port, it
    drops the DROPPED new data segments of each direction the first time they
    pass, and holds back the HELD one until the next frame has passed. It
    counts what it did by IP version and direction."""

    DROPPED = (5, 7)
    HELD = 12
    # A held frame goes on alone if nothing follows it for this long.
    HOLD_SECONDS = 0.05

    def __init__(self, port):
        self.port = port
        self.directory = tempfile.TemporaryDirectory(prefix="fr-")
        self.dropped = {}
        self.reordered = {}
        self.listeners = {}
        for name in ("rx_out", "rx_in", "tx_out", "tx_in"):
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(os.path.join(self.directory.name, name))
            listener.listen(1)
            self.listeners[name] = listener

    def QemuArguments(self, netdev):
        arguments = []
        for name in self.listeners:
            path = os.path.join(self.directory.name, name)
            arguments += ["-chardev", f"socket,id=relay_{name},path={path}"]
        # rx carries what the guest sends, tx what QEMU sends the guest.
        for queue in ("rx", "tx"):
            arguments += [
                "-object",
                f"filter-redirector,id=relay_{queue},netdev={netdev},"
                f"queue={queue},outdev=relay_{queue}_out,"
                f"indev=relay_{queue}_in",
            ]
        return arguments

    def Start(self):
        for queue, toward_guest in (("rx", False), ("tx", True)):
            threading.Thread(
                target=self.Relay, args=(queue, toward_guest), daemon=True
            ).start()

    @staticmethod
    def ParseSegment(frame):
        """Returns (family, flow, sequence, payload length) for a TCP
        segment in an Ethernet frame, or None."""
        ethertype = int.from_bytes(frame[12:14], "big")
        packet = frame[14:]
        if ethertype == 0x0800 and len(packet) >= 20 and packet[9] == 6:
            family = socket.AF_INET
            addresses = packet[12:20]
            end = int.from_bytes(packet[2:4], "big")
            segment = packet[(packet[0] & 0xF) * 4 : end]
        elif ethertype == 0x86DD and len(packet) >= 40 and packet[6] == 6:
            family = socket.AF_INET6
            addresses = packet[8:40]
            end = 40 + int.from_bytes(packet[4:6], "big")
            segment = packet[40:end]
        else:
            return None
        if len(segment) < 20:
            return None
        flow = (addresses, segment[0:4])
        sequence = int.from_bytes(segment[4:8], "big")
        payload = len(segment) - (segment[12] >> 4) * 4
        return family, flow, sequence, payload

    def Relay(self, queue, toward_guest):
        source, _ = self.listeners[f"{queue}_out"].accept()
        destination, _ = self.listeners[f"{queue}_in"].accept()
        flows = {}
        held = None
        held_until = 0
        received = bytearray()
        while True:
            wait = (
                max(0, held_until - time.monotonic())
                if held is not None
                else None
            )
            if not select.select([source], [], [], wait)[0]:
                destination.sendall(held)
                held = None
                continue
            chunk = source.recv(65536)
            if not chunk:
                return
            received += chunk
            # QEMU frames each packet with its length, big-endian.
            while len(received) >= 4:
                length = int.from_bytes(received[:4], "big")
                if len(received) < 4 + length:
                    break
                frame = bytes(received[4 : 4 + length])
                del received[: 4 + length]
                family, verdict = self.Judge(frame, flows)
                key = (family, toward_guest)
                if verdict == "drop":
                    self.dropped[key] = self.dropped.get(key, 0) + 1
                    continue
                if verdict == "hold":
                    held = struct.pack(">I", length) + frame
                    held_until = time.monotonic() + self.HOLD_SECONDS
                    continue
                destination.sendall(struct.pack(">I", length) + frame)
                if held is not None:
                    destination.sendall(held)
                    held = None
                    self.reordered[key] = self.reordered.get(key, 0) + 1

    def Judge(self, frame, flows):
        """Returns (family, "drop" | "hold" | "pass"), with no family for a
        frame that is not a TCP segment."""
        parsed = self.ParseSegment(frame)
        if parsed is None:
            return (None, "pass")
        family, flow, sequence, payload = parsed
        ports = struct.unpack(">HH", flow[1])
        if self.port not in ports or payload == 0:
            return (family, "pass")
        end, count = flows.get(flow, (sequence, 0))
        # Only data past what the flow sent before counts; a retransmission
        # always passes.
        if (sequence - end) % (1 << 32) >= 1 << 31:
            return (family, "pass")
        count += 1
        flows[flow] = ((sequence + payload) % (1 << 32), count)
        if count in self.DROPPED:
            return (family, "drop")
        if count == self.HELD:
            return (family, "hold")
        return (family, "pass")

    def Close(self):
        for listener in self.listeners.values():
            listener.close()
        self.directory.cleanup()
