from smoke.cases.base import KernelTest


class Memory(KernelTest):
    expected = "KERN/MEMORY: tests passed"
    failure = "memory tests did not report success"
    passed = "memory tests reported success over serial"
