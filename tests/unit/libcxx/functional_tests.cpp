#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/functional"
#include "libcxx/include/optional"
#include "libcxx/include/string_view"
#include "libcxx/include/variant"

namespace
{

namespace fstd = featheros_std;

struct Tracked
{
    static inline int alive = 0;
    int value;
    explicit Tracked(int initial) : value(initial) { ++alive; }
    Tracked(const Tracked& other) : value(other.value) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++alive; }
    ~Tracked() { --alive; }
    int operator()(int argument) const { return value + argument; }
};

// Larger than function's inline storage, so it lives on the heap.
struct Large
{
    long values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    long operator()() const { return values[0] + values[7]; }
};

struct Counter
{
    int calls = 0;
    int operator()() { return ++calls; }
};

// Which overload a call reached, by the wrapper's value category.
struct Qualified
{
    constexpr int operator()() & { return 1; }
    constexpr int operator()() const& { return 2; }
    constexpr int operator()() && { return 3; }
    constexpr int operator()() const&& { return 4; }
};

// Callable only as an lvalue.
struct LvalueOnly
{
    void operator()() & {}
    void operator()() && = delete;
};

struct Point
{
    int x;
    int y;
    constexpr int Sum() const { return x + y; }
};

enum class Color : unsigned char
{
    Red = 3,
};

struct NotHashable
{
};

int Twice(int value)
{
    return value * 2;
}

} // namespace

TEST_CASE("function calls what it holds", "[libcxx]")
{
    fstd::function<int(int)> empty;
    REQUIRE((!empty && empty == nullptr));

    fstd::function<int(int)> lambda = [](int value) { return value + 1; };
    REQUIRE((lambda && lambda(2) == 3));
    fstd::function<int(int)> pointer = &Twice;
    REQUIRE(pointer(4) == 8);
    REQUIRE(*pointer.target<int (*)(int)>() == &Twice);
    REQUIRE(pointer.target<Tracked>() == nullptr);

    int (*null_pointer)(int) = nullptr;
    fstd::function<int(int)> from_null = null_pointer;
    REQUIRE(!from_null);
    fstd::function<int(int)> from_empty_function = fstd::function<long(long)>();
    REQUIRE(!from_empty_function);

    fstd::function<long()> large = Large();
    REQUIRE(large() == 9);
    REQUIRE(large.target<Large>()->values[3] == 4);

    // The result converts, and void discards it.
    fstd::function<void(int)> discard = &Twice;
    discard(1);
    fstd::function<long(short)> widened = &Twice;
    REQUIRE(widened(3) == 6);

    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::function<int(int)>, NotHashable>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::function<int(int)>, int (*)(Point)>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::function<int(int)>>);
}

TEST_CASE("function copies, moves and swaps its callable", "[libcxx]")
{
    {
        fstd::function<int(int)> first = Tracked(1);
        REQUIRE(Tracked::alive == 1);
        fstd::function<int(int)> copy = first;
        REQUIRE((Tracked::alive == 2 && copy(1) == 2));
        fstd::function<int(int)> moved = fstd::move(first);
        REQUIRE((Tracked::alive == 2 && !first && moved(2) == 3));
        fstd::function<int(int)> other = [](int value) { return -value; };
        moved.swap(other);
        REQUIRE((moved(1) == -1 && other(1) == 2));
        swap(moved, other);
        REQUIRE(moved(1) == 2);
        moved = nullptr;
        REQUIRE((Tracked::alive == 1 && !moved));
        moved = copy;
        REQUIRE(Tracked::alive == 2);
        moved = fstd::move(copy);
        REQUIRE(Tracked::alive == 1);
    }
    REQUIRE(Tracked::alive == 0);

    fstd::function<long()> large = Large();
    fstd::function<long()> large_copy = large;
    fstd::function<long()> small = [] { return 1L; };
    large_copy.swap(small);
    REQUIRE((large_copy() == 1 && small() == 9 && large() == 9));
}

TEST_CASE("function calls a mutable callable through const and through reference_wrapper",
          "[libcxx]")
{
    const fstd::function<int()> counter = Counter();
    counter();
    REQUIRE(counter() == 2);

    Counter shared;
    fstd::function<int()> referring;
    referring = fstd::ref(shared);
    referring();
    REQUIRE((shared.calls == 1 && referring.target<fstd::reference_wrapper<Counter>>() != nullptr));
}

TEST_CASE("function's deduction guides", "[libcxx]")
{
    fstd::function from_pointer = &Twice;
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_pointer), fstd::function<int(int)>>);
    fstd::function from_lambda = [](long value, char) noexcept { return value; };
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_lambda), fstd::function<long(long, char)>>);
    fstd::function from_mutable = [count = 0]() mutable { return ++count; };
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_mutable), fstd::function<int()>>);
    REQUIRE(from_mutable() == 1);
}

TEST_CASE("not_fn, bind_front and bind_back forward their value category", "[libcxx]")
{
    constexpr auto odd = [](int value) { return value % 2 != 0; };
    constexpr auto even = fstd::not_fn(odd);
    STATIC_REQUIRE((even(4) && !even(3)));

    constexpr auto subtract = [](int left, int right) { return left - right; };
    STATIC_REQUIRE(fstd::bind_front(subtract, 10)(3) == 7);
    STATIC_REQUIRE(fstd::bind_back(subtract, 10)(3) == -7);
    STATIC_REQUIRE(fstd::bind_front(subtract, 10, 4)() == 6);
    STATIC_REQUIRE(fstd::bind_front(&Point::Sum, Point{1, 2})() == 3);

    auto wrapped = fstd::bind_front(Qualified());
    const auto& constant = wrapped;
    REQUIRE(wrapped() == 1);
    REQUIRE(constant() == 2);
    REQUIRE(fstd::move(wrapped)() == 3);
    REQUIRE(fstd::move(constant)() == 4);

    // An rvalue wrapper whose callable takes only lvalues has its call deleted, not fall back
    // to the const lvalue overload.
    auto lvalue_only = fstd::bind_front(LvalueOnly());
    STATIC_REQUIRE(fstd::is_invocable_v<decltype(lvalue_only)&>);
    STATIC_REQUIRE(!fstd::is_invocable_v<decltype(lvalue_only)>);
    STATIC_REQUIRE(!fstd::is_invocable_v<decltype(fstd::not_fn(odd)), Point>);

    auto owner = fstd::bind_back([](Tracked& callable, int argument) { return callable(argument); },
                                 5);
    {
        Tracked target(1);
        REQUIRE(owner(target) == 6);
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("mem_fn calls members through objects, pointers and references", "[libcxx]")
{
    constexpr Point point{2, 5};
    STATIC_REQUIRE(fstd::mem_fn(&Point::Sum)(point) == 7);
    STATIC_REQUIRE(fstd::mem_fn(&Point::y)(&point) == 5);
    Point changing{1, 1};
    fstd::mem_fn(&Point::x)(changing) = 4;
    REQUIRE(changing.x == 4);
    REQUIRE(fstd::mem_fn(&Point::Sum)(fstd::ref(changing)) == 5);
    STATIC_REQUIRE(!fstd::is_invocable_v<decltype(fstd::mem_fn(&Point::Sum)), int>);
}

TEST_CASE("hash covers the built-in types and disables the rest", "[libcxx]")
{
    REQUIRE(fstd::hash<int>()(42) == 42);
    REQUIRE(fstd::hash<char>()('a') == static_cast<size_t>('a'));
    REQUIRE(fstd::hash<Color>()(Color::Red) == 3);
    REQUIRE(fstd::hash<bool>()(true) == 1);
    int value = 0;
    REQUIRE(fstd::hash<int*>()(&value) == reinterpret_cast<size_t>(&value));
    REQUIRE(fstd::hash<fstd::nullptr_t>()(nullptr) == 0);

    REQUIRE(fstd::hash<double>()(0.0) == fstd::hash<double>()(-0.0));
    REQUIRE(fstd::hash<float>()(0.0F) == fstd::hash<float>()(-0.0F));
    REQUIRE(fstd::hash<double>()(1.0) != fstd::hash<double>()(2.0));
    REQUIRE(fstd::hash<long double>()(0.0L) == fstd::hash<long double>()(-0.0L));
    REQUIRE(fstd::hash<long double>()(1.5L) == fstd::hash<long double>()(1.5L));

    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::hash<NotHashable>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::hash<NotHashable>>);
    STATIC_REQUIRE(!fstd::is_invocable_v<fstd::hash<NotHashable>, NotHashable>);
    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::hash<fstd::optional<NotHashable>>>);
    STATIC_REQUIRE(
        !fstd::is_default_constructible_v<fstd::hash<fstd::variant<int, NotHashable>>>);
}

TEST_CASE("hash of string views depends only on the characters", "[libcxx]")
{
    const char text[] = "feather feather";
    const fstd::string_view first(text, 7);
    const fstd::string_view second(text + 8, 7);
    REQUIRE(fstd::hash<fstd::string_view>()(first) == fstd::hash<fstd::string_view>()(second));
    REQUIRE(fstd::hash<fstd::string_view>()("abc") != fstd::hash<fstd::string_view>()("acb"));
    REQUIRE(fstd::hash<fstd::string_view>()("") != fstd::hash<fstd::string_view>()("a"));
    REQUIRE(fstd::hash<fstd::u16string_view>()(u"ab") ==
            fstd::hash<fstd::u16string_view>()(fstd::u16string_view(u"abc", 2)));
    REQUIRE(fstd::hash<fstd::wstring_view>()(L"x") != fstd::hash<fstd::wstring_view>()(L"y"));
}

TEST_CASE("hash of optional, variant and monostate", "[libcxx]")
{
    REQUIRE(fstd::hash<fstd::optional<int>>()(fstd::optional<int>(5)) == fstd::hash<int>()(5));
    REQUIRE(fstd::hash<fstd::optional<const int>>()(fstd::optional<const int>(5)) ==
            fstd::hash<int>()(5));
    REQUIRE(fstd::hash<fstd::optional<int>>()(fstd::nullopt) ==
            fstd::hash<fstd::optional<int>>()(fstd::nullopt));

    using Number = fstd::variant<int, long>;
    REQUIRE(fstd::hash<Number>()(Number(1)) == fstd::hash<Number>()(Number(1)));
    REQUIRE(fstd::hash<Number>()(Number(1)) != fstd::hash<Number>()(Number(1L)));
    REQUIRE(fstd::hash<fstd::monostate>()(fstd::monostate()) ==
            fstd::hash<fstd::monostate>()(fstd::monostate()));
}
