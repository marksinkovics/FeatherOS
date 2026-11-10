READY_MESSAGE = "KERN/I: Kernel initialization complete"
TIMER_STARTED_MESSAGE = "KERN/I: Timer started at 100 Hz"
SHELL_BANNER = "FeatherOS shell. Type 'help' for commands."
VFS_MESSAGE = "KERN/I: VFS: hello.txt read passed"
FAT16_MESSAGE = "KERN/I: VFS: FAT16 BLOCK.TXT read passed"
ELF_LOAD_MESSAGE = "KERN/I: ELF64: test image loaded into user address space"
USER_MODE_MESSAGES = {
    "x86_64": "KERN/I: ELF64: ring 3 console and exit syscalls passed",
    "aarch64": "KERN/I: ELF64: EL0 console and exit syscalls passed",
}
USER_MODE_OUTPUTS = {
    "x86_64": (
        b"UKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
    "aarch64": (
        b"AKERN/I: ELF64: two isolated user programs exited and were reaped"
    ),
}
PANIC_MARKER = "KERN/PANIC:"
MAX_CPU_PROTECTIONS = {
    "x86_64": "KERN/I: CPU protections: SMEP SMAP RDRAND",
    "aarch64": "KERN/I: CPU protections: PAN RNDR",
}
EXPECTED_ARCHITECTURE = {
    "x86_64": "KERN/I: Architecture: x86_64",
    "aarch64": "KERN/I: Architecture: aarch64",
}
QEMU_CPU_COUNT = 4
SMP_MESSAGE = f"KERN/I: SMP: {QEMU_CPU_COUNT} CPUs online with IPIs and timers"
# QEMU gives the first NIC 52:54:00:12:34:56, and slirp answers ARP for
# 10.0.2.2 as 52:55:0a:00:02:02. Its router fe80::2 advertises fec0::/64, and
# it answers neighbor solicitations for fec0::2 as 52:56 and the address's
# last 32 bits.
NETWORK_MESSAGE = "KERN/I: Network: virtio-net 52:54:00:12:34:56"
# init's DHCP client leases slirp's only address.
NETWORK_DHCP_MESSAGE = "KERN/I: Network: IPv4 10.0.2.15/24 via 10.0.2.2"
DHCP_LEASED_MESSAGE = b"dhcp: leased "
NETWORK_IPV6_MESSAGE = (
    "KERN/I: Network: IPv6 fe80::5054:ff:fe12:3456 and "
    "fec0::5054:ff:fe12:3456/64 via fe80::2"
)
