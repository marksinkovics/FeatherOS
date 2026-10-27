#ifndef FEATHEROS_LIBC_BIG_INTEGER_H
#define FEATHEROS_LIBC_BIG_INTEGER_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// An unsigned integer of up to Capacity 32-bit words, least significant first. It holds every
// finite value of the widest float (2^16384 for a 128-bit long double) times the power of ten
// that decimal conversion multiplies by, so printf and strtod can convert exactly.
class BigInteger
{
public:
    static constexpr size_t Capacity = 640;

    void Set(uint64_t value);
    bool IsZero() const;
    size_t BitLength() const;
    bool Bit(size_t index) const;
    // True if any bit below index is set.
    bool AnyBitBelow(size_t index) const;
    // The bits [start, start + 64) as an integer.
    uint64_t Bits64(size_t start) const;
    // False if the result would not fit.
    bool ShiftLeft(size_t bits);

    void ShiftRight(size_t bits);
    // Keeps only the bits below index.
    void KeepBitsBelow(size_t index);

    bool MultiplyAdd(uint32_t factor, uint32_t addend);
    bool MultiplyByPowerOfTen(uint32_t exponent);
    // Divides in place and returns the remainder.
    uint32_t DivideSmall(uint32_t divisor);

    int Compare(const BigInteger& other) const;
    // Requires other <= *this.
    void Subtract(const BigInteger& other);

    void SetBit(size_t index);

private:
    void Trim();
    uint32_t words[Capacity];
    size_t count;
};

} // namespace FeatherOS::LibC

#endif
