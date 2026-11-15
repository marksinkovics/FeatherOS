#include "type_registry.h"

namespace FeatherOS::VFS
{
namespace
{

bool SameText(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

} // namespace

bool TypeRegistry::Register(const char* type, const char* source, Filesystem::Driver* driver)
{
    if (type == nullptr || source == nullptr || driver == nullptr || type[0] == '\0' ||
        source[0] == '\0')
    {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        const Record& record = records[index];
        if (SameText(record.type, type) && SameText(record.source, source))
        {
            return record.driver == driver;
        }
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        Record& record = records[index];
        if (record.driver == driver)
        {
            if (!SameText(record.type, type))
            {
                return false;
            }
            record.source = source;
            return true;
        }
    }
    if (count == Capacity)
    {
        return false;
    }
    records[count++] = {type, source, driver};
    return true;
}

void TypeRegistry::Unregister(const Filesystem::Driver* driver)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (records[index].driver == driver)
        {
            for (uint32_t next = index + 1; next < count; ++next)
            {
                records[next - 1] = records[next];
            }
            records[--count] = {};
            return;
        }
    }
}

TypeRegistry::Lookup TypeRegistry::Find(const char* type, const char* source,
                                        Filesystem::Driver** driver) const
{
    if (type == nullptr || source == nullptr || driver == nullptr)
    {
        return Lookup::UnknownType;
    }
    bool known_type = false;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (!SameText(records[index].type, type))
        {
            continue;
        }
        known_type = true;
        if (SameText(records[index].source, source))
        {
            *driver = records[index].driver;
            return Lookup::Found;
        }
    }
    return known_type ? Lookup::UnknownSource : Lookup::UnknownType;
}

bool TypeRegistry::Describe(const Filesystem::Driver* driver, const char** type,
                            const char** source) const
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (records[index].driver == driver)
        {
            *type = records[index].type;
            *source = records[index].source;
            return true;
        }
    }
    return false;
}

} // namespace FeatherOS::VFS
