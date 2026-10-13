import os
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = REPO_ROOT / "build.sh"


class BootMatrixTests(unittest.TestCase):
    def run_boot_case(
        self,
        arch,
        panic_test=False,
        exception_test=False,
        interrupt_test=False,
        memory_test=False,
        timer_test=False,
    ):
        environment = os.environ.copy()
        environment["PANIC_TEST"] = "ON" if panic_test else "OFF"
        environment["EXCEPTION_TEST"] = "ON" if exception_test else "OFF"
        environment["INTERRUPT_TEST"] = "ON" if interrupt_test else "OFF"
        environment["MEMORY_TEST"] = "ON" if memory_test else "OFF"
        environment["TIMER_TEST"] = "ON" if timer_test else "OFF"
        command = ["bash", str(BUILD_SCRIPT), "--arch", arch, "test"]

        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_01_x86_64_panic_reporting(self):
        self.run_boot_case("x86_64", panic_test=True)

    def test_02_aarch64_panic_reporting(self):
        self.run_boot_case("aarch64", panic_test=True)

    def test_03_x86_64_divide_error_reporting(self):
        self.run_boot_case("x86_64", exception_test=True)

    def test_04_aarch64_undefined_instruction_reporting(self):
        self.run_boot_case("aarch64", exception_test=True)

    def test_05_x86_64_normal_boot(self):
        self.run_boot_case("x86_64", panic_test=False)

    def test_06_x86_64_pit_interrupt(self):
        self.run_boot_case("x86_64", interrupt_test=True)

    def test_07_aarch64_normal_boot(self):
        self.run_boot_case("aarch64", panic_test=False)

    def test_08_aarch64_gic_interrupt(self):
        self.run_boot_case("aarch64", interrupt_test=True)

    def test_09_x86_64_memory(self):
        self.run_boot_case("x86_64", memory_test=True)

    def test_10_aarch64_memory(self):
        self.run_boot_case("aarch64", memory_test=True)

    def test_11_x86_64_timer(self):
        self.run_boot_case("x86_64", timer_test=True)

    def test_12_aarch64_timer(self):
        self.run_boot_case("aarch64", timer_test=True)


if __name__ == "__main__":
    unittest.main()
