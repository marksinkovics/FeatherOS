#ifndef FEATHEROS_CPU_H
#define FEATHEROS_CPU_H

#include <stdint.h>

namespace FeatherOS::Cpu
{

constexpr uint32_t MaximumCpus = 8;

void InstallLocalState(uint32_t cpu_index);
uint32_t CurrentIndex();

} // namespace FeatherOS::Cpu

#endif
