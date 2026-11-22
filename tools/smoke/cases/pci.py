import re

from smoke.cases.base import KernelTest

EDU_LINE = re.compile(
    r"KERN/I: PCI: [0-9a-f]{2}:[0-9a-f]{2}\.[0-7] 1234:11e8 class 00ff00"
)
MSI_LINE = re.compile(r"KERN/PCI: MSI delivered with data \d+")
DMA_LINE = re.compile(
    r"KERN/PCI: DMA round trip passed with buffers at \d+ and \d+"
)


class Pci(KernelTest):
    """QEMU's edu test device: it answers its registers, raises an interrupt
    on request (INTx on AArch64, then an MSI), and copies memory in and out
    by DMA."""

    expected = "KERN/PCI: tests passed"
    ends_on_panic = True
    failure = "PCI tests did not report success"
    passed = "edu delivered an MSI and a DMA round trip matched"

    def QemuArguments(self):
        # edu clamps DMA addresses to 28 bits, and virt's RAM starts at 1 GiB.
        if self.args.arch == "aarch64":
            return ["-device", "edu,dma_mask=0xffffffff"]
        return ["-device", "edu"]

    def Check(self, result):
        output = result.output
        if EDU_LINE.search(output) is None:
            return "the boot log does not list the edu device"
        required = ["KERN/PCI: edu answered", "KERN/PCI: tests passed"]
        if self.args.arch == "aarch64":
            required.append("KERN/PCI: INTx delivered")
        for line in required:
            if line not in output:
                return f"PCI tests never printed '{line}'"
        if MSI_LINE.search(output) is None:
            return "the MSI from edu did not arrive"
        if DMA_LINE.search(output) is None:
            return "the DMA round trip through edu did not pass"
        if "KERN/PCI: DMA below the first frame refused" not in output:
            return "a DMA allocation below the first frame was not refused"
        return None
