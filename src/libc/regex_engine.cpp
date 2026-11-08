#include "libc/regex_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace FeatherOS::LibC::Regex
{
namespace
{

// Keeps relative offsets within int32_t and bounds what nested intervals can expand to.
constexpr size_t MaximumInstructions = 1 << 16;
constexpr int MaximumNesting = 128;
// A backtracking search that goes on longer gives up with NoMemory. Without back-references it
// remembers what it visited, so it only gets near this when that table would be too large.
constexpr unsigned long StepBudget = 1UL << 24;
constexpr size_t MaximumVisitedBits = 1 << 20;

template <typename T> class Array
{
public:
    Array() = default;
    ~Array()
    {
        free(m_items);
    }
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    bool Push(const T& item)
    {
        if (m_count == m_capacity && !Reserve(m_count + 1))
        {
            return false;
        }
        m_items[m_count++] = item;
        return true;
    }

    bool Reserve(size_t count)
    {
        if (count <= m_capacity)
        {
            return true;
        }
        size_t capacity = m_capacity == 0 ? 16 : m_capacity * 2;
        while (capacity < count)
        {
            capacity *= 2;
        }
        auto* items = static_cast<T*>(realloc(m_items, capacity * sizeof(T)));
        if (items == nullptr)
        {
            return false;
        }
        m_items = items;
        m_capacity = capacity;
        return true;
    }

    T Pop()
    {
        return m_items[--m_count];
    }

    void Truncate(size_t count)
    {
        m_count = count;
    }

    // The caller owns the items, which may be null when there are none.
    T* Release()
    {
        T* items = m_items;
        m_items = nullptr;
        m_count = 0;
        m_capacity = 0;
        return items;
    }

    size_t Count() const
    {
        return m_count;
    }

    T& operator[](size_t index)
    {
        return m_items[index];
    }

    T* Items()
    {
        return m_items;
    }

private:
    T* m_items = nullptr;
    size_t m_count = 0;
    size_t m_capacity = 0;
};

struct ByteSet
{
    uint64_t bits[4];

    void Add(unsigned char byte)
    {
        bits[byte / 64] |= 1ULL << (byte % 64);
    }

    void Remove(unsigned char byte)
    {
        bits[byte / 64] &= ~(1ULL << (byte % 64));
    }

    bool Contains(unsigned char byte) const
    {
        return (bits[byte / 64] >> (byte % 64) & 1) != 0;
    }
};

enum class Op : uint8_t
{
    Byte,
    Set,
    Any,
    // Continues at a, or failing that at b, both relative.
    Split,
    Jump,
    // Stores the position in register a.
    Save,
    // Clears registers a to b, the subexpressions nested in one that starts again, since each
    // iteration reports only what matched within it.
    Reset,
    // Stores the position in loop register a, which Progress a then requires to have moved, so
    // an iteration that matched nothing cannot repeat forever.
    Mark,
    Progress,
    Begin,
    End,
    // Matches the text subexpression a matched.
    Reference,
    Match,
};

struct Instruction
{
    Op op;
    unsigned char byte;
    int32_t a;
    int32_t b;
};

bool IsLower(int byte)
{
    return byte >= 'a' && byte <= 'z';
}

bool IsUpper(int byte)
{
    return byte >= 'A' && byte <= 'Z';
}

int FoldCase(int byte)
{
    return IsUpper(byte) ? byte - 'A' + 'a' : byte;
}

int OtherCase(int byte)
{
    if (IsLower(byte))
    {
        return byte - 'a' + 'A';
    }
    return IsUpper(byte) ? byte - 'A' + 'a' : byte;
}

// The C locale's classes, the same on the host as on FeatherOS.
bool InClass(const char* name, size_t length, int byte)
{
    auto is = [&](const char* candidate)
    { return strlen(candidate) == length && memcmp(candidate, name, length) == 0; };
    const bool digit = byte >= '0' && byte <= '9';
    const bool alpha = IsLower(byte) || IsUpper(byte);
    const bool graph = byte > ' ' && byte < 0x7f;
    if (is("alnum"))
    {
        return alpha || digit;
    }
    if (is("alpha"))
    {
        return alpha;
    }
    if (is("blank"))
    {
        return byte == ' ' || byte == '\t';
    }
    if (is("cntrl"))
    {
        return byte < ' ' || byte == 0x7f;
    }
    if (is("digit"))
    {
        return digit;
    }
    if (is("graph"))
    {
        return graph;
    }
    if (is("lower"))
    {
        return IsLower(byte);
    }
    if (is("print"))
    {
        return graph || byte == ' ';
    }
    if (is("punct"))
    {
        return graph && !alpha && !digit;
    }
    if (is("space"))
    {
        return byte == ' ' || (byte >= '\t' && byte <= '\r');
    }
    if (is("upper"))
    {
        return IsUpper(byte);
    }
    if (is("xdigit"))
    {
        return digit || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
    }
    return false;
}

bool IsClassName(const char* name, size_t length)
{
    static constexpr const char* Names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                            "lower", "print", "punct", "space", "upper", "xdigit"};
    for (const char* candidate : Names)
    {
        if (strlen(candidate) == length && memcmp(candidate, name, length) == 0)
        {
            return true;
        }
    }
    return false;
}

class Compiler
{
public:
    Compiler(const char* pattern, int flags)
        : m_cursor(pattern), m_end(pattern + strlen(pattern)), m_flags(flags)
    {
    }

    Status Run(Program** program, size_t* subexpression_count);

private:
    bool Extended() const
    {
        return (m_flags & Regex::Extended) != 0;
    }

    bool AtEnd() const
    {
        return m_cursor == m_end;
    }

    // Whether the text at the cursor starts with a backslash and then character.
    bool AtEscaped(char character) const
    {
        return m_end - m_cursor >= 2 && m_cursor[0] == '\\' && m_cursor[1] == character;
    }

    Status Emit(Instruction instruction);
    Status EmitByte(unsigned char byte);
    Status EmitSet(const ByteSet& set);
    Status Insert(size_t index, Instruction instruction);

    Status ParseAlternation(int depth);
    Status ParseBranch(int depth);
    Status ParseAtom(int depth, bool at_branch_start);
    Status ParseGroup(int depth);
    Status ParseEscape();
    Status ParseBracket();
    Status ParseBracketElement(ByteSet* set, int* single);
    Status ParseInterval(int* minimum, int* maximum);
    Status ParseQuantifiers(size_t fragment_start);
    Status Repeat(size_t fragment_start, int minimum, int maximum);

    const char* m_cursor;
    const char* m_end;
    int m_flags;
    Array<Instruction> m_code;
    Array<ByteSet> m_sets;
    size_t m_groups = 0;
    // Subexpressions 1 to 9 that have been closed, which back-references may name.
    uint32_t m_closed = 0;
    int32_t m_loops = 0;
    bool m_references = false;
};

} // namespace

struct Program
{
    Instruction* code;
    size_t length;
    ByteSet* sets;
    size_t subexpressions;
    size_t loops;
    int flags;
    bool references;
};

namespace
{

Status Compiler::Emit(Instruction instruction)
{
    if (m_code.Count() >= MaximumInstructions)
    {
        return Status::NoMemory;
    }
    return m_code.Push(instruction) ? Status::Matched : Status::NoMemory;
}

Status Compiler::EmitSet(const ByteSet& set)
{
    if (!m_sets.Push(set))
    {
        return Status::NoMemory;
    }
    return Emit({Op::Set, 0, static_cast<int32_t>(m_sets.Count() - 1), 0});
}

Status Compiler::EmitByte(unsigned char byte)
{
    if ((m_flags & IgnoreCase) != 0 && OtherCase(byte) != byte)
    {
        ByteSet set{};
        set.Add(byte);
        set.Add(static_cast<unsigned char>(OtherCase(byte)));
        return EmitSet(set);
    }
    return Emit({Op::Byte, byte, 0, 0});
}

// Every offset is relative, so moving the instructions after index keeps them valid as long
// as none jumps across index, which holds for the fragment at the end of the program.
Status Compiler::Insert(size_t index, Instruction instruction)
{
    const Status status = Emit(instruction);
    if (status != Status::Matched)
    {
        return status;
    }
    Instruction* code = m_code.Items();
    memmove(code + index + 1, code + index, (m_code.Count() - 1 - index) * sizeof(Instruction));
    code[index] = instruction;
    return Status::Matched;
}

Status Compiler::Run(Program** program, size_t* subexpression_count)
{
    Status status = ParseAlternation(0);
    if (status == Status::Matched && !AtEnd())
    {
        // Only a "\)" without its "\(" stops the outermost basic expression early.
        status = Status::UnmatchedParenthesis;
    }
    if (status == Status::Matched)
    {
        status = Emit({Op::Match, 0, 0, 0});
    }
    if (status != Status::Matched)
    {
        return status;
    }
    auto* result = static_cast<Program*>(malloc(sizeof(Program)));
    if (result == nullptr)
    {
        return Status::NoMemory;
    }
    result->length = m_code.Count();
    result->code = m_code.Release();
    result->sets = m_sets.Release();
    result->subexpressions = m_groups;
    result->loops = static_cast<size_t>(m_loops);
    result->flags = m_flags;
    result->references = m_references;
    *program = result;
    *subexpression_count = m_groups;
    return Status::Matched;
}

// Branches are laid out as: Split(next branch) branch Jump(end) ... last branch.
Status Compiler::ParseAlternation(int depth)
{
    if (depth > MaximumNesting)
    {
        return Status::NoMemory;
    }
    Array<size_t> jumps;
    size_t branch_start = m_code.Count();
    for (;;)
    {
        Status status = ParseBranch(depth);
        if (status != Status::Matched)
        {
            return status;
        }
        if (!Extended() || AtEnd() || *m_cursor != '|')
        {
            break;
        }
        ++m_cursor;
        const auto branch_length = static_cast<int32_t>(m_code.Count() - branch_start);
        status = Insert(branch_start, {Op::Split, 0, 1, branch_length + 2});
        if (status == Status::Matched && !jumps.Push(m_code.Count()))
        {
            status = Status::NoMemory;
        }
        if (status == Status::Matched)
        {
            status = Emit({Op::Jump, 0, 0, 0});
        }
        if (status != Status::Matched)
        {
            return status;
        }
        branch_start = m_code.Count();
    }
    for (size_t index = 0; index < jumps.Count(); ++index)
    {
        m_code[jumps[index]].a = static_cast<int32_t>(m_code.Count() - jumps[index]);
    }
    return Status::Matched;
}

Status Compiler::ParseBranch(int depth)
{
    bool first_atom = true;
    while (!AtEnd())
    {
        if (Extended() && (*m_cursor == '|' || (*m_cursor == ')' && depth > 0)))
        {
            break;
        }
        if (!Extended() && AtEscaped(')'))
        {
            break;
        }
        const size_t fragment_start = m_code.Count();
        // A "*" after a leading "^" is as literal as one at the start, so the atom after the
        // anchor reads it.
        const bool leading_anchor = !Extended() && first_atom && *m_cursor == '^';
        Status status = ParseAtom(depth, first_atom);
        if (status == Status::Matched && !leading_anchor)
        {
            status = ParseQuantifiers(fragment_start);
        }
        if (status != Status::Matched)
        {
            return status;
        }
        first_atom = false;
    }
    return Status::Matched;
}

Status Compiler::ParseAtom(int depth, bool at_branch_start)
{
    const char character = *m_cursor;
    if (Extended())
    {
        switch (character)
        {
        case '(':
            ++m_cursor;
            return ParseGroup(depth);
        case '*':
        case '+':
        case '?':
        case '{':
            return Status::BadRepetition;
        case '^':
            ++m_cursor;
            return Emit({Op::Begin, 0, 0, 0});
        case '$':
            ++m_cursor;
            return Emit({Op::End, 0, 0, 0});
        default:
            break;
        }
    }
    else
    {
        if (AtEscaped('('))
        {
            m_cursor += 2;
            return ParseGroup(depth);
        }
        if (AtEscaped('{'))
        {
            return Status::BadRepetition;
        }
        if (character == '^' && at_branch_start)
        {
            ++m_cursor;
            return Emit({Op::Begin, 0, 0, 0});
        }
        if (character == '$' &&
            (m_cursor + 1 == m_end ||
             (depth > 0 && m_end - m_cursor >= 3 && m_cursor[1] == '\\' && m_cursor[2] == ')')))
        {
            ++m_cursor;
            return Emit({Op::End, 0, 0, 0});
        }
    }
    switch (character)
    {
    case '.':
    {
        ++m_cursor;
        return Emit({Op::Any, 0, 0, 0});
    }
    case '[':
        ++m_cursor;
        return ParseBracket();
    case '\\':
        return ParseEscape();
    default:
        ++m_cursor;
        return EmitByte(static_cast<unsigned char>(character));
    }
}

// The cursor is past the opening parenthesis.
Status Compiler::ParseGroup(int depth)
{
    const size_t group = ++m_groups;
    const auto start_register = static_cast<int32_t>(2 * group);
    const size_t save_index = m_code.Count();
    Status status = Emit({Op::Save, 0, start_register, 0});
    if (status == Status::Matched)
    {
        status = ParseAlternation(depth + 1);
    }
    if (status != Status::Matched)
    {
        return status;
    }
    if (Extended() ? AtEnd() || *m_cursor != ')' : !AtEscaped(')'))
    {
        return Status::UnmatchedParenthesis;
    }
    m_cursor += Extended() ? 1 : 2;
    if (group <= 9)
    {
        m_closed |= 1U << group;
    }
    if (m_groups > group)
    {
        status = Insert(save_index + 1,
                        {Op::Reset, 0, start_register + 2, static_cast<int32_t>(2 * m_groups + 1)});
        if (status != Status::Matched)
        {
            return status;
        }
    }
    return Emit({Op::Save, 0, start_register + 1, 0});
}

Status Compiler::ParseEscape()
{
    if (m_end - m_cursor < 2)
    {
        return Status::BadEscape;
    }
    const char character = m_cursor[1];
    m_cursor += 2;
    if (character >= '1' && character <= '9')
    {
        const int group = character - '0';
        if ((m_closed & (1U << group)) == 0)
        {
            return Status::BadReference;
        }
        m_references = true;
        return Emit({Op::Reference, 0, group, 0});
    }
    // Letters and digits after a backslash are left for extensions such as \d; basic syntax's
    // \+, \? and \| are GNU operators, rejected rather than taken as plain characters.
    const bool alphanumeric =
        IsLower(character) || IsUpper(character) || (character >= '0' && character <= '9');
    if (alphanumeric || (!Extended() && (character == '+' || character == '?' || character == '|')))
    {
        return Status::BadEscape;
    }
    return EmitByte(static_cast<unsigned char>(character));
}

// The cursor is past the "[".
Status Compiler::ParseBracket()
{
    ByteSet set{};
    bool negated = false;
    if (!AtEnd() && *m_cursor == '^')
    {
        negated = true;
        ++m_cursor;
    }
    bool first = true;
    for (;;)
    {
        if (AtEnd())
        {
            return Status::UnmatchedBracket;
        }
        if (*m_cursor == ']' && !first)
        {
            ++m_cursor;
            break;
        }
        first = false;
        int low = -1;
        Status status = ParseBracketElement(&set, &low);
        if (status != Status::Matched)
        {
            return status;
        }
        if (m_end - m_cursor >= 2 && m_cursor[0] == '-' && m_cursor[1] != ']')
        {
            ++m_cursor;
            int high = -1;
            ByteSet unused{};
            status = ParseBracketElement(&unused, &high);
            if (status != Status::Matched)
            {
                return status;
            }
            if (low < 0 || high < 0 || high < low)
            {
                return Status::BadRange;
            }
            for (int byte = low; byte <= high; ++byte)
            {
                set.Add(static_cast<unsigned char>(byte));
            }
        }
        else if (low >= 0)
        {
            set.Add(static_cast<unsigned char>(low));
        }
    }
    if ((m_flags & IgnoreCase) != 0)
    {
        for (int byte = 0; byte < 256; ++byte)
        {
            if (set.Contains(static_cast<unsigned char>(byte)))
            {
                set.Add(static_cast<unsigned char>(OtherCase(byte)));
            }
        }
    }
    if (negated)
    {
        for (uint64_t& word : set.bits)
        {
            word = ~word;
        }
        if ((m_flags & Newline) != 0)
        {
            set.Remove('\n');
        }
    }
    // The text never holds a NUL, which regexec takes as its end.
    set.Remove(0);
    return EmitSet(set);
}

// A single byte goes to *single for the caller to add or use as a range's end; a class goes
// straight into set, leaving *single at -1.
Status Compiler::ParseBracketElement(ByteSet* set, int* single)
{
    if (m_end - m_cursor >= 2 && m_cursor[0] == '[' &&
        (m_cursor[1] == ':' || m_cursor[1] == '=' || m_cursor[1] == '.'))
    {
        const char kind = m_cursor[1];
        const char* name = m_cursor + 2;
        const char* close = name;
        while (close + 1 < m_end && !(close[0] == kind && close[1] == ']'))
        {
            ++close;
        }
        if (close + 1 >= m_end)
        {
            return Status::UnmatchedBracket;
        }
        const auto length = static_cast<size_t>(close - name);
        m_cursor = close + 2;
        if (kind == ':')
        {
            if (!IsClassName(name, length))
            {
                return Status::BadClass;
            }
            for (int byte = 1; byte < 256; ++byte)
            {
                if (InClass(name, length, byte))
                {
                    set->Add(static_cast<unsigned char>(byte));
                }
            }
            return Status::Matched;
        }
        // Only single bytes collate in the C locale, and each is its own equivalence class.
        if (length != 1)
        {
            return Status::BadCollatingElement;
        }
        *single = static_cast<unsigned char>(*name);
        return Status::Matched;
    }
    *single = static_cast<unsigned char>(*m_cursor);
    ++m_cursor;
    return Status::Matched;
}

// The cursor is past "{" or "\{"; a maximum of -1 means no limit.
Status Compiler::ParseInterval(int* minimum, int* maximum)
{
    auto number = [this](int* value)
    {
        if (AtEnd() || *m_cursor < '0' || *m_cursor > '9')
        {
            return false;
        }
        *value = 0;
        while (!AtEnd() && *m_cursor >= '0' && *m_cursor <= '9')
        {
            *value = *value * 10 + (*m_cursor - '0');
            if (*value > MaximumRepetition)
            {
                *value = MaximumRepetition + 1;
            }
            ++m_cursor;
        }
        return true;
    };
    if (!number(minimum))
    {
        return AtEnd() ? Status::UnmatchedBrace : Status::BadInterval;
    }
    *maximum = *minimum;
    if (!AtEnd() && *m_cursor == ',')
    {
        ++m_cursor;
        if (!number(maximum))
        {
            *maximum = -1;
        }
    }
    const bool closed = Extended() ? !AtEnd() && *m_cursor == '}' : AtEscaped('}');
    if (!closed)
    {
        return AtEnd() ? Status::UnmatchedBrace : Status::BadInterval;
    }
    m_cursor += Extended() ? 1 : 2;
    if (*minimum > MaximumRepetition || *maximum > MaximumRepetition ||
        (*maximum >= 0 && *maximum < *minimum))
    {
        return Status::BadInterval;
    }
    return Status::Matched;
}

Status Compiler::ParseQuantifiers(size_t fragment_start)
{
    while (!AtEnd())
    {
        int minimum = 0;
        int maximum = -1;
        const char character = *m_cursor;
        if (character == '*')
        {
            ++m_cursor;
        }
        else if (Extended() && character == '+')
        {
            ++m_cursor;
            minimum = 1;
        }
        else if (Extended() && character == '?')
        {
            ++m_cursor;
            maximum = 1;
        }
        else if (Extended() ? character == '{' : AtEscaped('{'))
        {
            m_cursor += Extended() ? 1 : 2;
            const Status status = ParseInterval(&minimum, &maximum);
            if (status != Status::Matched)
            {
                return status;
            }
        }
        else
        {
            break;
        }
        const Status status = Repeat(fragment_start, minimum, maximum);
        if (status != Status::Matched)
        {
            return status;
        }
    }
    return Status::Matched;
}

// Rewrites the fragment from fragment_start to the end as minimum copies of it, then either a
// loop over one more copy or maximum - minimum nested optional copies.
Status Compiler::Repeat(size_t fragment_start, int minimum, int maximum)
{
    const size_t length = m_code.Count() - fragment_start;
    const size_t optional = maximum < 0 ? 0 : static_cast<size_t>(maximum - minimum);
    const size_t total = static_cast<size_t>(minimum) * length +
                         (maximum < 0 ? length + 4 : optional * (length + 1));
    if (fragment_start + total > MaximumInstructions)
    {
        return Status::NoMemory;
    }
    auto* fragment = static_cast<Instruction*>(malloc(length * sizeof(Instruction) + 1));
    if (fragment == nullptr)
    {
        return Status::NoMemory;
    }
    memcpy(fragment, m_code.Items() + fragment_start, length * sizeof(Instruction));
    m_code.Truncate(fragment_start);
    // Pushes cannot fail once the space is reserved.
    auto copy = [&]
    {
        for (size_t index = 0; index < length; ++index)
        {
            m_code.Push(fragment[index]);
        }
    };
    if (!m_code.Reserve(fragment_start + total))
    {
        free(fragment);
        return Status::NoMemory;
    }
    for (int index = 0; index < minimum; ++index)
    {
        copy();
    }
    const auto span = static_cast<int32_t>(length);
    if (maximum < 0)
    {
        const int32_t loop = m_loops++;
        m_code.Push({Op::Split, 0, 1, span + 4});
        m_code.Push({Op::Mark, 0, loop, 0});
        copy();
        m_code.Push({Op::Progress, 0, loop, 0});
        m_code.Push({Op::Jump, 0, -(span + 3), 0});
    }
    else
    {
        for (size_t index = 0; index < optional; ++index)
        {
            m_code.Push({Op::Split, 0, 1, static_cast<int32_t>((optional - index) * (length + 1))});
            copy();
        }
    }
    free(fragment);
    return Status::Matched;
}

class Matcher
{
public:
    Matcher(const Program& program, const char* text, size_t length, int flags)
        : m_program(program), m_text(reinterpret_cast<const unsigned char*>(text)),
          m_length(static_cast<long>(length)), m_flags(flags)
    {
    }

    Status Run(Span* spans, size_t count);

private:
    struct StateList
    {
        int32_t* dense;
        int32_t* sparse;
        long* starts;
        size_t count;
    };

    // A branch to try at a position, or with register >= 0 a register's value to restore.
    struct Job
    {
        int32_t pc;
        int32_t register_index;
        long value;
    };

    bool AtBeginning(long position) const
    {
        if (position == 0)
        {
            return (m_flags & NotBeginning) == 0;
        }
        return (m_program.flags & Newline) != 0 && m_text[position - 1] == '\n';
    }

    bool AtFinish(long position) const
    {
        if (position == m_length)
        {
            return (m_flags & NotEnd) == 0;
        }
        return (m_program.flags & Newline) != 0 && m_text[position] == '\n';
    }

    bool Consumes(const Instruction& instruction, unsigned char byte) const
    {
        switch (instruction.op)
        {
        case Op::Byte:
            return byte == instruction.byte;
        case Op::Set:
            return m_program.sets[instruction.a].Contains(byte);
        case Op::Any:
            return (m_program.flags & Newline) == 0 || byte != '\n';
        default:
            return false;
        }
    }

    Status Simulate(bool first_only, long* start, long* end);
    void AddThread(StateList* list, int32_t pc, long start, long position, int32_t* stack);
    Status Search(long start, long required_end, long* registers, long* found, long* found_end);

    const Program& m_program;
    const unsigned char* m_text;
    long m_length;
    int m_flags;
    long m_best_start = -1;
    long m_best_end = -1;
    unsigned long m_steps = 0;
};

// Adds pc and everything reachable from it without consuming a byte. A state already in the
// list keeps the start it was reached with first, the earliest, since the threads are added in
// order of their starts.
void Matcher::AddThread(StateList* list, int32_t pc, long start, long position, int32_t* stack)
{
    size_t depth = 0;
    stack[depth++] = pc;
    while (depth > 0)
    {
        const int32_t current = stack[--depth];
        const int32_t slot = list->sparse[current];
        if (static_cast<size_t>(slot) < list->count && list->dense[slot] == current)
        {
            continue;
        }
        list->sparse[current] = static_cast<int32_t>(list->count);
        list->dense[list->count] = current;
        list->starts[list->count] = start;
        ++list->count;
        const Instruction& instruction = m_program.code[current];
        switch (instruction.op)
        {
        case Op::Split:
            stack[depth++] = current + instruction.b;
            stack[depth++] = current + instruction.a;
            break;
        case Op::Jump:
            stack[depth++] = current + instruction.a;
            break;
        case Op::Save:
        case Op::Reset:
        case Op::Mark:
        case Op::Progress:
            stack[depth++] = current + 1;
            break;
        case Op::Begin:
            if (AtBeginning(position))
            {
                stack[depth++] = current + 1;
            }
            break;
        case Op::End:
            if (AtFinish(position))
            {
                stack[depth++] = current + 1;
            }
            break;
        case Op::Match:
            if (m_best_start < 0 || start < m_best_start ||
                (start == m_best_start && position > m_best_end))
            {
                m_best_start = start;
                m_best_end = position;
            }
            break;
        default:
            break;
        }
    }
}

Status Matcher::Simulate(bool first_only, long* start, long* end)
{
    const size_t states = m_program.length;
    // Each state is added to a list once per position, and a Split pushes two.
    const size_t stack_entries = 2 * states + 2;
    auto* memory = static_cast<unsigned char*>(calloc(
        1, 2 * states * (2 * sizeof(int32_t) + sizeof(long)) + stack_entries * sizeof(int32_t)));
    if (memory == nullptr)
    {
        return Status::NoMemory;
    }
    StateList lists[2];
    unsigned char* cursor = memory;
    for (StateList& list : lists)
    {
        list.starts = reinterpret_cast<long*>(cursor);
        cursor += states * sizeof(long);
        list.dense = reinterpret_cast<int32_t*>(cursor);
        cursor += states * sizeof(int32_t);
        list.sparse = reinterpret_cast<int32_t*>(cursor);
        cursor += states * sizeof(int32_t);
        list.count = 0;
    }
    auto* stack = reinterpret_cast<int32_t*>(cursor);

    StateList* current = &lists[0];
    StateList* next = &lists[1];
    AddThread(current, 0, 0, 0, stack);
    for (long position = 0; position < m_length; ++position)
    {
        if (first_only && m_best_start >= 0)
        {
            break;
        }
        next->count = 0;
        const unsigned char byte = m_text[position];
        for (size_t index = 0; index < current->count; ++index)
        {
            const long thread_start = current->starts[index];
            if (m_best_start >= 0 && thread_start > m_best_start)
            {
                continue;
            }
            const int32_t pc = current->dense[index];
            if (Consumes(m_program.code[pc], byte))
            {
                AddThread(next, pc + 1, thread_start, position + 1, stack);
            }
        }
        if (m_best_start < 0)
        {
            AddThread(next, 0, position + 1, position + 1, stack);
        }
        StateList* swap = current;
        current = next;
        next = swap;
        if (current->count == 0)
        {
            break;
        }
    }
    free(memory);
    *start = m_best_start;
    *end = m_best_end;
    return m_best_start >= 0 ? Status::Matched : Status::NoMatch;
}

// A depth-first search in the program's order of preference, from start. With a required end,
// the first path that ends there wins, and (pc, position) pairs already tried are skipped, which
// holds without back-references since whether a pair leads to the end does not depend on the
// registers. Without one, every path is tried and the longest wins.
Status Matcher::Search(long start, long required_end, long* registers, long* found, long* found_end)
{
    const size_t register_count = 2 * (m_program.subexpressions + 1) + m_program.loops;
    const long limit = required_end >= 0 ? required_end : m_length;
    uint64_t* visited = nullptr;
    size_t width = 0;
    if (required_end >= 0)
    {
        width = static_cast<size_t>(required_end - start) + 1;
        if (m_program.length * width <= MaximumVisitedBits)
        {
            visited = static_cast<uint64_t*>(calloc((m_program.length * width + 63) / 64, 8));
        }
    }
    const size_t loop_base = 2 * (m_program.subexpressions + 1);
    Array<Job> jobs;
    Status status = jobs.Push({0, -1, start}) ? Status::NoMatch : Status::NoMemory;
    while (status == Status::NoMatch && jobs.Count() > 0)
    {
        const Job job = jobs.Pop();
        if (job.register_index >= 0)
        {
            registers[job.register_index] = job.value;
            continue;
        }
        int32_t pc = job.pc;
        long position = job.value;
        for (;;)
        {
            if (++m_steps > StepBudget)
            {
                status = Status::NoMemory;
                break;
            }
            const Instruction& instruction = m_program.code[pc];
            if (visited != nullptr && instruction.op != Op::Progress)
            {
                const size_t bit =
                    static_cast<size_t>(pc) * width + static_cast<size_t>(position - start);
                if ((visited[bit / 64] >> (bit % 64) & 1) != 0)
                {
                    break;
                }
                visited[bit / 64] |= 1ULL << (bit % 64);
            }
            bool advance = false;
            switch (instruction.op)
            {
            case Op::Byte:
            case Op::Set:
            case Op::Any:
                if (position < limit && Consumes(instruction, m_text[position]))
                {
                    ++position;
                    advance = true;
                }
                break;
            case Op::Split:
                if (!jobs.Push({pc + instruction.b, -1, position}))
                {
                    status = Status::NoMemory;
                    break;
                }
                pc += instruction.a - 1;
                advance = true;
                break;
            case Op::Jump:
                pc += instruction.a - 1;
                advance = true;
                break;
            case Op::Save:
            case Op::Mark:
            {
                const size_t index = instruction.op == Op::Save
                                         ? static_cast<size_t>(instruction.a)
                                         : loop_base + static_cast<size_t>(instruction.a);
                if (!jobs.Push({0, static_cast<int32_t>(index), registers[index]}))
                {
                    status = Status::NoMemory;
                    break;
                }
                registers[index] = position;
                advance = true;
                break;
            }
            case Op::Reset:
                advance = true;
                for (int32_t index = instruction.a; index <= instruction.b && advance; ++index)
                {
                    if (!jobs.Push({0, index, registers[index]}))
                    {
                        status = Status::NoMemory;
                        advance = false;
                    }
                    registers[index] = -1;
                }
                break;
            case Op::Progress:
                advance = registers[loop_base + static_cast<size_t>(instruction.a)] != position;
                break;
            case Op::Begin:
                advance = AtBeginning(position);
                break;
            case Op::End:
                advance = AtFinish(position);
                break;
            case Op::Reference:
            {
                const long from = registers[2 * static_cast<size_t>(instruction.a)];
                const long to = registers[2 * instruction.a + 1];
                if (from < 0 || to < from || to - from > limit - position)
                {
                    break;
                }
                bool same = true;
                for (long offset = 0; offset < to - from && same; ++offset)
                {
                    const unsigned char left = m_text[from + offset];
                    const unsigned char right = m_text[position + offset];
                    same = (m_program.flags & IgnoreCase) != 0 ? FoldCase(left) == FoldCase(right)
                                                               : left == right;
                }
                if (same)
                {
                    position += to - from;
                    advance = true;
                }
                break;
            }
            case Op::Match:
                if (required_end >= 0 ? position == required_end : position > *found_end)
                {
                    memcpy(found, registers, register_count * sizeof(long));
                    *found_end = position;
                    // Nothing can be longer than the whole text.
                    if (required_end >= 0 || position == m_length)
                    {
                        status = Status::Matched;
                    }
                }
                break;
            }
            if (!advance || status != Status::NoMatch)
            {
                break;
            }
            ++pc;
        }
    }
    free(visited);
    if (status == Status::NoMatch && *found_end >= 0)
    {
        status = Status::Matched;
    }
    return status;
}

Status Matcher::Run(Span* spans, size_t count)
{
    long start = -1;
    long end = -1;
    const bool want_subexpressions = count > 1 && m_program.subexpressions > 0;
    long* registers = nullptr;
    long* found = nullptr;
    const size_t register_count = 2 * (m_program.subexpressions + 1) + m_program.loops;
    if (want_subexpressions || m_program.references)
    {
        registers = static_cast<long*>(malloc(2 * register_count * sizeof(long)));
        if (registers == nullptr)
        {
            return Status::NoMemory;
        }
        found = registers + register_count;
    }

    Status status = Status::NoMatch;
    if (!m_program.references)
    {
        status = Simulate(count == 0, &start, &end);
        if (status == Status::Matched && want_subexpressions)
        {
            for (size_t index = 0; index < register_count; ++index)
            {
                registers[index] = -1;
            }
            long found_end = -1;
            status = Search(start, end, registers, found, &found_end);
        }
    }
    else
    {
        // A Thompson simulation cannot follow back-references, so each start is searched in turn.
        for (long candidate = 0; candidate <= m_length && status == Status::NoMatch; ++candidate)
        {
            for (size_t index = 0; index < register_count; ++index)
            {
                registers[index] = -1;
            }
            long found_end = -1;
            status = Search(candidate, -1, registers, found, &found_end);
            start = candidate;
            end = found_end;
        }
    }

    if (status == Status::Matched)
    {
        for (size_t index = 0; index < count; ++index)
        {
            spans[index] = {-1, -1};
        }
        if (count > 0)
        {
            spans[0] = {start, end};
        }
        for (size_t group = 1;
             group < count && group <= m_program.subexpressions && found != nullptr; ++group)
        {
            const long from = found[2 * group];
            const long to = found[2 * group + 1];
            if (from >= 0 && to >= from)
            {
                spans[group] = {from, to};
            }
        }
    }
    free(registers);
    return status;
}

} // namespace

Status Compile(const char* pattern, int flags, Program** program, size_t* subexpression_count)
{
    Compiler compiler(pattern, flags);
    return compiler.Run(program, subexpression_count);
}

Status Execute(const Program& program, const char* text, size_t length, int flags, Span* spans,
               size_t count)
{
    if ((program.flags & NoSubexpressions) != 0)
    {
        count = 0;
    }
    Matcher matcher(program, text, length, flags);
    return matcher.Run(spans, count);
}

void Free(Program* program)
{
    if (program != nullptr)
    {
        free(program->code);
        free(program->sets);
        free(program);
    }
}

const char* Describe(Status status)
{
    switch (status)
    {
    case Status::Matched:
        return "Success";
    case Status::NoMatch:
        return "No match";
    case Status::BadPattern:
        return "Invalid regular expression";
    case Status::BadCollatingElement:
        return "Invalid collation character";
    case Status::BadClass:
        return "Invalid character class name";
    case Status::BadEscape:
        return "Invalid or trailing backslash";
    case Status::BadReference:
        return "Invalid back reference";
    case Status::UnmatchedBracket:
        return "Unmatched [, [^, [:, [., or [=";
    case Status::UnmatchedParenthesis:
        return "Unmatched ( or \\(";
    case Status::UnmatchedBrace:
        return "Unmatched \\{";
    case Status::BadInterval:
        return "Invalid content of \\{\\}";
    case Status::BadRange:
        return "Invalid range end";
    case Status::NoMemory:
        return "Memory exhausted";
    case Status::BadRepetition:
        return "Invalid preceding regular expression";
    }
    return "Unknown error";
}

} // namespace FeatherOS::LibC::Regex
