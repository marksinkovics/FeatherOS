from smoke.cases.base import KernelTest


class Panic(KernelTest):
    expected = "KERN/PANIC: Intentional panic test"
    failure = "expected panic record was not observed"
    passed = "expected panic was reported over serial"
