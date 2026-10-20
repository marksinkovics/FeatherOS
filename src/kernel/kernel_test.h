#ifndef FEATHEROS_KERNEL_TEST_H
#define FEATHEROS_KERNEL_TEST_H

namespace FeatherOS::KernelTest
{

void Run(const void* platform_data);
#if defined(FEATHEROS_SMP_TEST)
void RunSmpTests();
#endif
#if defined(FEATHEROS_STORAGE_TEST)
void RunStorageTests();
#endif

} // namespace FeatherOS::KernelTest

#endif