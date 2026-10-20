#ifndef FEATHEROS_SMP_H
#define FEATHEROS_SMP_H

#include <stdint.h>

struct limine_smp_response;

namespace FeatherOS::Smp
{

uint32_t StartSecondaryCpus(limine_smp_response* response);
uint32_t OnlineCpuCount();
// Parks every other CPU with interrupts masked, for shutdown. Returns false if one did not stop
// in time.
bool StopOtherCpus();

} // namespace FeatherOS::Smp

#endif
