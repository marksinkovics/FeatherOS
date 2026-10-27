#include <errno.h>

namespace
{

// Each thread has its own.
__thread int error_number = 0;

} // namespace

extern "C" int* __errno_location()
{
    return &error_number;
}
