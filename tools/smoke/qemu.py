import json
import socket

from smoke.boot import QEMU_CPU_COUNT

NETDEV = "featheros_net"


def DiskArguments(args, case):
    """How featheros_disk, the case's image or the test disk, is attached:
    virtio-blk, or an NVMe namespace (512 or 4096-byte blocks) or an AHCI
    disk when DISK_BUS asks, so the root can be read through those drivers."""
    image = case.DiskImage() or args.disk
    drive = ["-drive", f"if=none,id=featheros_disk,file={image},format=raw"]
    bus = args.disk_bus
    if bus == "nvme":
        device = ["-device", "nvme,serial=feather-root,drive=featheros_disk"]
    elif bus == "nvme4k":
        device = [
            "-device",
            "nvme,id=nvme0,serial=feather-root",
            "-device",
            "nvme-ns,drive=featheros_disk,nsid=1,logical_block_size=4096,"
            "physical_block_size=4096",
        ]
    elif bus == "ahci":
        device = [
            "-device",
            "ich9-ahci,id=ahci",
            "-device",
            "ide-hd,drive=featheros_disk,bus=ahci.0",
        ]
    elif args.arch == "aarch64":
        device = ["-device", "virtio-blk-device,drive=featheros_disk"]
    else:
        device = [
            "-device",
            "virtio-blk-pci,drive=featheros_disk,disable-legacy=off",
        ]
    return drive + device


def Command(qemu, args, case):
    command = [qemu]
    if args.arch == "aarch64":
        command.extend(
            [
                "-machine",
                "virt",
                "-cpu",
                args.cpu or "cortex-a72",
                "-m",
                "2G",
                "-smp",
                str(QEMU_CPU_COUNT),
                "-global",
                "virtio-mmio.force-legacy=false",
                "-bios",
                args.firmware,
                "-device",
                "ramfb",
            ]
        )
        if case.virtio_keyboard:
            command.extend(["-device", "virtio-keyboard-device"])
    else:
        command.extend(
            ["-m", "2G", "-smp", str(QEMU_CPU_COUNT), "-boot", "order=d"]
        )
        if args.cpu:
            command.extend(["-cpu", args.cpu])
        if args.firmware:
            command.extend(
                [
                    "-drive",
                    f"if=pflash,unit=0,format=raw,readonly=on,file={args.firmware}",
                ]
            )
    if case.has_disk:
        command.extend(DiskArguments(args, case))
    command.extend(
        [
            # QEMU's user-mode network: the guest is 10.0.2.15 behind 10.0.2.2.
            "-netdev",
            f"user,id={NETDEV}{case.NetdevOptions()}",
            "-device",
            f"virtio-net-device,netdev={NETDEV}"
            if args.arch == "aarch64"
            # An empty romfile skips the network boot ROM.
            else f"virtio-net-pci,netdev={NETDEV},disable-legacy=off,romfile=",
        ]
    )
    command.extend(case.QemuArguments())
    command.extend(
        [
            "-display",
            "none",
            "-serial",
            "stdio",
            "-monitor",
            "none",
            *(() if case.reboots else ("-no-reboot",)),
            *(() if case.shuts_down else ("-no-shutdown",)),
        ]
    )
    if case.BootMedia() is None:
        command.extend(["-cdrom", args.iso])
    qmp_socket = case.QmpSocket()
    if qmp_socket is not None:
        command.extend(["-qmp", f"unix:{qmp_socket},server=on,wait=off"])
    return command


def SendkeyNames(text):
    names = {" ": "spc", "|": "shift-backslash", "\n": "ret"}
    return tuple(names.get(character, character) for character in text)


def SendQmpCommand(channel, command_id, execute, arguments=None):
    request = {"execute": execute, "id": command_id}
    if arguments is not None:
        request["arguments"] = arguments
    channel.write(json.dumps(request).encode() + b"\r\n")
    channel.flush()
    while True:
        response = json.loads(channel.readline())
        if response.get("id") == command_id:
            if "error" in response:
                raise RuntimeError(response["error"].get("desc", "QMP error"))
            return response


def SendQmpKeys(channel, first_id, keys):
    for offset, key in enumerate(keys):
        SendQmpCommand(
            channel,
            first_id + offset,
            "human-monitor-command",
            {"command-line": f"sendkey {key}"},
        )
    return first_id + len(keys)


def SendQmpKeyEvent(channel, command_id, key, down):
    event = {
        "type": "key",
        "data": {"down": down, "key": {"type": "qcode", "data": key}},
    }
    SendQmpCommand(
        channel, command_id, "input-send-event", {"events": [event]}
    )
    return command_id + 1


def OpenQmpChannel(socket_path):
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5)
    connection.connect(socket_path)
    channel = connection.makefile("rwb")
    greeting = json.loads(channel.readline())
    if "QMP" not in greeting:
        raise RuntimeError("QEMU did not send a QMP greeting")
    SendQmpCommand(channel, 1, "qmp_capabilities")
    return connection, channel
