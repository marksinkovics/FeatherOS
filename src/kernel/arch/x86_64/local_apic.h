#ifndef FEATHEROS_X86_64_LOCAL_APIC_H
#define FEATHEROS_X86_64_LOCAL_APIC_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::LocalApic
{

constexpr uint8_t IpiVector = 48;
constexpr uint8_t TimerVector = 49;
constexpr uint8_t SpuriousVector = 0xFF;

using Handler = void (*)();

bool InitializeBootCpu();
void InitializeSecondaryCpu();
void SetIpiHandler(Handler handler);
void SendIpi(uint32_t cpu_index);
bool CalibrateTimer(uint32_t calibration_ticks);
bool StartPeriodicTimer(Handler handler);
void HandleInterrupt(uint64_t vector);

} // namespace FeatherOS::Arch::X86_64::LocalApic

#endif
