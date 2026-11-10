#ifndef FEATHEROS_LIBC_WIDE_DIVISION_H
#define FEATHEROS_LIBC_WIDE_DIVISION_H

#include <stdint.h>

namespace FeatherOS::LibC
{

// A 128-bit unsigned integer as two halves, for code that must not use 128-bit operators: GCC
// turns those into calls to the very functions built on this.
struct Wide
{
    uint64_t high;
    uint64_t low;
};

// divisor must not be zero.
Wide DivideWide(Wide dividend, Wide divisor, Wide* remainder);

} // namespace FeatherOS::LibC

#endif
