#ifndef FEATHEROS_USER_TEXT_TOOLS_H
#define FEATHEROS_USER_TEXT_TOOLS_H

#include <stddef.h>

// What head, tail, wc, grep and sort share.
namespace FeatherOS::User
{

struct TextCounts
{
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
};

// Adds text to counts. in_word carries across calls, so a word split between two reads counts
// once; words are runs of anything but spaces, tabs and newlines.
void CountText(const char* text, size_t length, TextCounts* counts, bool* in_word);

bool ContainsText(const char* line, const char* pattern, bool ignore_case);

// sort's order: by bytes, or with numeric by the number each line starts with (a line without
// one counts as 0), ties going by bytes.
int CompareLines(const char* left, const char* right, bool numeric);

// Reads single-dash options such as -i or -ic until the first operand or "--", setting
// letters[letter - 'a'] for each. Returns the first operand's index, or -1 for a letter outside
// allowed, which *bad then holds.
int ParseFlags(int argc, char** argv, const char* allowed, bool (&letters)[26], char* bad);

// The last count lines given to Add, oldest first, for tail.
class LastLines
{
public:
    explicit LastLines(size_t count);
    ~LastLines();
    LastLines(const LastLines&) = delete;
    LastLines& operator=(const LastLines&) = delete;

    // False when there was no memory for the copy.
    bool Add(const char* line);
    size_t Count() const;
    const char* Line(size_t index) const;

private:
    char** m_lines;
    size_t m_capacity;
    size_t m_count;
    // Where the oldest line is once the ring is full.
    size_t m_oldest;
};

} // namespace FeatherOS::User

#endif
