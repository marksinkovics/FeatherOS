#ifndef FEATHEROS_ARCH_CPU_FEATURES_H
#define FEATHEROS_ARCH_CPU_FEATURES_H

#include <stdint.h>

namespace FeatherOS::Arch::CpuFeatures
{

struct Protections
{
    // x86-64 SMEP; AArch64 already marks every user page PXN.
    bool execute_protection;
    // x86-64 SMAP or AArch64 PAN: kernel accesses to user pages fault outside an access window.
    bool access_protection;
    // x86-64 RDRAND or AArch64 RNDR.
    bool hardware_random;
};

void InitializeCurrentCpu();
Protections Enabled();
bool ReadHardwareRandom(uint64_t* value);
void AllowUserAccess();
void ForbidUserAccess();

class UserAccessWindow
{
public:
    UserAccessWindow()
    {
        AllowUserAccess();
    }
    ~UserAccessWindow()
    {
        ForbidUserAccess();
    }
    UserAccessWindow(const UserAccessWindow&) = delete;
    UserAccessWindow& operator=(const UserAccessWindow&) = delete;
};

} // namespace FeatherOS::Arch::CpuFeatures

#endif
