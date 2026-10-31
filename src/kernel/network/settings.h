#ifndef FEATHEROS_NETWORK_SETTINGS_H
#define FEATHEROS_NETWORK_SETTINGS_H

#include "user/syscall_abi.h"

#include <stdint.h>

// The interface's settings as the NetworkSettings syscall exchanges them.
namespace FeatherOS::Network
{

// SyscallErrorNetworkUnreachable without an interface.
uint64_t ReadSettings(User::NetworkSettings* settings);
// Applies the parts of settings that changes names, as the syscall does, the link first, and logs
// a new IPv4 address. IPv4 fails with SyscallErrorNetworkDown while the link is down.
uint64_t ApplySettings(const User::NetworkSettings& settings, uint64_t changes);

} // namespace FeatherOS::Network

#endif
