#ifndef FEATHEROS_POLL_H
#define FEATHEROS_POLL_H

#include <stdint.h>

// Wakes Poll callers when a descriptor may have become ready. Waiters sample the generation, check
// their descriptors, then wait for a newer generation, so a change between the check and the wait
// is never missed.
namespace FeatherOS::Poll
{

// Something a descriptor waits for may have happened: data, space, a connection or a hang-up.
void Notify();
uint64_t Generation();
// Returns once the generation moved past seen, the deadline tick passed (0 waits without one) or
// the task was woken, such as by a signal.
void WaitForChange(uint64_t seen, uint64_t deadline_tick);

} // namespace FeatherOS::Poll

#endif
