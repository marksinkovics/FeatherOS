#include "big_integer.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

void BigInteger::Set(uint64_t value)
{
    words[0] = static_cast<uint32_t>(value);
    words[1] = static_cast<uint32_t>(value >> 32);
    count = words[1] != 0 ? 2 : words[0] != 0 ? 1 : 0;
}

bool BigInteger::IsZero() const
{
    return count == 0;
}

size_t BigInteger::BitLength() const
{
    if (count == 0)
    {
        return 0;
    }
    size_t bits = (count - 1) * 32;
    for (uint32_t top = words[count - 1]; top != 0; top >>= 1)
    {
        ++bits;
    }
    return bits;
}

bool BigInteger::Bit(size_t index) const
{
    const size_t word = index / 32;
    return word < count && ((words[word] >> (index % 32)) & 1U) != 0;
}

bool BigInteger::AnyBitBelow(size_t index) const
{
    const size_t whole_words = index / 32;
    for (size_t word = 0; word < whole_words && word < count; ++word)
    {
        if (words[word] != 0)
        {
            return true;
        }
    }
    const uint32_t mask = (1U << (index % 32)) - 1;
    return whole_words < count && (words[whole_words] & mask) != 0;
}

uint64_t BigInteger::Bits64(size_t start) const
{
    uint64_t value = 0;
    for (size_t bit = 0; bit < 64; ++bit)
    {
        if (Bit(start + bit))
        {
            value |= 1ULL << bit;
        }
    }
    return value;
}

bool BigInteger::ShiftLeft(size_t bits)
{
    if (count == 0)
    {
        return true;
    }
    const size_t word_shift = bits / 32;
    const uint32_t bit_shift = bits % 32;
    if (count + word_shift + 1 > Capacity)
    {
        return false;
    }
    words[count + word_shift] = 0;
    for (size_t index = count; index-- > 0;)
    {
        const uint64_t shifted = static_cast<uint64_t>(words[index]) << bit_shift;
        words[index + word_shift + 1] |= static_cast<uint32_t>(shifted >> 32);
        words[index + word_shift] = static_cast<uint32_t>(shifted);
    }
    for (size_t index = 0; index < word_shift; ++index)
    {
        words[index] = 0;
    }
    count += word_shift + 1;
    Trim();
    return true;
}

void BigInteger::ShiftRight(size_t bits)
{
    const size_t word_shift = bits / 32;
    const uint32_t bit_shift = bits % 32;
    if (word_shift >= count)
    {
        count = 0;
        return;
    }
    for (size_t index = 0; index + word_shift < count; ++index)
    {
        uint64_t value = words[index + word_shift] >> bit_shift;
        if (bit_shift != 0 && index + word_shift + 1 < count)
        {
            value |= static_cast<uint64_t>(words[index + word_shift + 1]) << (32 - bit_shift);
        }
        words[index] = static_cast<uint32_t>(value);
    }
    count -= word_shift;
    Trim();
}

void BigInteger::KeepBitsBelow(size_t index)
{
    const size_t whole_words = index / 32;
    if (whole_words >= count)
    {
        return;
    }
    words[whole_words] &= (1U << (index % 32)) - 1;
    count = whole_words + 1;
    Trim();
}

bool BigInteger::MultiplyAdd(uint32_t factor, uint32_t addend)
{
    uint64_t carry = addend;
    for (size_t index = 0; index < count; ++index)
    {
        const uint64_t product = static_cast<uint64_t>(words[index]) * factor + carry;
        words[index] = static_cast<uint32_t>(product);
        carry = product >> 32;
    }
    if (carry != 0)
    {
        if (count == Capacity)
        {
            return false;
        }
        words[count++] = static_cast<uint32_t>(carry);
    }
    return true;
}

bool BigInteger::MultiplyByPowerOfTen(uint32_t exponent)
{
    for (; exponent >= 9; exponent -= 9)
    {
        if (!MultiplyAdd(1000000000U, 0))
        {
            return false;
        }
    }
    uint32_t factor = 1;
    for (; exponent > 0; --exponent)
    {
        factor *= 10;
    }
    return MultiplyAdd(factor, 0);
}

uint32_t BigInteger::DivideSmall(uint32_t divisor)
{
    uint64_t remainder = 0;
    for (size_t index = count; index-- > 0;)
    {
        const uint64_t value = (remainder << 32) | words[index];
        words[index] = static_cast<uint32_t>(value / divisor);
        remainder = value % divisor;
    }
    Trim();
    return static_cast<uint32_t>(remainder);
}

int BigInteger::Compare(const BigInteger& other) const
{
    if (count != other.count)
    {
        return count < other.count ? -1 : 1;
    }
    for (size_t index = count; index-- > 0;)
    {
        if (words[index] != other.words[index])
        {
            return words[index] < other.words[index] ? -1 : 1;
        }
    }
    return 0;
}

void BigInteger::Subtract(const BigInteger& other)
{
    int64_t borrow = 0;
    for (size_t index = 0; index < count; ++index)
    {
        int64_t value = static_cast<int64_t>(words[index]) - borrow -
                        (index < other.count ? static_cast<int64_t>(other.words[index]) : 0);
        borrow = value < 0 ? 1 : 0;
        words[index] = static_cast<uint32_t>(value + (borrow << 32));
    }
    Trim();
}

void BigInteger::SetBit(size_t index)
{
    const size_t word = index / 32;
    while (count <= word)
    {
        words[count++] = 0;
    }
    words[word] |= 1U << (index % 32);
}

void BigInteger::Trim()
{
    while (count > 0 && words[count - 1] == 0)
    {
        --count;
    }
}

} // namespace FeatherOS::LibC
