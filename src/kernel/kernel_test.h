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
// Reports the shown console and each console's foreground program on serial when they change.
void StartConsoleReports();

} // namespace FeatherOS::KernelTest

#endif
