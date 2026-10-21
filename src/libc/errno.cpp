#include <errno.h>

namespace
{

int error_number = 0;

} // namespace

extern "C" int* __errno_location()
{
    return &error_number;
}
