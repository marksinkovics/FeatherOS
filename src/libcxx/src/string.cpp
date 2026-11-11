// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/charconv"
#include "libcxx/include/string"
#else
#include <charconv>
#include <string>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

template class basic_string<char>;

namespace
{

// The strto* functions skip spaces and take a sign, as sto* must; errno tells overflow.
template <class Result, class Parse>
Result ParseNumber(const string& text, size_t* end, const char* name, Parse parse)
{
    const char* const start = text.c_str();
    char* stop = nullptr;
    const int saved = errno;
    errno = 0;
    const auto value = parse(start, &stop);
    if (stop == start)
    {
        __detail::LibraryAbort(name);
    }
    if (errno == ERANGE)
    {
        __detail::LibraryAbort(name);
    }
    errno = saved;
    if (end != nullptr)
    {
        *end = static_cast<size_t>(stop - start);
    }
    return static_cast<Result>(value);
}

template <class T> string IntegerToString(T value)
{
    char buffer[24];
    const to_chars_result result = _FSTD::to_chars(buffer, buffer + sizeof(buffer), value);
    return string(buffer, static_cast<size_t>(result.ptr - buffer));
}

// to_string of a floating-point value is printf's %f, which for a large long double runs to
// thousands of digits, so the length is asked for first.
template <class T> string FloatToString(const char* format, T value)
{
    string result;
    const int length = snprintf(nullptr, 0, format, value);
    result.resize_and_overwrite(static_cast<size_t>(length),
                                [&](char* buffer, size_t count)
                                {
                                    snprintf(buffer, count + 1, format, value);
                                    return count;
                                });
    return result;
}

} // namespace

int stoi(const string& text, size_t* end, int base)
{
    const long value = ParseNumber<long>(text, end, "stoi: not a number or out of range",
                                         [base](const char* start, char** stop)
                                         { return strtol(start, stop, base); });
    if (value < -2147483647L - 1 || value > 2147483647L)
    {
        __detail::LibraryAbort("stoi: not a number or out of range");
    }
    return static_cast<int>(value);
}

long stol(const string& text, size_t* end, int base)
{
    return ParseNumber<long>(text, end, "stol: not a number or out of range",
                             [base](const char* start, char** stop)
                             { return strtol(start, stop, base); });
}

long long stoll(const string& text, size_t* end, int base)
{
    return ParseNumber<long long>(text, end, "stoll: not a number or out of range",
                                  [base](const char* start, char** stop)
                                  { return strtoll(start, stop, base); });
}

unsigned long stoul(const string& text, size_t* end, int base)
{
    return ParseNumber<unsigned long>(text, end, "stoul: not a number or out of range",
                                      [base](const char* start, char** stop)
                                      { return strtoul(start, stop, base); });
}

unsigned long long stoull(const string& text, size_t* end, int base)
{
    return ParseNumber<unsigned long long>(text, end, "stoull: not a number or out of range",
                                           [base](const char* start, char** stop)
                                           { return strtoull(start, stop, base); });
}

float stof(const string& text, size_t* end)
{
    return ParseNumber<float>(text, end, "stof: not a number or out of range",
                              [](const char* start, char** stop) { return strtof(start, stop); });
}

double stod(const string& text, size_t* end)
{
    return ParseNumber<double>(text, end, "stod: not a number or out of range",
                               [](const char* start, char** stop) { return strtod(start, stop); });
}

long double stold(const string& text, size_t* end)
{
    return ParseNumber<long double>(text, end, "stold: not a number or out of range",
                                    [](const char* start, char** stop)
                                    { return strtold(start, stop); });
}

string to_string(int value)
{
    return IntegerToString(value);
}
string to_string(unsigned value)
{
    return IntegerToString(value);
}
string to_string(long value)
{
    return IntegerToString(value);
}
string to_string(unsigned long value)
{
    return IntegerToString(value);
}
string to_string(long long value)
{
    return IntegerToString(value);
}
string to_string(unsigned long long value)
{
    return IntegerToString(value);
}
string to_string(float value)
{
    return FloatToString("%f", static_cast<double>(value));
}
string to_string(double value)
{
    return FloatToString("%f", value);
}
string to_string(long double value)
{
    return FloatToString("%Lf", value);
}

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
