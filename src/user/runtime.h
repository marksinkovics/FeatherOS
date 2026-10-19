#ifndef FEATHEROS_USER_RUNTIME_H
#define FEATHEROS_USER_RUNTIME_H

#include "syscall.h"

#include <stdint.h>

// Small freestanding helpers shared by the user programs.
namespace FeatherOS::User
{

inline void WriteText(const char* text)
{
    while (*text != '\0')
    {
        WriteCharacter(static_cast<uint64_t>(*text));
        ++text;
    }
}

inline void WriteLine(const char* text)
{
    WriteText(text);
    WriteCharacter('\n');
}

inline void WriteNumber(uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0)
    {
        WriteCharacter(static_cast<uint64_t>(digits[--count]));
    }
}

inline bool StringEquals(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

// Parses a whole string of decimal digits; fails on other characters or overflow.
inline bool ParseUnsignedNumber(const char* text, uint64_t* value)
{
    if (*text == '\0')
    {
        return false;
    }
    uint64_t parsed = 0;
    for (; *text != '\0'; ++text)
    {
        if (*text < '0' || *text > '9')
        {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(*text - '0');
        if (parsed > (UINT64_MAX - digit) / 10)
        {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

} // namespace FeatherOS::User

#endif
