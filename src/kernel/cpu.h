#ifndef FEATHEROS_CPU_H
#define FEATHEROS_CPU_H

#include <stdint.h>

namespace FeatherOS::Cpu
{

constexpr uint32_t MaximumCpus = 8;

void InstallLocalState(uint32_t cpu_index);
uint32_t CurrentIndex();

// The task running on this CPU, which the scheduler stores here so that finding it is one load
// through the CPU's own base register. Reading the index and then an array slot is two steps, and
// a task preempted between them and resumed on another CPU would read the old CPU's slot.
void* CurrentTask();
void SetCurrentTask(void* task);
// Another CPU's current task, for the scheduler to read under its lock.
void* TaskOfCpu(uint32_t cpu_index);

} // namespace FeatherOS::Cpu

#endif
