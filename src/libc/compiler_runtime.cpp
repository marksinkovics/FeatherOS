#include <stdint.h>

// GCC calls libgcc for operations the target has no instruction for, and programs do not link
// libgcc. At -O2, AArch64 shifts unsigned __int128 values right through __lshrti3. The shift is
// done on 64-bit halves, since a 128-bit shift here would call this function again.
extern "C" unsigned __int128 __lshrti3(unsigned __int128 value, int shift)
{
    constexpr int HalfBits = 64;
    auto low = static_cast<uint64_t>(value);
    auto high = static_cast<uint64_t>(value >> HalfBits);
    if (shift >= HalfBits)
    {
        low = high >> (shift - HalfBits);
        high = 0;
    }
    else if (shift > 0)
    {
        low = (low >> shift) | (high << (HalfBits - shift));
        high >>= shift;
    }
    return static_cast<unsigned __int128>(high) << HalfBits | low;
}
