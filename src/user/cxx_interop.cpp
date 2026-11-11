#include "cxx_interop.h"

namespace FeatherOS::User::Interop
{

namespace
{

int g_tracked_live;
int g_nodes_live;

} // namespace

Color NextColor(Color color)
{
    switch (color)
    {
    case Color::Red:
        return Color::Green;
    case Color::Green:
        return Color::Blue;
    case Color::Blue:
        break;
    }
    return Color::Red;
}

Point Point::operator+(const Point& other) const
{
    return {.x = x + other.x, .y = y + other.y};
}

bool Point::operator==(const Point& other) const
{
    return x == other.x && y == other.y;
}

Tracked::Tracked(int id) : m_id(id)
{
    ++g_tracked_live;
}

Tracked::Tracked(const Tracked& other) : m_id(other.m_id)
{
    ++g_tracked_live;
}

Tracked::~Tracked()
{
    --g_tracked_live;
}

int Tracked::Id() const
{
    return m_id;
}

int Tracked::Live()
{
    return g_tracked_live;
}

Counter::Counter() : m_count(0) {}

Counter::Counter(Counter&& other) : m_count(other.m_count)
{
    other.m_count = 0;
}

void Counter::Bump()
{
    ++m_count;
}

int Counter::Count() const
{
    return m_count;
}

Shape::~Shape() = default;

int Shape::Area() const
{
    return 0;
}

Square::Square(int side) : m_side(side) {}

int Square::Area() const
{
    return m_side * m_side;
}

Node::Node(int value) : m_references(1), m_value(value)
{
    ++g_nodes_live;
}

Node::~Node()
{
    --g_nodes_live;
}

Node* Node::Create(int value)
{
    return new Node(value);
}

int Node::Live()
{
    return g_nodes_live;
}

int Node::Value() const
{
    return m_value;
}

std::optional<int> Half(int value)
{
    if (value % 2 != 0)
    {
        return std::nullopt;
    }
    return value / 2;
}

IntVector Squares(int count)
{
    IntVector squares;
    for (int value = 0; value < count; ++value)
    {
        squares.push_back(value * value);
    }
    return squares;
}

int Sum(IntSpan values)
{
    int sum = 0;
    for (int value : values)
    {
        sum += value;
    }
    return sum;
}

int CountVowels(std::string_view text)
{
    int count = 0;
    for (char character : text)
    {
        count += std::string_view("aeiou").contains(character) ? 1 : 0;
    }
    return count;
}

std::string Greeting(std::string_view name)
{
    return "hello, " + std::string(name);
}

std::size_t CountWords(const std::string& text)
{
    std::size_t words = 0;
    for (std::size_t start = text.find_first_not_of(' '); start != std::string::npos;
         start = text.find_first_not_of(' ', text.find(' ', start)))
    {
        ++words;
    }
    return words;
}

} // namespace FeatherOS::User::Interop

void RetainInteropNode(FeatherOS::User::Interop::Node* node)
{
    ++node->m_references;
}

void ReleaseInteropNode(FeatherOS::User::Interop::Node* node)
{
    if (--node->m_references == 0)
    {
        delete node;
    }
}
