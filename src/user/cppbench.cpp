#include "command_line.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The workloads of swiftbench (src/user/swiftbench), ported line by line, so their check values
// match: life's Grid and json's parser become C++ classes over the same steps.
constexpr char Help[] =
    "usage: cppbench [-n runs | -t seconds] [workload...]\n"
    "Runs the workloads swiftbench runs, written the same way in C++, and prints each one's\n"
    "check value, which matches swiftbench's, its runs and the time each took.\n"
    "  life       64x64 cells for 20 generations, on a port of life's grid\n"
    "  sieve      the primes below 1000000\n"
    "  quicksort  20000 numbers, by the quicksort swiftbench has too\n"
    "  sort       the same numbers, by the C library's qsort\n"
    "  json       a document of 500 objects, by a port of json's parser\n"
    "-t runs each workload for that many seconds (3 by default), -n that many times.";

namespace
{

constexpr long long LifeSize = 64;
constexpr int LifeGenerations = 20;
constexpr size_t SieveLimit = 1000000;
constexpr size_t SortCount = 20000;
constexpr size_t JsonObjects = 500;
constexpr size_t JsonMaximumDepth = 256;
constexpr size_t MaximumChosen = 16;

constexpr const char* WorkloadNames[] = {"life", "sieve", "quicksort", "sort", "json"};
enum Workload : unsigned char
{
    Life,
    Sieve,
    Quicksort,
    LibrarySort,
    Json,
    WorkloadCount,
};

template <typename T> T&& Move(T& value)
{
    return static_cast<T&&>(value);
}

// A growable array, as there is no std::vector. Without placement new, growing default-constructs
// the larger array and moves the elements over.
template <typename T> class Vector
{
public:
    Vector() = default;
    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

    Vector(Vector&& other) noexcept
        : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity)
    {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    Vector& operator=(Vector&& other) noexcept
    {
        if (this != &other)
        {
            delete[] m_data;
            m_data = other.m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    ~Vector()
    {
        delete[] m_data;
    }

    size_t Size() const
    {
        return m_size;
    }

    T& operator[](size_t index)
    {
        return m_data[index];
    }

    const T& operator[](size_t index) const
    {
        return m_data[index];
    }

    void Append(T value)
    {
        Reserve(m_size + 1);
        m_data[m_size++] = Move(value);
    }

    void Append(const T* values, size_t count)
    {
        Reserve(m_size + count);
        for (size_t index = 0; index < count; ++index)
        {
            m_data[m_size++] = values[index];
        }
    }

private:
    T* m_data = nullptr;
    size_t m_size = 0;
    size_t m_capacity = 0;

    void Reserve(size_t needed)
    {
        if (needed <= m_capacity)
        {
            return;
        }
        size_t grown = m_capacity == 0 ? 4 : m_capacity * 2;
        while (grown < needed)
        {
            grown *= 2;
        }
        T* larger = new T[grown];
        for (size_t index = 0; index < m_size; ++index)
        {
            larger[index] = Move(m_data[index]);
        }
        delete[] m_data;
        m_data = larger;
        m_capacity = grown;
    }
};

class SplitMix
{
public:
    explicit SplitMix(uint64_t seed) : m_state(seed) {}

    uint64_t Next()
    {
        m_state += 0x9e3779b97f4a7c15ULL;
        uint64_t value = m_state;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

private:
    uint64_t m_state;
};

// life's Grid: the edges wrap.
class Grid
{
public:
    Grid(long long columns, long long rows)
        : m_columns(columns), m_rows(rows),
          m_cells(new uint8_t[static_cast<size_t>(columns * rows)]())
    {
    }

    Grid(const Grid&) = delete;
    Grid& operator=(const Grid&) = delete;

    ~Grid()
    {
        delete[] m_cells;
    }

    long long Columns() const
    {
        return m_columns;
    }

    long long Rows() const
    {
        return m_rows;
    }

    void Set(long long column, long long row, bool live)
    {
        m_cells[Index(column, row)] = live ? 1 : 0;
    }

    unsigned long long Population() const
    {
        unsigned long long population = 0;
        for (long long index = 0; index < m_columns * m_rows; ++index)
        {
            population += m_cells[index];
        }
        return population;
    }

    void Advance()
    {
        auto* next = new uint8_t[static_cast<size_t>(m_columns * m_rows)]();
        for (long long row = 0; row < m_rows; ++row)
        {
            const long long above = (row + m_rows - 1) % m_rows * m_columns;
            const long long here = row * m_columns;
            const long long below = (row + 1) % m_rows * m_columns;
            for (long long column = 0; column < m_columns; ++column)
            {
                const long long left = (column + m_columns - 1) % m_columns;
                const long long right = (column + 1) % m_columns;
                const int neighbors = m_cells[above + left] + m_cells[above + column] +
                                      m_cells[above + right] + m_cells[here + left] +
                                      m_cells[here + right] + m_cells[below + left] +
                                      m_cells[below + column] + m_cells[below + right];
                if (neighbors == 3 || (neighbors == 2 && m_cells[here + column] != 0))
                {
                    next[here + column] = 1;
                }
            }
        }
        delete[] m_cells;
        m_cells = next;
    }

private:
    long long m_columns;
    long long m_rows;
    uint8_t* m_cells;

    long long Index(long long column, long long row) const
    {
        return (row % m_rows + m_rows) % m_rows * m_columns +
               (column % m_columns + m_columns) % m_columns;
    }
};

unsigned long long RunLife()
{
    SplitMix generator(1);
    Grid grid(LifeSize, LifeSize);
    for (long long row = 0; row < grid.Rows(); ++row)
    {
        for (long long column = 0; column < grid.Columns(); ++column)
        {
            grid.Set(column, row, generator.Next() % 4 == 0);
        }
    }
    for (int generation = 0; generation < LifeGenerations; ++generation)
    {
        grid.Advance();
    }
    return grid.Population();
}

unsigned long long RunSieve()
{
    auto* composite = new bool[SieveLimit]();
    unsigned long long primes = 0;
    for (size_t number = 2; number < SieveLimit; ++number)
    {
        if (!composite[number])
        {
            ++primes;
            for (size_t multiple = number * number; multiple < SieveLimit; multiple += number)
            {
                composite[multiple] = true;
            }
        }
    }
    delete[] composite;
    return primes;
}

// Depends on every value's place, so a wrong order shows.
unsigned long long SortCheck(const uint32_t* values, size_t count)
{
    uint32_t sum = 0;
    for (size_t index = 0; index < count; ++index)
    {
        sum += values[index] * static_cast<uint32_t>(index + 1);
    }
    return sum;
}

void Swap(uint32_t* values, size_t left, size_t right)
{
    const uint32_t value = values[left];
    values[left] = values[right];
    values[right] = value;
}

// Leaves ranges of at most 16 values for one insertion sort over the whole array.
void QuicksortRange(uint32_t* values, size_t low, size_t high)
{
    while (high - low > 16)
    {
        const size_t middle = low + (high - low) / 2;
        if (values[middle] < values[low])
        {
            Swap(values, middle, low);
        }
        if (values[high - 1] < values[low])
        {
            Swap(values, high - 1, low);
        }
        if (values[high - 1] < values[middle])
        {
            Swap(values, high - 1, middle);
        }
        // Hoare's partition, which the median of three keeps inside the range.
        const uint32_t pivot = values[middle];
        size_t left = low - 1;
        size_t right = high;
        while (true)
        {
            do
            {
                ++left;
            } while (values[left] < pivot);
            do
            {
                --right;
            } while (values[right] > pivot);
            if (left >= right)
            {
                break;
            }
            Swap(values, left, right);
        }
        if (right + 1 - low < high - right - 1)
        {
            QuicksortRange(values, low, right + 1);
            low = right + 1;
        }
        else
        {
            QuicksortRange(values, right + 1, high);
            high = right + 1;
        }
    }
}

void InsertionSort(uint32_t* values, size_t count)
{
    for (size_t index = 1; index < count; ++index)
    {
        const uint32_t value = values[index];
        size_t position = index;
        while (position > 0 && values[position - 1] > value)
        {
            values[position] = values[position - 1];
            --position;
        }
        values[position] = value;
    }
}

uint32_t* CopyValues(const uint32_t* original)
{
    auto* values = new uint32_t[SortCount];
    for (size_t index = 0; index < SortCount; ++index)
    {
        values[index] = original[index];
    }
    return values;
}

unsigned long long RunQuicksort(const uint32_t* original)
{
    uint32_t* values = CopyValues(original);
    QuicksortRange(values, 0, SortCount);
    InsertionSort(values, SortCount);
    const unsigned long long check = SortCheck(values, SortCount);
    delete[] values;
    return check;
}

int CompareNumbers(const void* left, const void* right)
{
    const uint32_t first = *static_cast<const uint32_t*>(left);
    const uint32_t second = *static_cast<const uint32_t*>(right);
    return first < second ? -1 : first > second ? 1 : 0;
}

unsigned long long RunLibrarySort(const uint32_t* original)
{
    uint32_t* values = CopyValues(original);
    qsort(values, SortCount, sizeof(uint32_t), CompareNumbers);
    const unsigned long long check = SortCheck(values, SortCount);
    delete[] values;
    return check;
}

enum class JsonKind : unsigned char
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonMember;

// json's JSONValue: numbers keep their text, strings hold UTF-8 bytes.
struct JsonValue
{
    JsonKind kind = JsonKind::Null;
    bool truth = false;
    Vector<uint8_t> text;
    Vector<JsonValue> elements;
    Vector<JsonMember> members;
};

struct JsonMember
{
    Vector<uint8_t> key;
    JsonValue value;
};

bool SameBytes(const Vector<uint8_t>& left, const Vector<uint8_t>& right)
{
    if (left.Size() != right.Size())
    {
        return false;
    }
    for (size_t index = 0; index < left.Size(); ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

// A repeated key keeps its first place and its last value. json switches to a dictionary past 8
// members, which the benchmark's objects never have, so the port only searches in order.
void AddMember(Vector<JsonMember>* members, JsonMember member)
{
    for (size_t index = 0; index < members->Size(); ++index)
    {
        if (SameBytes((*members)[index].key, member.key))
        {
            (*members)[index].value = Move(member.value);
            return;
        }
    }
    members->Append(Move(member));
}

bool IsDigit(int byte)
{
    return byte >= '0' && byte <= '9';
}

bool IsLetter(int byte)
{
    return byte >= 0 && (byte | 0x20) >= 'a' && (byte | 0x20) <= 'z';
}

int HexDigitValue(int byte)
{
    if (IsDigit(byte))
    {
        return byte - '0';
    }
    const int lower = byte | 0x20;
    return byte >= 0 && lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

void AppendUtf8(uint32_t scalar, Vector<uint8_t>* text)
{
    if (scalar < 0x80)
    {
        text->Append(static_cast<uint8_t>(scalar));
    }
    else if (scalar < 0x800)
    {
        text->Append(static_cast<uint8_t>(0xc0 | scalar >> 6));
        text->Append(static_cast<uint8_t>(0x80 | (scalar & 0x3f)));
    }
    else if (scalar < 0x10000)
    {
        text->Append(static_cast<uint8_t>(0xe0 | scalar >> 12));
        text->Append(static_cast<uint8_t>(0x80 | (scalar >> 6 & 0x3f)));
        text->Append(static_cast<uint8_t>(0x80 | (scalar & 0x3f)));
    }
    else
    {
        text->Append(static_cast<uint8_t>(0xf0 | scalar >> 18));
        text->Append(static_cast<uint8_t>(0x80 | (scalar >> 12 & 0x3f)));
        text->Append(static_cast<uint8_t>(0x80 | (scalar >> 6 & 0x3f)));
        text->Append(static_cast<uint8_t>(0x80 | (scalar & 0x3f)));
    }
}

// json's JSONParser, RFC 8259 over bytes. Errors keep a fixed message and the offset rather than
// describing what was found, which only matters once parsing has failed.
class JsonParser
{
public:
    JsonParser(const uint8_t* bytes, size_t size) : m_bytes(bytes), m_size(size) {}

    // False at the end of the input, or after an error, which Error then names.
    bool Next(JsonValue* value)
    {
        SkipWhitespace();
        if (m_offset == m_size)
        {
            return false;
        }
        return ParseValue(value);
    }

    const char* Error() const
    {
        return m_error;
    }

    size_t Offset() const
    {
        return m_offset;
    }

private:
    const uint8_t* m_bytes;
    size_t m_size;
    size_t m_offset = 0;
    size_t m_depth = 0;
    const char* m_error = nullptr;

    bool Fail(const char* message)
    {
        m_error = message;
        return false;
    }

    int Peek() const
    {
        return m_offset < m_size ? m_bytes[m_offset] : -1;
    }

    bool ParseValue(JsonValue* value)
    {
        SkipWhitespace();
        switch (Peek())
        {
        case '{':
            return ParseObject(value);
        case '[':
            return ParseArray(value);
        case '"':
            value->kind = JsonKind::String;
            return ParseString(&value->text);
        case 't':
            value->kind = JsonKind::Bool;
            value->truth = true;
            return ExpectLiteral("true");
        case 'f':
            value->kind = JsonKind::Bool;
            return ExpectLiteral("false");
        case 'n':
            value->kind = JsonKind::Null;
            return ExpectLiteral("null");
        default:
            if (Peek() == '-' || IsDigit(Peek()))
            {
                value->kind = JsonKind::Number;
                return ParseNumber(&value->text);
            }
            return Fail("expected a value");
        }
    }

    bool ParseObject(JsonValue* value)
    {
        if (!Enter())
        {
            return false;
        }
        ++m_offset;
        value->kind = JsonKind::Object;
        SkipWhitespace();
        if (Peek() == '}')
        {
            ++m_offset;
            --m_depth;
            return true;
        }
        while (true)
        {
            SkipWhitespace();
            if (Peek() != '"')
            {
                return Fail("expected a member name");
            }
            JsonMember member;
            if (!ParseString(&member.key))
            {
                return false;
            }
            SkipWhitespace();
            if (Peek() != ':')
            {
                return Fail("expected ':' after a member name");
            }
            ++m_offset;
            if (!ParseValue(&member.value))
            {
                return false;
            }
            AddMember(&value->members, Move(member));
            SkipWhitespace();
            if (Peek() == ',')
            {
                ++m_offset;
            }
            else if (Peek() == '}')
            {
                ++m_offset;
                --m_depth;
                return true;
            }
            else
            {
                return Fail("expected ',' or '}' after a member");
            }
        }
    }

    bool ParseArray(JsonValue* value)
    {
        if (!Enter())
        {
            return false;
        }
        ++m_offset;
        value->kind = JsonKind::Array;
        SkipWhitespace();
        if (Peek() == ']')
        {
            ++m_offset;
            --m_depth;
            return true;
        }
        while (true)
        {
            JsonValue element;
            if (!ParseValue(&element))
            {
                return false;
            }
            value->elements.Append(Move(element));
            SkipWhitespace();
            if (Peek() == ',')
            {
                ++m_offset;
            }
            else if (Peek() == ']')
            {
                ++m_offset;
                --m_depth;
                return true;
            }
            else
            {
                return Fail("expected ',' or ']' after an element");
            }
        }
    }

    bool Enter()
    {
        ++m_depth;
        return m_depth <= JsonMaximumDepth || Fail("nested too deeply");
    }

    bool ParseString(Vector<uint8_t>* text)
    {
        ++m_offset;
        while (true)
        {
            const size_t start = m_offset;
            while (m_offset < m_size)
            {
                const uint8_t byte = m_bytes[m_offset];
                if (byte == '"' || byte == '\\' || byte < 0x20 || byte >= 0x80)
                {
                    break;
                }
                ++m_offset;
            }
            text->Append(m_bytes + start, m_offset - start);
            const int byte = Peek();
            if (byte < 0)
            {
                return Fail("unterminated string");
            }
            if (byte == '"')
            {
                ++m_offset;
                return true;
            }
            if (byte == '\\')
            {
                if (!ParseEscape(text))
                {
                    return false;
                }
            }
            else if (byte >= 0x80)
            {
                size_t length = 0;
                if (!ValidateUtf8Sequence(&length))
                {
                    return false;
                }
                text->Append(m_bytes + m_offset, length);
                m_offset += length;
            }
            else
            {
                return Fail("control character in a string");
            }
        }
    }

    // Surrogate errors point at the escape that is wrong.
    bool ParseEscape(Vector<uint8_t>* text)
    {
        const size_t start = m_offset;
        ++m_offset;
        const int byte = Peek();
        if (byte < 0)
        {
            return Fail("unterminated string");
        }
        int simple = -1;
        switch (byte)
        {
        case '"':
        case '\\':
        case '/':
            simple = byte;
            break;
        case 'b':
            simple = 0x08;
            break;
        case 'f':
            simple = 0x0c;
            break;
        case 'n':
            simple = '\n';
            break;
        case 'r':
            simple = '\r';
            break;
        case 't':
            simple = '\t';
            break;
        default:
            break;
        }
        if (simple >= 0)
        {
            text->Append(static_cast<uint8_t>(simple));
            ++m_offset;
            return true;
        }
        if (byte != 'u')
        {
            return Fail("unknown escape");
        }
        m_offset = start;
        uint32_t scalar = 0;
        if (!ParseHexEscape(&scalar))
        {
            return false;
        }
        if (scalar >= 0xdc00 && scalar <= 0xdfff)
        {
            m_offset = start;
            return Fail("low surrogate without a high one");
        }
        if (scalar >= 0xd800 && scalar <= 0xdbff)
        {
            const size_t low_start = m_offset;
            if (m_offset + 1 >= m_size || m_bytes[m_offset] != '\\' || m_bytes[m_offset + 1] != 'u')
            {
                m_offset = start;
                return Fail("high surrogate without a low one");
            }
            uint32_t low = 0;
            if (!ParseHexEscape(&low))
            {
                return false;
            }
            if (low < 0xdc00 || low > 0xdfff)
            {
                m_offset = low_start;
                return Fail("high surrogate without a low one");
            }
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00);
        }
        AppendUtf8(scalar, text);
        return true;
    }

    // Reads \uXXXX at the offset.
    bool ParseHexEscape(uint32_t* value)
    {
        m_offset += 2;
        *value = 0;
        for (int digit_index = 0; digit_index < 4; ++digit_index)
        {
            const int digit = HexDigitValue(Peek());
            if (digit < 0)
            {
                return Fail("expected four hex digits after \\u");
            }
            *value = *value << 4 | static_cast<uint32_t>(digit);
            ++m_offset;
        }
        return true;
    }

    // The length of the well-formed UTF-8 sequence at the offset (Unicode table 3-7).
    bool ValidateUtf8Sequence(size_t* length)
    {
        const uint8_t lead = m_bytes[m_offset];
        uint8_t lowest = 0x80;
        uint8_t highest = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            *length = 2;
        }
        else if (lead == 0xe0)
        {
            *length = 3;
            lowest = 0xa0;
        }
        else if ((lead >= 0xe1 && lead <= 0xec) || lead == 0xee || lead == 0xef)
        {
            *length = 3;
        }
        else if (lead == 0xed)
        {
            *length = 3;
            highest = 0x9f;
        }
        else if (lead == 0xf0)
        {
            *length = 4;
            lowest = 0x90;
        }
        else if (lead >= 0xf1 && lead <= 0xf3)
        {
            *length = 4;
        }
        else if (lead == 0xf4)
        {
            *length = 4;
            highest = 0x8f;
        }
        else
        {
            return Fail("invalid UTF-8 byte");
        }
        for (size_t index = 1; index < *length; ++index)
        {
            const size_t position = m_offset + index;
            const uint8_t low = index == 1 ? lowest : 0x80;
            const uint8_t high = index == 1 ? highest : 0xbf;
            if (position >= m_size || m_bytes[position] < low || m_bytes[position] > high)
            {
                return Fail("invalid UTF-8 sequence");
            }
        }
        return true;
    }

    bool ParseNumber(Vector<uint8_t>* text)
    {
        const size_t start = m_offset;
        if (Peek() == '-')
        {
            ++m_offset;
        }
        if (Peek() == '0')
        {
            ++m_offset;
            if (IsDigit(Peek()))
            {
                return Fail("leading zero in a number");
            }
        }
        else if (!SkipDigits())
        {
            return false;
        }
        if (Peek() == '.')
        {
            ++m_offset;
            if (!SkipDigits())
            {
                return false;
            }
        }
        if (Peek() == 'e' || Peek() == 'E')
        {
            ++m_offset;
            if (Peek() == '+' || Peek() == '-')
            {
                ++m_offset;
            }
            if (!SkipDigits())
            {
                return false;
            }
        }
        if (!ExpectEndOfWord())
        {
            return false;
        }
        text->Append(m_bytes + start, m_offset - start);
        return true;
    }

    bool SkipDigits()
    {
        if (!IsDigit(Peek()))
        {
            return Fail("expected a digit");
        }
        while (IsDigit(Peek()))
        {
            ++m_offset;
        }
        return true;
    }

    bool ExpectLiteral(const char* literal)
    {
        const size_t start = m_offset;
        for (const char* expected = literal; *expected != '\0'; ++expected)
        {
            if (Peek() != *expected)
            {
                m_offset = start;
                return Fail("expected a value");
            }
            ++m_offset;
        }
        return ExpectEndOfWord();
    }

    // So that "truex" or "12a" are not read as a value followed by another.
    bool ExpectEndOfWord()
    {
        const int byte = Peek();
        if (IsDigit(byte) || IsLetter(byte) || byte == '.' || byte == '+' || byte == '-')
        {
            return Fail("unexpected text after a value");
        }
        return true;
    }

    void SkipWhitespace()
    {
        while (Peek() == ' ' || Peek() == '\n' || Peek() == '\t' || Peek() == '\r')
        {
            ++m_offset;
        }
    }
};

void AppendText(Vector<uint8_t>* text, const char* characters)
{
    text->Append(reinterpret_cast<const uint8_t*>(characters), strlen(characters));
}

// Objects of 8 members, so json's parser searches them in order rather than through a
// dictionary, as the port does.
Vector<uint8_t> MakeDocument()
{
    SplitMix generator(3);
    Vector<uint8_t> text;
    char buffer[96];
    AppendText(&text, "[");
    for (unsigned long long index = 0; index < JsonObjects; ++index)
    {
        const unsigned long long price = generator.Next() % 100000;
        AppendText(&text, index == 0 ? "\n  " : ",\n  ");
        snprintf(buffer, sizeof(buffer), "{\"id\": %llu, \"name\": \"item %llu\", ", index, index);
        AppendText(&text, buffer);
        snprintf(buffer, sizeof(buffer), "\"price\": %llu.%llu%llu, ", price / 100, price / 10 % 10,
                 price % 10);
        AppendText(&text, buffer);
        AppendText(&text, price % 2 == 0 ? "\"active\": true, " : "\"active\": false, ");
        AppendText(&text, "\"tags\": [\"alpha\", \"beta\\n\", \"caf\\u00e9\"], ");
        // U+00FC, ü, as raw UTF-8.
        snprintf(buffer, sizeof(buffer), "\"note\": \"line \\\"%llu\\\" \xc3\xbc ends\", ", index);
        AppendText(&text, buffer);
        AppendText(&text, "\"scale\": -1.5e3, \"parent\": null}");
    }
    AppendText(&text, "\n]\n");
    return text;
}

void Tally(const JsonValue& value, unsigned long long* values, unsigned long long* bytes)
{
    ++*values;
    switch (value.kind)
    {
    case JsonKind::Null:
    case JsonKind::Bool:
        break;
    case JsonKind::Number:
    case JsonKind::String:
        *bytes += value.text.Size();
        break;
    case JsonKind::Array:
        for (size_t index = 0; index < value.elements.Size(); ++index)
        {
            Tally(value.elements[index], values, bytes);
        }
        break;
    case JsonKind::Object:
        for (size_t index = 0; index < value.members.Size(); ++index)
        {
            *bytes += value.members[index].key.Size();
            Tally(value.members[index].value, values, bytes);
        }
        break;
    }
}

unsigned long long RunJson(const Vector<uint8_t>& document)
{
    JsonParser parser(&document[0], document.Size());
    Vector<JsonValue> parsed;
    while (true)
    {
        JsonValue value;
        if (!parser.Next(&value))
        {
            break;
        }
        parsed.Append(Move(value));
    }
    if (parser.Error() != nullptr)
    {
        fprintf(stderr, "cppbench: json: offset %zu: %s\n", parser.Offset(), parser.Error());
        exit(1);
    }
    unsigned long long values = 0;
    unsigned long long bytes = 0;
    for (size_t index = 0; index < parsed.Size(); ++index)
    {
        Tally(parsed[index], &values, &bytes);
    }
    return document.Size() + values + bytes;
}

struct Limit
{
    bool by_runs;
    unsigned long long amount;
};

// The monotonic clock advances in 10 ms ticks.
unsigned long long Milliseconds()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned long long>(now.tv_sec) * 1000 +
           static_cast<unsigned long long>(now.tv_nsec) / 1000000;
}

template <typename Body> void Measure(const char* name, const Limit& limit, Body body)
{
    const unsigned long long start = Milliseconds();
    unsigned long long runs = 0;
    unsigned long long check = 0;
    unsigned long long elapsed = 0;
    do
    {
        check = body();
        ++runs;
        elapsed = Milliseconds() - start;
    } while ((limit.by_runs ? runs : elapsed) < limit.amount);
    printf("%s: check %llu, %llu run%s in %llu ms, %llu us each\n", name, check, runs,
           runs == 1 ? "" : "s", elapsed, elapsed * 1000 / runs);
}

void Run(Workload workload, const Limit& limit, const uint32_t* sort_values,
         const Vector<uint8_t>& document)
{
    const char* name = WorkloadNames[workload];
    switch (workload)
    {
    case Life:
        Measure(name, limit, RunLife);
        break;
    case Sieve:
        Measure(name, limit, RunSieve);
        break;
    case Quicksort:
        Measure(name, limit, [sort_values] { return RunQuicksort(sort_values); });
        break;
    case LibrarySort:
        Measure(name, limit, [sort_values] { return RunLibrarySort(sort_values); });
        break;
    default:
        Measure(name, limit, [&document] { return RunJson(document); });
        break;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    Limit limit = {false, 3000};
    Workload chosen[MaximumChosen];
    size_t chosen_count = 0;
    for (int index = 1; index < argc; ++index)
    {
        const bool by_runs = strcmp(argv[index], "-n") == 0;
        if (by_runs || strcmp(argv[index], "-t") == 0)
        {
            unsigned long long count = 0;
            if (index + 1 == argc || !FeatherOS::User::ParseUnsigned(argv[index + 1], &count) ||
                count == 0 || count > 1000000)
            {
                fprintf(stderr, "cppbench: %s needs a positive count; see cppbench -h\n",
                        argv[index]);
                return 1;
            }
            limit = {by_runs, by_runs ? count : count * 1000};
            ++index;
            continue;
        }
        size_t workload = 0;
        while (workload < WorkloadCount && strcmp(argv[index], WorkloadNames[workload]) != 0)
        {
            ++workload;
        }
        if (workload == WorkloadCount)
        {
            fprintf(stderr, "cppbench: unknown workload %s; see cppbench -h\n", argv[index]);
            return 1;
        }
        chosen[chosen_count++] = static_cast<Workload>(workload);
    }
    if (chosen_count == 0)
    {
        for (size_t workload = 0; workload < WorkloadCount; ++workload)
        {
            chosen[chosen_count++] = static_cast<Workload>(workload);
        }
    }

    SplitMix generator(2);
    auto* sort_values = new uint32_t[SortCount];
    for (size_t index = 0; index < SortCount; ++index)
    {
        sort_values[index] = static_cast<uint32_t>(generator.Next());
    }
    const Vector<uint8_t> document = MakeDocument();
    for (size_t index = 0; index < chosen_count; ++index)
    {
        Run(chosen[index], limit, sort_values, document);
    }
    delete[] sort_values;
    return 0;
}
