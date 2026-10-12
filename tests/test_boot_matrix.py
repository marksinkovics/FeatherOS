import os
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = REPO_ROOT / "build.sh"


class BootMatrixTests(unittest.TestCase):
    def run_boot_case(self, arch, panic_test):
        environment = os.environ.copy()
        environment["PANIC_TEST"] = "ON" if panic_test else "OFF"
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

    def test_03_x86_64_normal_boot(self):
        self.run_boot_case("x86_64", panic_test=False)

    def test_04_aarch64_normal_boot(self):
        self.run_boot_case("aarch64", panic_test=False)


if __name__ == "__main__":
    unittest.main()
