from smoke.cases.base import KernelTest


class Interrupt(KernelTest):
    expected = {
        "x86_64": "KERN/IRQ: PIT interrupt acknowledged",
        "aarch64": "KERN/IRQ: GIC software interrupt acknowledged",
    }
    failure = "expected hardware interrupt was not acknowledged"
    passed = "hardware interrupt was acknowledged over serial"
