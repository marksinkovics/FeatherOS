#ifndef FEATHEROS_KERNEL_TEST_H
#define FEATHEROS_KERNEL_TEST_H

#include "kernel/kernel_test_mode.h"

#include <stdint.h>

namespace FeatherOS::KernelTest
{

void Run(const void* platform_data);
void RunSmpTests();
void RunStorageTests(uint64_t hhdm_offset);
void RunNetworkTests();
void RunPciTests(const void* platform_data);
// Waits for the first probed USB drive, then writes and reads it through the drive interface.
void StartUsbStorageTests();
// Waits for the first NVMe namespace, then mounts its volumes and writes and reads it raw.
void StartNvmeTests();
// Waits for the first AHCI disk and runs the same checks as the NVMe test on it.
void StartAhciTests();
void StartStoragePolicyTests();
// Reports the shown console and each console's foreground program on serial when they change.
void StartConsoleReports();

} // namespace FeatherOS::KernelTest

#endif
