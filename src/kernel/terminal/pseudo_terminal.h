#ifndef FEATHEROS_TERMINAL_PSEUDO_TERMINAL_H
#define FEATHEROS_TERMINAL_PSEUDO_TERMINAL_H

#include "kernel/pipe.h"

#include <stdint.h>

// Pseudo-terminal pairs. The slave is a Terminals::Terminal like the console, whose output the
// master reads and whose input the master writes. A pair is in use from its master's open until
// the master and every slave descriptor are closed. Closing the master hangs the terminal up.
// Functions return the value a syscall hands back. Buffers are kernel memory.
namespace FeatherOS::Terminals
{
class Terminal;
} // namespace FeatherOS::Terminals

namespace FeatherOS::PseudoTerminals
{

struct Pair;

// For the master's open: a new pair with its slave locked, or SyscallErrorNoDevice when every
// pair is in use, or SyscallErrorNoMemory.
uint64_t Create(Pair** pair);
void CloseMaster(Pair* pair);
// For /dev/pts/N: SyscallErrorNotFound for a pair not in use, SyscallErrorInputOutput while it is
// locked or its master is closed.
uint64_t OpenSlave(uint32_t number, Pair** pair);
// Another slave descriptor for terminal, such as one /dev/tty opened; null if terminal belongs
// to no pair.
Pair* AddSlave(Terminals::Terminal* terminal);
void CloseSlave(Pair* pair);
bool InUse(uint32_t number);
// Restores the settings owner left behind on every pair's terminal.
void ReleaseSettings(uint64_t owner);
uint32_t Number(const Pair* pair);
Terminals::Terminal* TerminalOf(Pair* pair);
void SetLocked(Pair* pair, bool locked);
// What programs wrote to the slave, waiting as a pipe's read end does: end of file once every
// slave descriptor that was opened is closed again.
uint64_t ReadMaster(Pair* pair, void* buffer, uint64_t length, Pipes::InterruptCheck interrupted,
                    bool nonblocking);
// The slave's input, waiting while its queue is full.
uint64_t WriteMaster(Pair* pair, const void* buffer, uint64_t length,
                     Pipes::InterruptCheck interrupted, bool nonblocking);
// User::Poll* events for the master.
int16_t PollMaster(Pair* pair);

} // namespace FeatherOS::PseudoTerminals

#endif
