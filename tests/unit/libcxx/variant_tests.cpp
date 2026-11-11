#include <catch2/catch_test_macros.hpp>

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
    Tracked(Tracked&& other) noexcept : value(other.value)
    {
        other.value = -1;
        ++alive;
    }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
};

struct MoveOnly
{
    int value;
    explicit MoveOnly(int initial) : value(initial) {}
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

struct NoDefault
{
    explicit NoDefault(int) {}
};

// A literal type that is not trivially destructible, so constexpr code exercises the
// non-trivial paths.
struct Literal
{
    int value;
    constexpr Literal(int initial) : value(initial) {}
    constexpr Literal(const Literal& other) : value(other.value) {}
    constexpr Literal& operator=(const Literal& other)
    {
        value = other.value;
        return *this;
    }
    constexpr ~Literal() {}
    friend constexpr bool operator==(const Literal&, const Literal&) = default;
    friend constexpr auto operator<=>(const Literal&, const Literal&) = default;
};

struct Shape : fstd::variant<int, double>
{
    using variant::variant;
};

struct Describe
{
    constexpr int operator()(int) const { return 1; }
    constexpr int operator()(double) const { return 2; }
    constexpr int operator()(const Literal& literal) const { return literal.value; }
};

} // namespace

TEST_CASE("variant's layout and traits", "[libcxx]")
{
    using Simple = fstd::variant<int, double>;
    STATIC_REQUIRE(fstd::variant_size_v<Simple> == 2 && fstd::variant_size_v<const Simple> == 2);
    STATIC_REQUIRE(fstd::is_same_v<fstd::variant_alternative_t<1, Simple>, double>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::variant_alternative_t<0, const Simple>, const int>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<Simple>);
    STATIC_REQUIRE(fstd::is_trivially_destructible_v<Simple>);
    STATIC_REQUIRE(sizeof(fstd::variant<int, char>) == 2 * sizeof(int));
    STATIC_REQUIRE(!fstd::is_trivially_copy_constructible_v<fstd::variant<int, Tracked>>);
    STATIC_REQUIRE(!fstd::is_trivially_destructible_v<fstd::variant<int, Tracked>>);
    STATIC_REQUIRE(fstd::is_copy_constructible_v<fstd::variant<int, Tracked>>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::variant<int, MoveOnly>>);
    STATIC_REQUIRE(fstd::is_move_constructible_v<fstd::variant<int, MoveOnly>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::variant<int, Tracked>>);
    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::variant<NoDefault, int>>);
    STATIC_REQUIRE(fstd::is_default_constructible_v<fstd::variant<fstd::monostate, NoDefault>>);
    STATIC_REQUIRE(fstd::variant_npos == static_cast<fstd::size_t>(-1));
}

TEST_CASE("variant's converting constructor picks the alternative without narrowing", "[libcxx]")
{
    STATIC_REQUIRE(fstd::variant<int, double>(1.5f).index() == 1);
    STATIC_REQUIRE(fstd::variant<float, long>(0).index() == 1);
    STATIC_REQUIRE(fstd::variant<fstd::string_view, bool>("text").index() == 0);
    STATIC_REQUIRE(fstd::variant<bool, int>(true).index() == 0);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::variant<int, long>, double>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::variant<int, int>, int>);
    STATIC_REQUIRE(fstd::is_convertible_v<int, fstd::variant<int, double>>);

    constexpr fstd::variant<int, Literal> literal(fstd::in_place_type<Literal>, 4);
    STATIC_REQUIRE(literal.index() == 1 && fstd::get<1>(literal).value == 4);
    constexpr fstd::variant<int, int> second(fstd::in_place_index<1>, 3);
    STATIC_REQUIRE(second.index() == 1 && fstd::get<1>(second) == 3);
    constexpr fstd::variant<int, double> defaulted;
    STATIC_REQUIRE(defaulted.index() == 0 && fstd::get<0>(defaulted) == 0);
    STATIC_REQUIRE(!defaulted.valueless_by_exception());
}

TEST_CASE("variant manages its alternatives' lifetimes", "[libcxx]")
{
    {
        fstd::variant<int, Tracked> value(fstd::in_place_type<Tracked>, 1);
        REQUIRE(Tracked::alive == 1);
        fstd::variant<int, Tracked> copy = value;
        REQUIRE((Tracked::alive == 2 && fstd::get<Tracked>(copy).value == 1));
        value = 5;
        REQUIRE((Tracked::alive == 1 && fstd::holds_alternative<int>(value)));
        value = copy;
        REQUIRE((Tracked::alive == 2 && fstd::get<1>(value).value == 1));
        fstd::get<1>(copy).value = 6;
        value = copy;
        REQUIRE((Tracked::alive == 2 && fstd::get<1>(value).value == 6));
        fstd::variant<int, Tracked> moved = fstd::move(value);
        REQUIRE((Tracked::alive == 3 && fstd::get<1>(value).value == -1));
        REQUIRE(moved.emplace<0>(9) == 9);
        REQUIRE(Tracked::alive == 2);
        REQUIRE(copy.emplace<Tracked>(8).value == 8);
        REQUIRE(Tracked::alive == 2);
    }
    REQUIRE(Tracked::alive == 0);

    fstd::variant<MoveOnly, int> owned(fstd::in_place_index<0>, 3);
    fstd::variant<MoveOnly, int> taken = fstd::move(owned);
    REQUIRE(fstd::get<MoveOnly>(taken).value == 3);
    owned = 4;
    taken = fstd::move(owned);
    REQUIRE(fstd::get<int>(taken) == 4);

    constexpr int computed = [] {
        fstd::variant<int, Literal> value = 1;
        value = Literal(5);
        fstd::variant<int, Literal> copy = value;
        copy.emplace<0>(2);
        value = copy;
        return fstd::get<0>(value) + copy.index();
    }();
    STATIC_REQUIRE(computed == 2);
}

TEST_CASE("variant's get, get_if and holds_alternative", "[libcxx]")
{
    fstd::variant<int, fstd::string_view> value = fstd::string_view("word");
    REQUIRE(fstd::holds_alternative<fstd::string_view>(value));
    REQUIRE(fstd::get<1>(value) == "word");
    REQUIRE(fstd::get_if<int>(&value) == nullptr);
    REQUIRE(fstd::get_if<1>(&value)->size() == 4);
    fstd::variant<int, fstd::string_view>* missing = nullptr;
    REQUIRE(fstd::get_if<0>(missing) == nullptr);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::get<0>(fstd::move(value))), int&&>);
    const auto& constant = value;
    STATIC_REQUIRE(
        fstd::is_same_v<decltype(fstd::get_if<1>(&constant)), const fstd::string_view*>);
}

TEST_CASE("visit dispatches on one or more variants", "[libcxx]")
{
    constexpr fstd::variant<int, double, Literal> literal = Literal(7);
    STATIC_REQUIRE(fstd::visit(Describe{}, literal) == 7);
    STATIC_REQUIRE(fstd::visit(Describe{}, fstd::variant<int, double, Literal>(1.0)) == 2);

    constexpr auto add = [](auto left, auto right) { return static_cast<long>(left + right); };
    STATIC_REQUIRE(fstd::visit(add, fstd::variant<int, char>('\x02'),
                               fstd::variant<long, short>(static_cast<short>(3))) == 5);
    STATIC_REQUIRE(fstd::visit<int>([](auto value) { return value * 2; },
                                    fstd::variant<int, double>(1.5)) == 3);
    STATIC_REQUIRE(fstd::visit([] { return 4; }) == 4);
    STATIC_REQUIRE(fstd::visit([](auto value) { return static_cast<int>(value); }, Shape(2.5)) ==
                   2);

    fstd::variant<int, long> target = 1L;
    fstd::visit([](auto& value) { value += 2; }, target);
    REQUIRE(fstd::get<long>(target) == 3);
    fstd::variant<long> single = 2L;
    long& reference = fstd::visit([](long& value) -> long& { return value; }, single);
    REQUIRE(&reference == fstd::get_if<long>(&single));

    fstd::variant<MoveOnly, int> owned(fstd::in_place_index<0>, 6);
    auto moved = fstd::visit(
        [](auto&& value) -> int {
            if constexpr (fstd::is_same_v<fstd::remove_cvref_t<decltype(value)>, MoveOnly>)
            {
                MoveOnly taken = fstd::move(value);
                return taken.value;
            }
            else
            {
                return value;
            }
        },
        fstd::move(owned));
    REQUIRE(moved == 6);
}

TEST_CASE("variant compares by index, then by value", "[libcxx]")
{
    using Value = fstd::variant<int, double>;
    STATIC_REQUIRE(Value(1) == Value(1) && Value(1) != Value(1.0) && Value(2) != Value(3));
    STATIC_REQUIRE(Value(5) < Value(0.5) && Value(1) < Value(2) && Value(0.5) > Value(5));
    STATIC_REQUIRE(Value(1) <= Value(1) && Value(2.0) >= Value(1.0));
    STATIC_REQUIRE((Value(1) <=> Value(2)) < 0 && (Value(1.0) <=> Value(1)) > 0);
    STATIC_REQUIRE(fstd::is_same_v<decltype(Value() <=> Value()), fstd::partial_ordering>);

    using Monostate = fstd::variant<fstd::monostate, int>;
    STATIC_REQUIRE(Monostate() == Monostate() && Monostate() < Monostate(0));
    STATIC_REQUIRE((fstd::monostate{} <=> fstd::monostate{}) == fstd::strong_ordering::equal);

    using Literals = fstd::variant<int, Literal>;
    STATIC_REQUIRE(Literals(Literal(1)) == Literals(Literal(1)) &&
                   Literals(Literal(1)) < Literals(Literal(2)));

    const double nan = __builtin_nan("");
    REQUIRE(!(Value(nan) == Value(nan)));
    REQUIRE((Value(nan) <=> Value(1.0)) == fstd::partial_ordering::unordered);
}

TEST_CASE("variant swaps alternatives of the same and of different kinds", "[libcxx]")
{
    fstd::variant<int, Tracked> left(fstd::in_place_type<Tracked>, 1);
    fstd::variant<int, Tracked> right(fstd::in_place_type<Tracked>, 2);
    swap(left, right);
    REQUIRE((fstd::get<1>(left).value == 2 && fstd::get<1>(right).value == 1));
    right = 7;
    left.swap(right);
    REQUIRE((fstd::get<0>(left) == 7 && fstd::get<1>(right).value == 2));
    REQUIRE(Tracked::alive == 1);
    right = 0;
    REQUIRE(Tracked::alive == 0);
}
