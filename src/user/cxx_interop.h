#ifndef FEATHEROS_USER_CXX_INTEROP_H
#define FEATHEROS_USER_CXX_INTEROP_H

// The C++ that cxxinterop, a Swift program, calls to check which C++ features Swift can use.
// Built by GCC like other user code, so Swift reaches it through the Itanium C++ ABI alone.

// The annotations of the toolchain's <swift/bridging>, which only Swift's Clang understands.
#if __has_attribute(swift_attr)
#define FEATHEROS_SWIFT_SHARED_REFERENCE(retain, release)                                          \
    __attribute__((swift_attr("import_reference"))) __attribute__((swift_attr("retain:" #retain))) \
    __attribute__((swift_attr("release:" #release)))
#define FEATHEROS_SWIFT_RETURNS_RETAINED __attribute__((swift_attr("returns_retained")))
#else
#define FEATHEROS_SWIFT_SHARED_REFERENCE(retain, release)
#define FEATHEROS_SWIFT_RETURNS_RETAINED
#endif

namespace FeatherOS::User::Interop
{
class Node;
}

// Swift finds a shared reference's retain and release functions only at global scope.
void RetainInteropNode(FeatherOS::User::Interop::Node* node);
void ReleaseInteropNode(FeatherOS::User::Interop::Node* node);

namespace FeatherOS::User::Interop
{

constexpr int Answer = 42;

enum class Color
{
    Red,
    Green,
    Blue,
};

Color NextColor(Color color);

struct Point
{
    int x;
    int y;

    Point operator+(const Point& other) const;
    bool operator==(const Point& other) const;
};

template <typename T> struct Box
{
    T value;

    T Get() const
    {
        return value;
    }
};

using IntBox = Box<int>;

// Counts its live objects, so the checks see Swift run constructors and destructors.
class Tracked
{
public:
    explicit Tracked(int id);
    Tracked(const Tracked& other);
    ~Tracked();

    int Id() const;
    static int Live();

private:
    int m_id;
};

// Movable but not copyable.
class Counter
{
public:
    Counter();
    Counter(Counter&& other);
    Counter(const Counter&) = delete;

    void Bump();
    int Count() const;

private:
    int m_count;
};

class Shape
{
public:
    virtual ~Shape();
    virtual int Area() const;
};

class Square : public Shape
{
public:
    explicit Square(int side);
    int Area() const override;

private:
    int m_side;
};

// Reference counted, so Swift holds it as a class instead of copying it.
class FEATHEROS_SWIFT_SHARED_REFERENCE(RetainInteropNode, ReleaseInteropNode) Node
{
public:
    static Node* Create(int value) FEATHEROS_SWIFT_RETURNS_RETAINED;
    static int Live();

    int Value() const;

private:
    explicit Node(int value);
    ~Node();

    friend void ::RetainInteropNode(Node* node);
    friend void ::ReleaseInteropNode(Node* node);

    int m_references;
    int m_value;
};

} // namespace FeatherOS::User::Interop

#endif
