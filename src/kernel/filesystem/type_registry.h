#ifndef FEATHEROS_FILESYSTEM_TYPE_REGISTRY_H
#define FEATHEROS_FILESYSTEM_TYPE_REGISTRY_H

#include "kernel/filesystem/filesystem.h"

#include <stdint.h>

namespace FeatherOS::VFS
{

// The filesystem instances mount can name: each is a driver with its type, such as "tmpfs", and
// its source, such as "tmp", which tells instances of one type apart. Names are not copied, so
// they must outlive the registry. Its caller provides the locking.
class TypeRegistry
{
public:
    static constexpr uint32_t Capacity = 16;

    enum class Lookup : uint8_t
    {
        Found,
        UnknownType,
        UnknownSource,
    };

    // Registering the same instance again succeeds; a taken name or driver fails.
    bool Register(const char* type, const char* source, Filesystem::Driver* driver);
    Lookup Find(const char* type, const char* source, Filesystem::Driver** driver) const;
    // The names a registered driver goes by; false for any other.
    bool Describe(const Filesystem::Driver* driver, const char** type, const char** source) const;

private:
    struct Record
    {
        const char* type;
        const char* source;
        Filesystem::Driver* driver;
    };

    Record records[Capacity] = {};
    uint32_t count = 0;
};

} // namespace FeatherOS::VFS

#endif
