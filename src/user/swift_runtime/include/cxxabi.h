#ifndef FEATHEROS_SWIFT_RUNTIME_CXXABI_H
#define FEATHEROS_SWIFT_RUNTIME_CXXABI_H

#include <stddef.h>

// The runtime includes this for a demangler fallback it never takes on ELF: Swift symbols go
// through its own demangler, and nothing calls __cxa_demangle, which is not provided.
namespace abi
{
extern "C" char* __cxa_demangle(const char* name, char* buffer, size_t* length, int* status);
}

#endif
