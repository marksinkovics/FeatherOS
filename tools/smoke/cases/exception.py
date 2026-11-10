from smoke.cases.base import KernelTest


class CpuException(KernelTest):
    expected = {
        "x86_64": "KERN/EXCEPTION: Divide error",
        "aarch64": "KERN/EXCEPTION: AArch64 undefined instruction",
    }
    failure = "expected CPU exception was not reported"
    passed = "expected CPU exception was reported over serial"
