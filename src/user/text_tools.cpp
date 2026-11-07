#include "text_tools.h"

#include <stdlib.h>
#include <string.h>

namespace FeatherOS::User
{

void CountText(const char* text, size_t length, TextCounts* counts, bool* in_word)
{
    counts->bytes += length;
    for (size_t index = 0; index < length; ++index)
    {
        const char character = text[index];
        const bool blank = character == ' ' || character == '\t' || character == '\n';
        if (character == '\n')
        {
            ++counts->lines;
        }
        if (!blank && !*in_word)
        {
            ++counts->words;
        }
        *in_word = !blank;
    }
}

char* FixedTextPattern(const char* text)
{
    auto* pattern = static_cast<char*>(malloc(2 * strlen(text) + 1));
    if (pattern == nullptr)
    {
        return nullptr;
    }
    char* cursor = pattern;
    for (; *text != '\0'; ++text)
    {
        if (strchr(".[\\*^$", *text) != nullptr)
        {
            *cursor++ = '\\';
        }
        *cursor++ = *text;
    }
    *cursor = '\0';
    return pattern;
}

int CompareLines(const char* left, const char* right, bool numeric)
{
    if (numeric)
    {
        const double left_value = strtod(left, nullptr);
        const double right_value = strtod(right, nullptr);
        if (left_value != right_value)
        {
            return left_value < right_value ? -1 : 1;
        }
    }
    return strcmp(left, right);
}

int ParseFlags(int argc, char** argv, const char* allowed, bool (&letters)[128], char* bad)
{
    int index = 1;
    for (; index < argc && argv[index][0] == '-' && argv[index][1] != '\0'; ++index)
    {
        if (strcmp(argv[index], "--") == 0)
        {
            return index + 1;
        }
        for (const char* letter = argv[index] + 1; *letter != '\0'; ++letter)
        {
            const bool is_letter =
                (*letter >= 'a' && *letter <= 'z') || (*letter >= 'A' && *letter <= 'Z');
            if (!is_letter || strchr(allowed, *letter) == nullptr)
            {
                *bad = *letter;
                return -1;
            }
            letters[static_cast<unsigned char>(*letter)] = true;
        }
    }
    return index;
}

LastLines::LastLines(size_t count)
    : m_lines(static_cast<char**>(calloc(count == 0 ? 1 : count, sizeof(char*)))),
      m_capacity(m_lines == nullptr ? 0 : count), m_count(0), m_oldest(0)
{
}

LastLines::~LastLines()
{
    for (size_t index = 0; index < m_count; ++index)
    {
        free(m_lines[index]);
    }
    free(m_lines);
}

bool LastLines::Add(const char* line)
{
    if (m_capacity == 0)
    {
        return m_lines != nullptr;
    }
    char* copy = strdup(line);
    if (copy == nullptr)
    {
        return false;
    }
    if (m_count < m_capacity)
    {
        m_lines[m_count++] = copy;
        return true;
    }
    free(m_lines[m_oldest]);
    m_lines[m_oldest] = copy;
    m_oldest = (m_oldest + 1) % m_capacity;
    return true;
}

size_t LastLines::Count() const
{
    return m_count;
}

const char* LastLines::Line(size_t index) const
{
    return m_lines[(m_oldest + index) % (m_capacity == 0 ? 1 : m_capacity)];
}

} // namespace FeatherOS::User
