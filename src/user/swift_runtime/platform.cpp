#include <stdio.h>
#include <string>
#include <thread>
#include <unistd.h>

// The parts of libc++'s compiled library that the runtime uses: basic_string<char>'s members,
// which its header declares extern and libc++'s own string.cpp instantiates, the hash table's
// sizes and hash, three integer conversions and the processor count. With these, nothing links a
// compiled libc++.
_LIBCPP_BEGIN_NAMESPACE_STD

#define FEATHEROS_INSTANTIATE(...) template __VA_ARGS__;
_LIBCPP_STRING_V1_EXTERN_TEMPLATE_LIST(FEATHEROS_INSTANTIATE, char)
#undef FEATHEROS_INSTANTIATE

size_t __next_prime(size_t value)
{
    for (;; ++value)
    {
        if (value < 2)
        {
            continue;
        }
        bool prime = true;
        for (size_t divisor = 2; divisor * divisor <= value; ++divisor)
        {
            if (value % divisor == 0)
            {
                prime = false;
                break;
            }
        }
        if (prime)
        {
            return value;
        }
    }
}

size_t __hash_memory(_LIBCPP_NOESCAPE const void* bytes, size_t size) noexcept
{
    return __murmur2_or_cityhash<size_t>()(bytes, size);
}

namespace
{

template <typename Value> string Format(const char* format, Value value)
{
    char text[32];
    snprintf(text, sizeof(text), format, value);
    return text;
}

} // namespace

string to_string(int value)
{
    return Format("%d", value);
}

string to_string(unsigned value)
{
    return Format("%u", value);
}

string to_string(unsigned long value)
{
    return Format("%lu", value);
}

unsigned thread::hardware_concurrency() noexcept
{
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<unsigned>(count) : 1;
}

_LIBCPP_END_NAMESPACE_STD
