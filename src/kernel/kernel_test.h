#ifndef FEATHEROS_KERNEL_TEST_H
#define FEATHEROS_KERNEL_TEST_H

namespace FeatherOS::KernelTest
{

void Run(const void* platform_data);
#if defined(FEATHEROS_SMP_TEST)
void RunSmpTests();
#endif

} // namespace FeatherOS::KernelTest

#endif