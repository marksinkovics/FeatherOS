#ifndef FEATHEROS_LIBC_ENVIRONMENT_H
#define FEATHEROS_LIBC_ENVIRONMENT_H

namespace FeatherOS::LibC
{

// Called by _start with the block the kernel passed.
void LoadEnvironment(const char* block);

} // namespace FeatherOS::LibC

#endif
