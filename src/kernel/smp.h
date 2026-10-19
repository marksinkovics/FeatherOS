#ifndef FEATHEROS_SMP_H
#define FEATHEROS_SMP_H

#include <stdint.h>

struct limine_smp_response;

namespace FeatherOS::Smp
{

uint32_t StartSecondaryCpus(limine_smp_response* response);
uint32_t OnlineCpuCount();

} // namespace FeatherOS::Smp

#endif
