#include "libc/environment.h"

#include "libc/environment_table.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

namespace
{

FeatherOS::LibC::EnvironmentTable table;

int Report(FeatherOS::LibC::EnvironmentResult result)
{
    switch (result)
    {
    case FeatherOS::LibC::EnvironmentResult::Done:
        return 0;
    case FeatherOS::LibC::EnvironmentResult::InvalidName:
        errno = EINVAL;
        return -1;
    default:
        errno = ENOMEM;
        return -1;
    }
}

} // namespace

// Programs may read environ directly; it always points at the table's entries.
char** environ = nullptr;

namespace FeatherOS::LibC
{

void LoadEnvironment(const char* block)
{
    table.Load(block);
    environ = table.Entries();
}

} // namespace FeatherOS::LibC

char* getenv(const char* name)
{
    return const_cast<char*>(table.Find(name));
}

int setenv(const char* name, const char* value, int overwrite)
{
    return Report(table.Set(name, value == nullptr ? "" : value, overwrite != 0));
}

int unsetenv(const char* name)
{
    return Report(table.Remove(name));
}
