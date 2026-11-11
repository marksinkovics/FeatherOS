#include "command_line.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuchar>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <print>
#include <queue>
#include <ratio>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <span>
#include <stack>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <pthread.h>
#include <time.h>

// Checks FeatherOS's C++ standard library as std, on the compiler that builds programs; the
// unit tests check the same headers on the host in a namespace of their own.
constexpr char Help[] = "usage: cxxtest\n"
                        "Checks the C++ standard library's headers and prints how many checks "
                        "passed.";

namespace
{

struct Base
{
    virtual ~Base() = default;
};
struct Derived final : Base
{
};
struct Aggregate
{
    int value;
    auto operator<=>(const Aggregate&) const = default;
};
struct NotCopyable
{
    NotCopyable() = default;
    NotCopyable(const NotCopyable&) = delete;
    NotCopyable& operator=(const NotCopyable&) = delete;
};
union Union
{
    int integer;
    float real;
};
enum Plain : unsigned char
{
    PlainValue
};
enum class Scoped : short
{
    Value = 7
};
struct Callable
{
    int operator()(int value) const
    {
        return value + 1;
    }
    int Member(int value) const
    {
        return value * 2;
    }
    int field = 5;
};

// <type_traits>
static_assert(std::is_same_v<std::remove_cvref_t<const int&>, int>);
static_assert(std::is_same_v<std::decay_t<int[3]>, int*>);
static_assert(std::is_same_v<std::decay_t<int(int)>, int (*)(int)>);
static_assert(std::is_same_v<std::add_lvalue_reference_t<void>, void>);
static_assert(std::is_integral_v<const bool> && std::is_integral_v<char32_t> &&
              !std::is_integral_v<float>);
static_assert(std::is_floating_point_v<long double> && !std::is_floating_point_v<int>);
static_assert(std::is_function_v<int(int)> && !std::is_function_v<int (*)(int)>);
static_assert(std::is_class_v<Aggregate> && std::is_union_v<Union> && std::is_enum_v<Scoped>);
static_assert(std::is_scoped_enum_v<Scoped> && !std::is_scoped_enum_v<Plain>);
static_assert(std::is_same_v<std::underlying_type_t<Scoped>, short>);
static_assert(std::is_member_function_pointer_v<decltype(&Callable::Member)> &&
              std::is_member_object_pointer_v<decltype(&Callable::field)>);
static_assert(std::is_polymorphic_v<Base> && std::is_final_v<Derived> &&
              std::is_base_of_v<Base, Derived> && std::has_virtual_destructor_v<Base>);
static_assert(std::is_aggregate_v<Aggregate> && std::is_trivially_copyable_v<Aggregate>);
static_assert(!std::is_copy_constructible_v<NotCopyable> &&
              std::is_nothrow_default_constructible_v<NotCopyable>);
static_assert(std::is_trivially_destructible_v<int> && !std::is_trivially_destructible_v<Base>);
static_assert(std::is_destructible_v<int&> && !std::is_destructible_v<int[]> &&
              !std::is_destructible_v<void>);
static_assert(std::is_signed_v<int> && std::is_unsigned_v<unsigned> && !std::is_signed_v<bool>);
static_assert(std::is_same_v<std::make_signed_t<const unsigned long>, const long>);
static_assert(std::is_same_v<std::make_unsigned_t<Scoped>, unsigned short>);
static_assert(std::is_same_v<std::make_unsigned_t<char>, unsigned char>);
static_assert(std::rank_v<int[2][3]> == 2 && std::extent_v<int[2][3], 1> == 3);
static_assert(std::is_same_v<std::remove_all_extents_t<int[2][3]>, int>);
static_assert(std::is_convertible_v<Derived*, Base*> && !std::is_convertible_v<Base*, Derived*>);
static_assert(std::is_same_v<std::common_type_t<int, long, short>, long>);
static_assert(std::is_same_v<std::common_type_t<Derived*, Base*>, Base*>);
static_assert(std::is_same_v<std::common_reference_t<int&, const int&>, const int&>);
static_assert(std::is_same_v<std::common_reference_t<int&&, int&>, const int&>);
static_assert(std::is_same_v<std::common_reference_t<int&&, int&&>, int&&>);
static_assert(std::is_same_v<std::invoke_result_t<Callable, int>, int>);
static_assert(std::is_same_v<std::invoke_result_t<decltype(&Callable::field), Callable&>, int&>);
static_assert(std::is_invocable_r_v<long, decltype(&Callable::Member), const Callable*, int>);
static_assert(!std::is_invocable_v<Callable, Aggregate>);
static_assert(std::conjunction_v<std::true_type, std::is_integral<int>> &&
              std::disjunction_v<std::false_type, std::true_type> &&
              std::negation_v<std::false_type>);
static_assert(std::is_swappable_v<int> && !std::is_swappable_v<NotCopyable>);
static_assert(std::is_nothrow_swappable_v<int[4]>);

// <concepts> and <compare>
static_assert(std::same_as<int, int> && std::derived_from<Derived, Base>);
static_assert(std::integral<char> && std::signed_integral<long> && std::floating_point<double>);
static_assert(std::regular<Aggregate> && std::semiregular<int*> && !std::copyable<NotCopyable>);
static_assert(std::totally_ordered<double> && std::equality_comparable_with<int, long>);
static_assert(std::invocable<Callable, int> && std::predicate<bool (*)(int), int>);
static_assert(std::common_with<int, double> && std::common_reference_with<int&, const int&>);
static_assert(std::is_same_v<decltype(1 <=> 2), std::strong_ordering>);
static_assert(std::is_same_v<decltype(1.0 <=> 2.0), std::partial_ordering>);
static_assert(std::is_same_v<std::compare_three_way_result_t<Aggregate>, std::strong_ordering>);
static_assert(
    std::is_same_v<std::common_comparison_category_t<std::strong_ordering, std::weak_ordering>,
                   std::weak_ordering>);
static_assert(std::is_same_v<std::common_comparison_category_t<std::strong_ordering, int>, void>);
static_assert(std::three_way_comparable<int> && !std::three_way_comparable<NotCopyable>);
static_assert(std::is_lt(std::strong_ordering::less) && std::is_gteq(std::weak_ordering::greater));
static_assert(!std::is_lt(std::partial_ordering::unordered) &&
              !std::is_gteq(std::partial_ordering::unordered));
static_assert((0 <=> std::strong_ordering::less) == std::strong_ordering::greater);

// <utility>
static_assert(std::is_same_v<std::make_index_sequence<4>, std::index_sequence<0, 1, 2, 3>>);
static_assert(std::index_sequence_for<int, char>::size() == 2);
static_assert(std::cmp_less(-1, 0u) && !std::cmp_equal(-1, UINT_MAX) && std::cmp_greater(1u, -1));
static_assert(std::in_range<unsigned char>(255) && !std::in_range<unsigned char>(-1));
static_assert(std::to_underlying(Scoped::Value) == 7);
static_assert(std::pair{1, 2} < std::pair{1, 3} && std::pair{2, 0} > std::pair{1, 9});
static_assert(std::is_same_v<decltype(std::make_pair(1, 'a')), std::pair<int, char>>);
static_assert(std::tuple_size_v<const std::pair<int, char>> == 2);
static_assert(std::is_same_v<std::tuple_element_t<1, std::pair<int, char>>, char>);
static_assert(std::is_same_v<std::common_type_t<std::pair<int, short>, std::pair<long, int>>,
                             std::pair<long, int>>);
static_assert(!std::is_convertible_v<int, std::pair<int, int>>);

// <limits>, <bit> and <cstddef>
static_assert(std::numeric_limits<int>::max() == INT_MAX &&
              std::numeric_limits<int>::min() == INT_MIN);
static_assert(std::numeric_limits<unsigned long long>::max() == ULLONG_MAX);
static_assert(std::numeric_limits<signed char>::digits == 7 &&
              std::numeric_limits<unsigned>::digits10 == 9);
static_assert(std::numeric_limits<bool>::digits == 1 && std::numeric_limits<bool>::max());
static_assert(std::numeric_limits<char>::is_signed == (CHAR_MIN < 0));
static_assert(std::numeric_limits<double>::digits == DBL_MANT_DIG &&
              std::numeric_limits<double>::max_digits10 == 17);
static_assert(std::numeric_limits<float>::epsilon() == FLT_EPSILON &&
              std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<const long>::max() == LONG_MAX);
static_assert(!std::numeric_limits<Aggregate>::is_specialized);
static_assert(std::numeric_limits<unsigned __int128>::digits == 128);
#if defined(__x86_64__)
static_assert(std::numeric_limits<int>::traps);
#else
static_assert(!std::numeric_limits<int>::traps);
#endif
static_assert(std::popcount(0xF0F0u) == 8 && std::countl_zero(uint8_t{1}) == 7);
static_assert(std::countr_zero(uint64_t{0}) == 64 && std::countl_one(uint16_t{0xFF00}) == 8);
static_assert(std::bit_width(uint32_t{1024}) == 11 && std::bit_floor(1000u) == 512u);
static_assert(std::bit_ceil(uint8_t{65}) == 128 && std::has_single_bit(64ul));
static_assert(std::rotl(uint8_t{0x81}, 1) == 0x03 && std::rotr(uint8_t{0x81}, -1) == 0x03);
static_assert(std::byteswap(uint32_t{0x11223344}) == 0x44332211);
static_assert(std::bit_cast<uint32_t>(1.0f) == 0x3F800000);
static_assert(std::endian::native == std::endian::little);
static_assert(std::to_integer<int>(std::byte{0x0F} | std::byte{0xF0}) == 0xFF);
static_assert(std::is_same_v<std::size_t, decltype(sizeof(int))>);
static_assert(alignof(std::max_align_t) >= alignof(long double));

// <iterator>, <functional>, <array>, <span>, <string_view> and <optional>
constexpr std::array<int, 4> Numbers{4, 1, 3, 2};
constexpr int Total(std::span<const int> values)
{
    int sum = 0;
    for (int value : values)
    {
        sum += value;
    }
    return sum;
}
constexpr std::optional<int> Half(int value)
{
    if (value % 2 != 0)
    {
        return std::nullopt;
    }
    return value / 2;
}
static_assert(std::contiguous_iterator<int*> &&
              std::random_access_iterator<std::reverse_iterator<int*>>);
static_assert(std::ranges::contiguous_range<std::array<int, 4>> &&
              std::ranges::borrowed_range<std::span<int>> && std::ranges::view<std::string_view>);
static_assert(std::is_same_v<std::iter_value_t<const char*>, char>);
static_assert(std::invoke(&Aggregate::value, Aggregate{4}) == 4 &&
              std::invoke([](int value) { return value * 2; }, 3) == 6);
static_assert(std::plus<>{}(2, 3L) == 5 && std::ranges::less{}(1, 2) && std::identity{}(4) == 4);
static_assert(std::is_aggregate_v<std::array<int, 4>> && sizeof(std::array<char, 3>) == 3);
static_assert(Numbers[2] == 3 && *Numbers.rbegin() == 2 && Total(Numbers) == 10);
static_assert(std::to_array({1, 2}) == std::array{1, 2} && std::array{1, 2} < std::array{1, 3});
static_assert(sizeof(std::span<int, 4>) == sizeof(int*) &&
              !std::is_convertible_v<std::span<const int>, std::span<int>>);
static_assert(std::span(Numbers).subspan<1, 2>().back() == 3 && std::span(Numbers).last(1)[0] == 2);
static_assert(std::string_view("hello, world").find("world") == 7 &&
              std::string_view("abc").rfind('b') == 1);
static_assert(std::string_view("abc") < std::string_view("abd") && std::string_view("x") == "x");
static_assert(std::is_trivially_copyable_v<std::optional<int>> &&
              !std::is_copy_constructible_v<std::optional<NotCopyable>>);
static_assert(Half(8).and_then(Half) == 2 && !Half(3) && Half(3).value_or(-1) == -1);
static_assert(Half(4).transform([](int value) { return value * 1.5; }) == 3.0);
static_assert(std::optional<int>() < 0 && std::optional<int>(1) == 1 &&
              std::optional<int>() == std::nullopt);

// <tuple>, <variant> and <expected>
constexpr std::expected<int, const char*> Parse(int value)
{
    if (value < 0)
    {
        return std::unexpected("negative");
    }
    return value * 2;
}
static_assert(std::tuple_size_v<std::tuple<int, char>> == 2 &&
              std::is_same_v<std::tuple_element_t<1, std::tuple<int, char>>, char>);
static_assert(std::tuple{1, 'a'} < std::tuple{1, 'b'} &&
              std::get<long>(std::tuple<int, long>(1, 2)) == 2);
static_assert(
    std::tuple_size_v<decltype(std::tuple_cat(std::tuple{1}, std::pair{2, 3}, std::array{4, 5}))> ==
    5);
static_assert(std::apply([](int left, int right) { return left * right; }, std::pair{3, 4}) == 12);
static_assert(std::variant<int, double>(1.5f).index() == 1 &&
              !std::is_constructible_v<std::variant<int, long>, double>);
static_assert(std::is_trivially_copyable_v<std::variant<int, double>> &&
              sizeof(std::variant<int, char>) == 2 * sizeof(int));
static_assert(std::visit([](auto value) { return static_cast<int>(value) + 1; },
                         std::variant<int, double>(2.5)) == 3);
static_assert(*Parse(2).and_then(Parse) == 8 && Parse(-1).error()[0] == 'n' &&
              Parse(-1).value_or(0) == 0);
static_assert(std::is_trivially_copyable_v<std::expected<int, int>> &&
              std::is_same_v<decltype(Parse(1).transform([](int) { return 1.5; })),
                             std::expected<double, const char*>>);

// <memory> and <functional>
constexpr int OwnInConstantEvaluation()
{
    auto first = std::make_unique<int>(4);
    std::unique_ptr<int> second = std::move(first);
    std::allocator<long> allocator;
    long* values = allocator.allocate(2);
    std::construct_at(values, 5L);
    const long fifth = values[0];
    std::destroy_at(values);
    allocator.deallocate(values, 2);
    return *second + static_cast<int>(fifth) + (first ? 100 : 0);
}
static_assert(OwnInConstantEvaluation() == 9);
static_assert(sizeof(std::unique_ptr<int>) == sizeof(int*) &&
              sizeof(std::shared_ptr<int>) == 2 * sizeof(void*));
static_assert(std::bind_front(std::minus<>(), 10)(3) == 7 &&
              std::bind_back(std::minus<>(), 10)(3) == -7 && std::not_fn(std::equal_to<>())(1, 2));
static_assert(std::is_same_v<decltype(std::function([](int value) { return value; })),
                             std::function<int(int)>>);
static_assert(!std::is_default_constructible_v<std::hash<Aggregate>> &&
              std::is_default_constructible_v<std::hash<std::optional<int>>>);
static_assert(std::uses_allocator_v<std::tuple<int>, std::allocator<int>> &&
              std::is_same_v<std::allocator_traits<std::allocator<int>>::rebind_alloc<char>,
                             std::allocator<char>>);

// <algorithm>, <numeric> and <iterator>
constexpr std::array<int, 7> SortedConstants()
{
    std::array<int, 7> values{7, 3, 9, 1, 3, 8, 2};
    std::sort(values.begin(), values.end());
    return values;
}
constexpr int PermutationCount()
{
    std::array<int, 4> values{1, 2, 3, 4};
    int count = 1;
    while (std::next_permutation(values.begin(), values.end()))
    {
        ++count;
    }
    return count;
}
constexpr std::array<int, 7> Sorted = SortedConstants();
static_assert(Sorted == std::array<int, 7>{1, 2, 3, 3, 7, 8, 9});
static_assert(PermutationCount() == 24);
static_assert(std::binary_search(Sorted.begin(), Sorted.end(), 8) &&
              std::accumulate(Sorted.begin(), Sorted.end(), 0) == 33);
static_assert(std::gcd(84, -36) == 12 && std::lcm(4, 6) == 12 && std::midpoint(-3, 2) == -1);
static_assert(std::random_access_iterator<std::move_iterator<int*>> &&
              std::is_same_v<std::iter_reference_t<std::move_iterator<int*>>, int&&>);

// <vector>
constexpr int VectorConstant()
{
    std::vector<int> values{5, 1, 4};
    values.insert(values.begin() + 1, 3, 2);
    values.push_back(9);
    std::sort(values.begin(), values.end());
    return values[0] * 100 + static_cast<int>(values.size()) * 10 + values.back() % 10;
}
constexpr bool BitsConstant()
{
    std::vector<bool> bits(100, true);
    bits.flip();
    bits.push_back(true);
    return bits.size() == 101 && !bits[64] && bits.back();
}
struct TreeNode
{
    int value;
    std::vector<TreeNode> children;
};
struct Owned
{
    static inline int live = 0;
    int value;
    explicit Owned(int initial) : value(initial)
    {
        ++live;
    }
    Owned(Owned&& other) noexcept : value(other.value)
    {
        ++live;
    }
    Owned& operator=(Owned&& other) noexcept
    {
        value = other.value;
        return *this;
    }
    ~Owned()
    {
        --live;
    }
};
static_assert(VectorConstant() == 179 && BitsConstant());
static_assert(std::contiguous_iterator<std::vector<int>::iterator> &&
              std::random_access_iterator<std::vector<bool>::iterator> &&
              std::is_same_v<decltype(std::vector{1, 2}), std::vector<int>>);

constexpr bool StringConstant()
{
    std::string text = "constant";
    text += " evaluation past the local storage";
    text.insert(0, text, 0, 9);
    text.replace(0, 8, "C");
    text.erase(text.find(" past"));
    std::string local = "ab";
    local.insert(1, local);
    return text == "C constant evaluation" && local == "aabb" && local.capacity() == 15 &&
           std::string("x") + "y" == "xy";
}
constexpr bool CharconvConstant()
{
    char buffer[8] = {};
    const std::to_chars_result written = std::to_chars(buffer, buffer + 8, 255, 16);
    unsigned value = 0;
    std::from_chars(buffer, written.ptr, value, 16);
    return value == 255 && buffer[0] == 'f' && written.ptr == buffer + 2;
}
static_assert(StringConstant() && CharconvConstant());
static_assert(std::chrono::milliseconds(1500) ==
                  std::chrono::seconds(1) + std::chrono::milliseconds(500) &&
              std::chrono::floor<std::chrono::seconds>(std::chrono::milliseconds(-1)).count() ==
                  -1 &&
              std::is_same_v<std::ratio_add<std::ratio<1, 3>, std::ratio<1, 6>>, std::ratio<1, 2>>);
static_assert(sizeof(std::string) == 4 * sizeof(void*) &&
              std::contiguous_iterator<std::string::iterator> &&
              std::is_same_v<decltype(std::basic_string(std::string_view("x"))), std::string>);

// <deque>, <map>, <set>, <unordered_map> and <unordered_set>
static_assert(std::bidirectional_iterator<std::map<int, int>::iterator> &&
              std::bidirectional_iterator<std::multiset<int>::iterator> &&
              std::forward_iterator<std::unordered_map<int, int>::iterator> &&
              std::forward_iterator<std::unordered_set<int>::local_iterator> &&
              std::random_access_iterator<std::deque<int>::iterator>);
static_assert(std::is_same_v<decltype(std::map{std::pair{1, 'a'}}), std::map<int, char>> &&
              std::is_same_v<decltype(std::unordered_set{1, 2}), std::unordered_set<int>> &&
              std::is_same_v<decltype(std::deque{1.0}), std::deque<double>> &&
              std::is_same_v<std::set<int>::iterator, std::set<int>::const_iterator>);

struct ThreeBytes
{
    char values[3];
};

struct ThreeWords
{
    long values[3];
};

struct TwoInts
{
    int first;
    int second;
};

static_assert(std::atomic<long>::is_always_lock_free && std::atomic<void*>::is_always_lock_free &&
              !std::atomic<ThreeBytes>::is_always_lock_free &&
              !std::atomic<ThreeWords>::is_always_lock_free &&
              std::atomic_ref<TwoInts>::required_alignment == 8 && ATOMIC_LLONG_LOCK_FREE == 2);
static_assert(std::chrono::is_clock_v<std::chrono::system_clock> &&
              std::chrono::steady_clock::is_steady &&
              (std::chrono::sys_seconds(std::chrono::seconds(2)) + std::chrono::milliseconds(5))
                      .time_since_epoch()
                      .count() == 2005);

uint64_t passed_checks = 0;
bool failed = false;

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        printf("cxxtest: failed %s\n", name);
        failed = true;
        return;
    }
    ++passed_checks;
}

// Keeps a value from being folded into a constant, so the checks run the generated code.
template <class T> T Opaque(T value)
{
    volatile T copy = value;
    return copy;
}

int Sum(std::initializer_list<int> values)
{
    int sum = 0;
    for (int value : values)
    {
        sum += value;
    }
    return sum;
}

struct alignas(64) Overaligned
{
    int value = 3;
};

struct Tracked
{
    static inline int live = 0;
    int value;
    explicit Tracked(int initial) : value(initial)
    {
        ++live;
    }
    ~Tracked()
    {
        --live;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};

namespace Swappable
{
struct Counted
{
    int swaps = 0;
};
void swap(Counted& left, Counted& right)
{
    ++left.swaps;
    ++right.swaps;
}
} // namespace Swappable

void CheckLanguageSupport()
{
    Check(Sum({}) == 0 && Sum({1, 2, 3, 4}) == 10, "initializer_list from a braced list");
    const std::initializer_list<int> list = {5, 6, 7};
    Check(list.size() == 3 && *std::begin(list) == 5 && std::end(list)[-1] == 7,
          "initializer_list members");

    alignas(Tracked) unsigned char storage[sizeof(Tracked)];
    Tracked* tracked = new (storage) Tracked(Opaque(9));
    Check(Tracked::live == 1 && std::launder(reinterpret_cast<Tracked*>(storage))->value == 9,
          "placement new and launder");
    tracked->~Tracked();
    Check(Tracked::live == 0, "destructor after placement new");

    auto* overaligned = new Overaligned;
    Check(reinterpret_cast<uintptr_t>(overaligned) % 64 == 0 && overaligned->value == 3,
          "aligned operator new");
    delete overaligned;
    int* nothrow = new (std::nothrow) int(4);
    Check(nothrow != nullptr && *nothrow == 4, "nothrow operator new");
    delete nothrow;
}

void CheckUtility()
{
    int left = Opaque(1);
    int right = Opaque(2);
    std::swap(left, right);
    Check(left == 2 && right == 1, "swap");
    int first[3] = {1, 2, 3};
    int second[3] = {4, 5, 6};
    std::swap(first, second);
    Check(first[0] == 4 && second[2] == 3, "swap of arrays");
    Check(std::exchange(left, 7) == 2 && left == 7, "exchange");

    Swappable::Counted a;
    Swappable::Counted b;
    std::ranges::swap(a, b);
    Check(a.swaps == 1 && b.swaps == 1, "ranges::swap finds swap by argument-dependent lookup");
    std::ranges::swap(left, right);
    Check(left == 1 && right == 7, "ranges::swap moves");

    std::pair<int, const char*> pair(Opaque(3), "three");
    auto [number, name] = pair;
    Check(number == 3 && strcmp(name, "three") == 0, "structured binding of a pair");
    Check(std::get<0>(pair) == 3 && std::get<const char*>(pair) == pair.second, "get on a pair");
    std::pair<long, const char*> converted = pair;
    Check(converted.first == 3, "pair conversion");
    int target_first = 0;
    int target_second = 0;
    std::pair<int&, int&> references(target_first, target_second);
    references = std::pair<int, int>(8, 9);
    Check(target_first == 8 && target_second == 9, "assignment to a pair of references");
    Check((std::pair(Opaque(1), 2) <=> std::pair(1, Opaque(3))) < 0, "pair <=>");
    std::pair<int, int> one(1, 1);
    std::pair<int, int> two(2, 2);
    swap(one, two);
    Check(one.first == 2 && two.second == 1, "pair swap");

    Check(std::cmp_less(Opaque(-1), Opaque(1u)) && std::cmp_greater(Opaque(1u), Opaque(-1)),
          "cmp_less across signedness");
    const int constant = 4;
    Check(&std::as_const(left) == &left && std::forward_like<const int&>(left) == 1 &&
              std::move_if_noexcept(constant) == 4,
          "as_const, forward_like and move_if_noexcept");
}

void CheckOrdering()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto unordered = Opaque(nan) <=> 1.0;
    Check(unordered == std::partial_ordering::unordered && !(unordered < 0) && !(unordered >= 0),
          "partial_ordering of NaN");
    Check((Opaque(1) <=> 2) < 0 && (Opaque(2) <=> 2) == 0 && (Opaque(3) <=> 2) > 0,
          "strong_ordering of ints");
    const std::weak_ordering weak = Opaque(5) <=> 4;
    Check(weak > 0 && std::partial_ordering(weak) == std::partial_ordering::greater,
          "ordering conversions");
    Check(std::compare_three_way()(Opaque(1), 1L) == 0, "compare_three_way");
    Check((Aggregate{Opaque(1)} <=> Aggregate{2}) < 0 && Aggregate{3} == Aggregate{3},
          "defaulted <=> and ==");
}

void CheckLimitsAndBits()
{
    using Double = std::numeric_limits<double>;
    Check(Opaque(Double::infinity()) > Double::max() &&
              Opaque(-Double::infinity()) < Double::lowest(),
          "infinity");
    Check(Opaque(Double::quiet_NaN()) != Double::quiet_NaN(), "quiet NaN");
    Check(Opaque(Double::denorm_min()) > 0.0 && Opaque(Double::denorm_min()) < Double::min(),
          "denorm_min");
    Check(1.0 + Opaque(Double::epsilon()) != 1.0 && 1.0 + Opaque(Double::epsilon()) / 2 == 1.0,
          "epsilon");

    Check(std::popcount(Opaque(0xFFFF'0000'FFFFull)) == 32, "popcount");
    Check(std::countl_zero(Opaque(uint32_t{1})) == 31 &&
              std::countl_zero(Opaque(uint32_t{0})) == 32,
          "countl_zero");
    Check(std::countr_zero(Opaque(uint64_t{1} << 40)) == 40, "countr_zero");
    unsigned __int128 wide = Opaque(static_cast<unsigned __int128>(1)) << 100;
    Check(std::countl_zero(wide) == 27 && std::countr_zero(wide) == 100 && std::popcount(wide) == 1,
          "128-bit bit counts");
    Check(std::bit_width(Opaque(5u)) == 3 && std::bit_ceil(Opaque(5u)) == 8u &&
              std::bit_floor(Opaque(5u)) == 4u,
          "bit_width, bit_ceil and bit_floor");
    Check(std::rotl(Opaque(0x8000'0001u), 4) == 0x18u && std::rotr(Opaque(1u), 1) == 0x8000'0000u,
          "rotl and rotr");
    Check(std::byteswap(Opaque(uint16_t{0x1234})) == 0x3412 &&
              std::byteswap(Opaque(uint64_t{0x0102030405060708})) == 0x0807060504030201,
          "byteswap");
    Check(std::bit_cast<double>(Opaque(uint64_t{0x3FF0000000000000})) == 1.0, "bit_cast");
}

void CheckVocabulary()
{
    using namespace std::literals;

    auto [first, second, third] = std::array{Opaque(1), 2, 3};
    Check(first + second + third == 6, "structured bindings of array");
    std::array<int, 3> numbers{Opaque(3), 1, 2};
    std::array<int, 3> other = numbers;
    other.back() = Opaque(5);
    Check(numbers.at(2) == 2 && numbers < other && numbers != other, "array access and order");

    int reversed = 0;
    for (auto it = std::rbegin(numbers); it != std::rend(numbers); ++it)
    {
        reversed = reversed * 10 + *it;
    }
    Check(reversed == 213 && std::distance(numbers.begin(), numbers.end()) == 3,
          "reverse_iterator and distance");

    std::span<int> all(numbers);
    all.subspan(1)[0] = Opaque(7);
    Check(numbers[1] == 7 && Total(all) == 12 && all.first<2>().size() == 2, "span over an array");
    uint32_t word = Opaque(0x11223344u);
    auto bytes = std::as_bytes(std::span(&word, 1));
    Check(bytes.size() == 4 && bytes[0] == std::byte{0x44}, "as_bytes");
    Check(std::ranges::size(numbers) == 3 && std::ranges::data(all) == numbers.data(),
          "ranges::size and ranges::data");

    const char* text = Opaque("key=value; flag");
    std::string_view view = text;
    std::size_t equals = view.find('=');
    Check(equals == 3 && view.substr(0, equals) == "key" && view.substr(equals + 1, 5) == "value",
          "string_view find and substr");
    Check(view.find_last_of(';') == 9 && view.find_first_not_of("key") == 3 &&
              view.ends_with("flag"sv) && view.contains("lue"),
          "string_view searches");
    const char high[] = {static_cast<char>(Opaque(0x80))};
    Check(std::string_view(high, 1) > "\x01"sv, "string_view compares bytes unsigned");
    char buffer[] = "abcdef";
    std::char_traits<char>::move(buffer + Opaque(1), buffer, 4);
    Check(std::string_view(buffer) == "aabcdf", "char_traits::move overlapping");

    {
        std::optional<Tracked> tracked(std::in_place, Opaque(4));
        Check(Tracked::live == 1 && tracked->value == 4, "optional constructs in place");
        tracked.reset();
        Check(Tracked::live == 0 && !tracked.has_value(), "optional destroys on reset");
        tracked.emplace(Opaque(6));
    }
    Check(Tracked::live == 0, "optional destroys at scope end");

    std::optional<int> parsed = Half(Opaque(12));
    Check(parsed.and_then(Half).transform([](int value) { return value + 1; }) == 4 &&
              !Half(Opaque(5)).has_value() && Half(Opaque(5)).value_or(-1) == -1,
          "optional chains");
    std::optional<std::string_view> word_view = "chain"sv;
    Check(word_view.transform(&std::string_view::size) == 5u, "optional transform via invoke");

    Callable callable;
    Check(std::invoke(&Callable::field, std::ref(callable)) == 5 &&
              std::invoke(&Callable::Member, &callable, Opaque(4)) == 8,
          "invoke through pointers and reference_wrapper");
}

void CheckSumTypes()
{
    std::tuple<int, const char*, long> record(Opaque(1), "one", 2L);
    auto [number, name, wide] = record;
    Check(number == 1 && strcmp(name, "one") == 0 && wide == 2, "structured bindings of tuple");
    int first = 0;
    long second = 0;
    std::tie(first, std::ignore, second) = record;
    Check(first == 1 && second == 2, "tie and ignore");
    auto joined = std::tuple_cat(record, std::pair(Opaque(3), 'c'), std::array{Opaque(4)});
    Check(std::get<3>(joined) == 3 && std::get<5>(joined) == 4 &&
              (std::tuple(1, 2) <=> std::tuple(1, Opaque(3))) < 0,
          "tuple_cat and tuple <=>");
    std::pair<std::string_view, std::tuple<int, int>> piecewise(
        std::piecewise_construct, std::forward_as_tuple("key", Opaque(2)),
        std::forward_as_tuple(Opaque(5), 6));
    Check(piecewise.first == "ke" && std::get<0>(piecewise.second) == 5,
          "pair's piecewise constructor");

    {
        std::variant<int, Tracked> value(std::in_place_type<Tracked>, Opaque(3));
        Check(Tracked::live == 1 && std::get<Tracked>(value).value == 3 &&
                  std::holds_alternative<Tracked>(value),
              "variant constructs in place");
        value = Opaque(4);
        Check(Tracked::live == 0 && std::get<int>(value) == 4 &&
                  std::get_if<Tracked>(&value) == nullptr,
              "variant destroys the old alternative");
        value.emplace<Tracked>(Opaque(5));
    }
    Check(Tracked::live == 0, "variant destroys at scope end");
    std::variant<int, double> number_or_real = Opaque(2.5);
    std::variant<long, char> other = Opaque('a');
    Check(std::visit([](auto left, auto right) { return static_cast<long>(left * 2) + right; },
                     number_or_real, other) == 102,
          "visit of two variants");
    Check(std::variant<int, double>(Opaque(1)) < std::variant<int, double>(0.5) &&
              std::variant<int, double>(Opaque(2)) == std::variant<int, double>(2),
          "variant comparisons");

    std::expected<int, const char*> parsed = Parse(Opaque(5));
    Check(parsed.transform([](int value) { return value + 1; }) == 11 &&
              Parse(Opaque(-5)).or_else([](const char*) { return Parse(1); }) == 2,
          "expected chains");
    std::expected<void, int> checked = std::unexpected(Opaque(7));
    Check(!checked && checked.error() == 7 &&
              checked.transform_error([](int error) { return error * 2; }).error() == 14,
          "expected of void");
}

struct Counted : Base
{
    static inline int live = 0;
    Counted()
    {
        ++live;
    }
    ~Counted() override
    {
        --live;
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
};

struct Node : std::enable_shared_from_this<Node>
{
    int value = 6;
};

void CheckOwnership()
{
    {
        std::unique_ptr<Base> owner = std::make_unique<Counted>();
        Check(Counted::live == 1, "make_unique into a unique_ptr to a base");
        owner.reset(new Counted);
        Check(Counted::live == 1 && owner != nullptr, "unique_ptr::reset destroys the old object");
        std::unique_ptr<int[]> values = std::make_unique<int[]>(Opaque(3));
        values[2] = 7;
        Check(values[0] == 0 && values[2] == 7, "unique_ptr of an array");
    }
    Check(Counted::live == 0, "unique_ptr destroys through a virtual destructor");

    std::weak_ptr<Base> observer;
    {
        std::shared_ptr<Base> first = std::make_shared<Counted>();
        std::shared_ptr<Base> second = first;
        observer = second;
        Check(first.use_count() == 2 && Counted::live == 1 && observer.lock() == first,
              "shared_ptr counts its owners");
        first.reset();
        Check(Counted::live == 1 && !observer.expired(),
              "shared_ptr keeps the object while an owner remains");
    }
    Check(Counted::live == 0 && observer.expired() && !observer.lock(),
          "the last owner destroys and weak_ptr expires");

    auto node = std::make_shared<Node>();
    std::shared_ptr<Node> again = node->shared_from_this();
    Check(again == node && node.use_count() == 2 && again->value == 6, "enable_shared_from_this");

    int deleted = 0;
    {
        auto deleter = [&deleted](int* pointer)
        {
            ++deleted;
            delete pointer;
        };
        std::shared_ptr<int> owner(new int(Opaque(4)), deleter);
        std::shared_ptr<int> copy = owner;
        owner.reset();
        Check(deleted == 0 && std::get_deleter<decltype(deleter)>(copy) != nullptr && *copy == 4,
              "get_deleter finds a lambda");
    }
    Check(deleted == 1, "shared_ptr's deleter runs once");

    auto pair = std::make_shared<std::pair<int, long>>(Opaque(1), 2L);
    std::shared_ptr<long> member(pair, &pair->second);
    std::shared_ptr<const std::pair<int, long>> constant = pair;
    Check(*member == 2 && pair.use_count() == 3 && constant->first == 1, "aliasing shared_ptr");
    std::shared_ptr<Base> base = std::make_shared<Counted>();
    auto counted = std::static_pointer_cast<Counted>(base);
    Check(static_cast<Base*>(counted.get()) == base.get() && base.use_count() == 2,
          "static_pointer_cast");

    std::allocator<Overaligned> allocator;
    Overaligned* aligned = allocator.allocate(Opaque(2));
    Check(reinterpret_cast<uintptr_t>(aligned) % 64 == 0, "allocator of an over-aligned type");
    allocator.deallocate(aligned, 2);

    std::tuple<std::pair<int, int>, int> with_allocator(std::allocator_arg, std::allocator<int>(),
                                                        std::pair(Opaque(1), 2), 3);
    Check(std::get<0>(with_allocator).first == 1 && std::get<1>(with_allocator) == 3,
          "tuple's allocator-extended constructor");
}

void CheckCallables()
{
    std::function<int(int)> empty;
    Check(!empty && empty == nullptr, "an empty function");
    const long weights[6] = {1, 2, 3, 4, 5, Opaque(6L)};
    auto large = [weights](int value) { return static_cast<int>(weights[5]) * value; };
    std::function<int(int)> small = [offset = Opaque(1)](int value) { return value + offset; };
    std::function<int(int)> heap = large;
    Check(small(2) == 3 && heap(2) == 12, "function holds small and large callables");
    std::function<int(int)> copy = heap;
    small.swap(copy);
    Check(small(1) == 6 && copy(1) == 2 && heap.target<decltype(large)>() != nullptr,
          "function copies, swaps and finds its target");
    Check(std::not_fn([](int value) { return value > 2; })(Opaque(1)) &&
              std::bind_front(std::minus<>(), Opaque(10))(3) == 7 &&
              std::bind_back(std::minus<>(), Opaque(10))(3) == -7,
          "not_fn, bind_front and bind_back");
    Check(std::mem_fn(&Callable::Member)(Callable(), Opaque(4)) == 8, "mem_fn");
    const char text[] = "abcabc";
    Check(std::hash<std::string_view>()(std::string_view(text, 3)) ==
              std::hash<std::string_view>()(std::string_view(text + 3, 3)),
          "hash of equal string views");
    Check(std::hash<double>()(Opaque(0.0)) == std::hash<double>()(Opaque(-0.0)) &&
              std::hash<long double>()(-0.0L) == 0 && std::hash<long double>()(1.0L) != 0 &&
              std::hash<std::optional<int>>()(Opaque(5)) == std::hash<int>()(5),
          "hash of zeros and of optional");
}

// A linear congruential generator, as a uniform random bit generator for shuffle.
struct Lcg
{
    using result_type = uint32_t;
    uint64_t state;
    static constexpr result_type min()
    {
        return 0;
    }
    static constexpr result_type max()
    {
        return UINT32_MAX;
    }
    result_type operator()()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<result_type>(state >> 32);
    }
};

struct Keyed
{
    int key;
    int tag;
};

// A container with only push_back, for back_inserter.
struct Collector
{
    using value_type = int;
    int values[8] = {};
    int size = 0;
    void push_back(int value)
    {
        values[size++] = value;
    }
};

void CheckAlgorithms()
{
    Lcg generator{Opaque(uint64_t{42})};
    int values[1000];
    for (int& value : values)
    {
        value = static_cast<int>(generator() % 500);
    }
    int copy[1000];
    std::copy(values, values + 1000, copy);
    std::sort(values, values + 1000);
    Check(std::is_sorted(values, values + 1000) && std::is_permutation(values, values + 1000, copy),
          "sort");
    std::nth_element(copy, copy + 300, copy + 1000);
    Check(copy[300] == values[300], "nth_element");

    Keyed keyed[200];
    for (int index = 0; index != 200; ++index)
    {
        keyed[index] = Keyed{static_cast<int>(generator() % 7), index};
    }
    std::stable_sort(keyed, keyed + 200,
                     [](const Keyed& left, const Keyed& right) { return left.key < right.key; });
    bool stable = true;
    for (int index = 1; index != 200; ++index)
    {
        stable =
            stable &&
            (keyed[index - 1].key < keyed[index].key ||
             (keyed[index - 1].key == keyed[index].key && keyed[index - 1].tag < keyed[index].tag));
    }
    Check(stable, "stable_sort keeps equal keys in order");

    int heap[6] = {Opaque(4), 9, 2, 7, 5, 1};
    std::make_heap(heap, heap + 6);
    std::pop_heap(heap, heap + 6);
    Check(heap[5] == 9 && std::is_heap(heap, heap + 5), "heap operations");

    int sequence[6] = {Opaque(1), 2, 3, 4, 5, 6};
    int* moved = std::rotate(sequence, sequence + 2, sequence + 6);
    std::reverse(sequence, sequence + 2);
    Check(moved == sequence + 4 && sequence[0] == 4 && sequence[1] == 3 && sequence[5] == 2,
          "rotate and reverse");
    std::copy(sequence + 1, sequence + 6, sequence);
    Check(sequence[0] == 3 && sequence[4] == 2, "copy into an overlapping range");

    const int left[4] = {Opaque(1), 3, 5, 7};
    const int right[4] = {Opaque(3), 4, 5, 8};
    int merged[8];
    int common[4];
    std::merge(left, left + 4, right, right + 4, merged);
    int* common_end = std::set_intersection(left, left + 4, right, right + 4, common);
    Check(std::is_sorted(merged, merged + 8) && common_end == common + 2 && common[1] == 5,
          "merge and set_intersection");

    int runs[8] = {Opaque(1), 1, 2, 3, 3, 3, 4, 1};
    int* unique_end = std::unique(runs, runs + 8);
    int* removed_end = std::remove(runs, unique_end, 3);
    Check(removed_end - runs == 4 && runs[2] == 4 && runs[3] == 1, "unique and remove");

    int counted[5];
    std::iota(counted, counted + 5, Opaque(1));
    int sums[5];
    std::inclusive_scan(counted, counted + 5, sums);
    Check(sums[4] == 15 && std::reduce(counted, counted + 5) == 15 &&
              std::inner_product(counted, counted + 5, counted, 0) == 55,
          "iota, inclusive_scan, reduce and inner_product");

    int deck[52];
    std::iota(deck, deck + 52, 0);
    std::shuffle(deck, deck + 52, generator);
    int sorted_deck[52];
    std::copy(deck, deck + 52, sorted_deck);
    std::sort(sorted_deck, sorted_deck + 52);
    Check(sorted_deck[0] == 0 && sorted_deck[51] == 51 &&
              std::adjacent_find(sorted_deck, sorted_deck + 52) == sorted_deck + 52 &&
              !std::is_sorted(deck, deck + 52),
          "shuffle");

    Collector collector;
    std::copy_if(counted, counted + 5, std::back_inserter(collector),
                 [](int value) { return value % 2 == 1; });
    Check(collector.size == 3 && collector.values[2] == 5, "back_inserter");

    const auto [smallest, largest] = std::minmax_element(left, left + 4);
    Check(*smallest == 1 && *largest == 7 && std::clamp(Opaque(9), 0, 5) == 5,
          "minmax_element and clamp");

    alignas(Tracked) unsigned char storage[3 * sizeof(Tracked)];
    auto* tracked = reinterpret_cast<Tracked*>(storage);
    for (int index = 0; index != 3; ++index)
    {
        std::construct_at(tracked + index, Opaque(index));
    }
    const bool constructed = Tracked::live == 3 && tracked[2].value == 2;
    std::destroy(tracked, tracked + 3);
    Check(constructed && Tracked::live == 0, "construct_at and destroy");
}

void CheckContainers()
{
    std::vector<int> grown;
    for (int index = 0; index != 1000; ++index)
    {
        grown.push_back(Opaque(index));
    }
    Check(grown.size() == 1000 && grown.capacity() >= 1000 && grown[999] == 999 &&
              std::accumulate(grown.begin(), grown.end(), 0) == 499500,
          "vector grows by push_back");

    std::vector<int> edited{Opaque(1), 2, 3, 4, 5};
    edited.insert(edited.begin() + 2, {7, 8});
    edited.erase(edited.begin());
    edited.insert(edited.end() - 1, 2, edited[0]);
    Check(edited == std::vector<int>{2, 7, 8, 3, 4, 2, 2, 5}, "vector insert and erase");

    std::vector<int> full;
    full.reserve(2);
    full.push_back(Opaque(6));
    full.push_back(7);
    full.push_back(full[0]);
    Check(full.size() == 3 && full[2] == 6, "push_back of an element while growing");

    {
        std::vector<Owned> owned;
        for (int index = 0; index != 20; ++index)
        {
            owned.emplace_back(Opaque(index));
        }
        owned.erase(owned.begin() + 3, owned.begin() + 13);
        Check(Owned::live == 10 && owned[3].value == 13, "vector of a move-only type");
    }
    Check(Owned::live == 0, "vector destroys its elements");

    std::vector<std::unique_ptr<int>> pointers;
    pointers.push_back(std::make_unique<int>(Opaque(4)));
    pointers.insert(pointers.begin(), std::make_unique<int>(3));
    Check(*pointers[0] == 3 && *pointers[1] == 4, "vector of unique_ptr");

    TreeNode root{1, {}};
    root.children.push_back(TreeNode{2, {}});
    root.children[0].children.push_back(TreeNode{Opaque(3), {}});
    const TreeNode copy = root;
    Check(copy.children[0].children[0].value == 3, "vector of an incomplete type");

    std::vector<int> erased{Opaque(1), 2, 1, 3};
    Check(std::erase(erased, 1) == 2 && erased == std::vector<int>{2, 3} &&
              (std::vector<int>{1, 2} <=> std::vector<int>{1, 3}) < 0,
          "erase and <=> of vectors");

    std::vector<bool> bits;
    for (int index = 0; index != 200; ++index)
    {
        bits.push_back(Opaque(index) % 3 == 0);
    }
    bits.insert(bits.begin() + 1, 70, true);
    Check(bits.size() == 270 && std::count(bits.begin(), bits.end(), true) == 67 + 70 && bits[1] &&
              bits[70] && !bits[72],
          "vector<bool>");

    const std::array<int, 3> source{Opaque(4), 5, 6};
    std::vector<int> ranged(std::from_range, source);
    ranged.append_range(source);
    Check(ranged.size() == 6 && ranged[3] == 4, "vector from a range");
}

// A linear congruential generator, so the keys come in no particular order.
unsigned NextKey(unsigned& state)
{
    state = state * 1103515245U + 12345U;
    return (state >> 16) % 4096;
}

void CheckAssociative()
{
    std::map<unsigned, unsigned> ordered;
    unsigned state = Opaque(1U);
    for (int index = 0; index != 2000; ++index)
    {
        const unsigned key = NextKey(state);
        ordered[key] += 1;
    }
    unsigned previous = 0;
    unsigned total = 0;
    bool in_order = true;
    for (const auto& [key, count] : ordered)
    {
        in_order = in_order && (total == 0 || key > previous);
        previous = key;
        total += count;
    }
    Check(in_order && total == 2000 && ordered.size() < 2000, "map keeps its keys in order");

    std::map<int, std::string> names;
    names.try_emplace(Opaque(2), "two");
    names.try_emplace(2, "zwei");
    names.insert_or_assign(3, "three");
    names.insert_or_assign(3, "drei");
    names.emplace(1, "one");
    Check(names.at(2) == "two" && names[3] == "drei" && names.begin()->second == "one",
          "map try_emplace, insert_or_assign and at");

    std::map<int, int> ranges{{1, 1}, {3, 3}, {5, 5}, {7, 7}};
    ranges.erase(ranges.lower_bound(Opaque(3)), ranges.upper_bound(5));
    Check(ranges.size() == 2 && ranges.lower_bound(2)->first == 7 && ranges.find(5) == ranges.end(),
          "map erase and bounds");

    std::multimap<int, int> repeated;
    for (int index = 0; index != 6; ++index)
    {
        repeated.emplace(Opaque(index) % 2, index);
    }
    const auto [odd_first, odd_last] = repeated.equal_range(1);
    Check(std::distance(odd_first, odd_last) == 3 && odd_first->second == 1 &&
              std::next(odd_first, 2)->second == 5,
          "multimap keeps equal keys in insertion order");

    std::set<std::string, std::less<>> words{"feather", "kernel", "swift"};
    Check(words.contains(std::string_view("kernel")) && words.find("rust") == words.end() &&
              *words.rbegin() == "swift",
          "set with a transparent comparator");

    std::map<int, std::string> source{{1, "a"}, {2, "b"}};
    std::map<int, std::string> target{{2, "x"}};
    auto node = source.extract(Opaque(1));
    node.key() = 9;
    const auto inserted = target.insert(std::move(node));
    target.merge(source);
    Check(inserted.inserted && target.at(9) == "a" && source.size() == 1 && target.size() == 2,
          "map node handles and merge");

    std::unordered_map<unsigned, unsigned> hashed;
    state = Opaque(7U);
    for (int index = 0; index != 3000; ++index)
    {
        hashed[NextKey(state) * 1024] += 1;
    }
    bool in_bucket = hashed.load_factor() <= hashed.max_load_factor();
    for (std::size_t bucket = 0; bucket != hashed.bucket_count(); ++bucket)
    {
        for (auto position = hashed.begin(bucket); position != hashed.end(bucket); ++position)
        {
            in_bucket = in_bucket && hashed.bucket(position->first) == bucket;
        }
    }
    unsigned hashed_total = 0;
    for (const auto& [key, count] : hashed)
    {
        hashed_total += count;
    }
    Check(in_bucket && hashed_total == 3000, "unordered_map buckets and load factor");

    const unsigned erased_key = hashed.begin()->first;
    const std::size_t before = hashed.size();
    hashed.erase(erased_key);
    std::erase_if(hashed, [](const auto& entry) { return entry.second > 1; });
    Check(!hashed.contains(erased_key) && hashed.size() < before &&
              std::all_of(hashed.begin(), hashed.end(),
                          [](const auto& entry) { return entry.second == 1; }),
          "unordered_map erase and erase_if");

    std::unordered_map<std::string, int> counts;
    for (const char* word : {"to", "be", "or", "not", "to", "be"})
    {
        ++counts[Opaque(word)];
    }
    Check(counts.size() == 4 && counts["to"] == 2 && counts.at("not") == 1,
          "unordered_map of strings");

    const std::unordered_multiset<int> left{Opaque(1), 2, 2, 3};
    const std::unordered_multiset<int> right{2, 3, 2, 1};
    Check(left == right && left.count(2) == 2 && left != std::unordered_multiset<int>{1, 2, 3, 3},
          "unordered_multiset counts and compares");

    std::deque<int> both_ends;
    for (int index = 0; index != 1000; ++index)
    {
        both_ends.push_back(Opaque(index));
        both_ends.push_front(-index);
    }
    Check(both_ends.size() == 2000 && both_ends.front() == -999 && both_ends.back() == 999 &&
              both_ends[1000] == 0,
          "deque grows at both ends");

    std::deque<int> middle{Opaque(1), 2, 3, 4, 5};
    middle.insert(middle.begin() + 2, 3, middle[0]);
    middle.erase(middle.begin() + 1);
    middle.emplace(middle.end() - 1, 9);
    Check(middle == std::deque<int>{1, 1, 1, 1, 3, 4, 9, 5}, "deque insert and erase");

    std::deque<int> queue;
    for (int index = 0; index != 3000; ++index)
    {
        queue.push_back(Opaque(index));
    }
    const int* const held = &queue.back();
    for (int index = 0; index != 2500; ++index)
    {
        queue.push_back(index);
        queue.pop_front();
    }
    Check(*held == 2999 && queue.size() == 3000 && queue.front() == 2500,
          "deque as a queue keeps references");

    {
        std::map<int, Owned> owned_map;
        std::unordered_map<int, Owned> owned_hash;
        std::deque<Owned> owned_deque;
        for (int index = 0; index != 50; ++index)
        {
            owned_map.emplace(index, Owned(Opaque(index)));
            owned_hash.emplace(index, Owned(index));
            owned_deque.emplace_front(index);
        }
        owned_map.erase(10);
        owned_hash.erase(20);
        owned_deque.pop_back();
    }
    Check(Owned::live == 0, "map, unordered_map and deque destroy their elements");
}

// The long double layout differs: x87's 80 bits on x86-64, IEEE quad on AArch64.
std::string LongDoubleText(long double value)
{
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

std::string DoubleText(double value, std::chars_format format, int precision = -1)
{
    char buffer[400];
    const std::to_chars_result result =
        precision < 0 ? std::to_chars(buffer, buffer + sizeof(buffer), value, format)
                      : std::to_chars(buffer, buffer + sizeof(buffer), value, format, precision);
    return std::string(buffer, result.ptr);
}

void CheckStrings()
{
    using namespace std::string_literals;
    std::string grown;
    for (int index = 0; index != 100; ++index)
    {
        grown.push_back(static_cast<char>('a' + Opaque(index) % 26));
    }
    Check(grown.size() == 100 && grown.capacity() >= 100 && grown[26] == 'a' &&
              grown.c_str()[100] == '\0' && std::strlen(grown.c_str()) == 100,
          "string grows past its local storage");

    std::string aliased = Opaque(0) == 0 ? "abcdefghij" : "";
    aliased.insert(2, aliased);
    aliased.replace(0, 4, aliased.c_str() + 10, 3);
    aliased.append(aliased, 0, 2);
    Check(aliased == "ijccdefghijcdefghijij", "string arguments from the string itself");

    const std::string joined = "feather"s + '-' + std::string(Opaque(2), 'o') + "s";
    Check(joined == "feather-oos" && joined < "feathers" && (joined <=> "feather-oos"s) == 0 &&
              joined.starts_with("feather") && joined.ends_with('s'),
          "string concatenation and comparison");

    const std::string text = "the quick brown fox jumps over the lazy dog";
    Check(text.find("the", 1) == 31 && text.rfind('o') == 41 && text.find_first_of("xyz") == 18 &&
              text.find_last_not_of("dog") == 39 && text.substr(4, 5) == "quick" &&
              text.contains("lazy"),
          "string searches");

    std::string heap(40, Opaque('h'));
    std::string local = "local";
    heap.swap(local);
    std::string moved = std::move(local);
    // A moved-from string is empty and local here, which the check reads on purpose.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    const bool emptied = local.empty() && local.capacity() == 15;
    Check(heap == "local" && moved.size() == 40 && emptied, "string swap and move");

    Check(std::hash<std::string>{}(text) == std::hash<std::string_view>{}(text),
          "hash of a string and its view");

    std::size_t end = 0;
    Check(std::to_string(Opaque(-1234)) == "-1234" && std::to_string(Opaque(2.5)) == "2.500000" &&
              std::stoi("  42x"s, &end) == 42 && end == 4 &&
              std::stoull("ff"s, nullptr, 16) == 255 && std::stod("1.25e2"s) == 125.0,
          "to_string and sto*");

    std::u32string wide = U"wide";
    wide += U" text past the local storage";
    std::u16string sixteen(Opaque(20), u'x');
    sixteen.replace(2, 16, u"ab");
    Check(wide.size() == 32 && wide.find(U"past") == 10 && sixteen == u"xxabxx",
          "u16string and u32string");

    std::string overwritten = "keep";
    overwritten.resize_and_overwrite(20,
                                     [](char* data, std::size_t count)
                                     {
                                         std::memset(data + 4, '+', count - 4);
                                         return count - 10;
                                     });
    std::erase_if(overwritten, [](char character) { return character == 'e'; });
    Check(overwritten == "kp++++++", "resize_and_overwrite and erase_if");

    char buffer[80];
    std::to_chars_result written = std::to_chars(buffer, buffer + 80, Opaque(-255LL), 2);
    long long parsed = 0;
    std::from_chars_result read = std::from_chars(buffer, written.ptr, parsed, 2);
    int small = 0;
    const char* too_big = "99999999999";
    Check(std::string(buffer, written.ptr) == "-11111111" && parsed == -255 &&
              read.ptr == written.ptr &&
              std::from_chars(too_big, too_big + 11, small).ec == std::errc::result_out_of_range &&
              std::to_chars(buffer, buffer + 2, 1000).ec == std::errc::value_too_large,
          "integer to_chars and from_chars");

    Check(DoubleText(Opaque(0.1), std::chars_format::general) == "0.1" &&
              DoubleText(1e23, std::chars_format::general) == "1e+23" &&
              DoubleText(Opaque(5e-324), std::chars_format::scientific) == "5e-324" &&
              DoubleText(1e23, std::chars_format::fixed) == "99999999999999991611392" &&
              DoubleText(Opaque(123456.0), std::chars_format::general) == "123456",
          "shortest double to_chars");

    Check(DoubleText(Opaque(2.0 / 3), std::chars_format::fixed, 4) == "0.6667" &&
              DoubleText(1e-10, std::chars_format::scientific, 2) == "1.00e-10" &&
              DoubleText(Opaque(3.0), std::chars_format::hex) == "1.8p+1" &&
              DoubleText(-0.0, std::chars_format::general, 3) == "-0",
          "double to_chars with a precision");

    double value = 0;
    const char* scientific = "1.7976931348623157e308";
    const char* overflow = "1e400";
    const char* hex = "1.8p+1";
    Check(std::from_chars(scientific, scientific + 22, value).ec == std::errc{} &&
              value == DBL_MAX &&
              std::from_chars(overflow, overflow + 5, value).ec == std::errc::result_out_of_range &&
              value == DBL_MAX &&
              std::from_chars(hex, hex + 6, value, std::chars_format::hex).ptr == hex + 6 &&
              value == 3.0,
          "double from_chars");

    // Compared as text: AArch64 has no compiler runtime for long double comparisons.
    const std::string tenth = LongDoubleText(Opaque(0.1L));
    const char* third = "0.333333333333333333333333333333333333333";
    long double back = 0;
    std::from_chars(third, third + std::strlen(third), back);
    Check(tenth == "0.1" && LongDoubleText(back).starts_with("0.3333333333333333333") &&
              LongDoubleText(LDBL_MAX).size() > 20,
          "long double to_chars and from_chars");
}

void CheckCLibrary()
{
    char text[] = "feather";
    char* found = std::strchr(text, 't');
    const char* found_const = std::strchr(static_cast<const char*>(text), 'h');
    Check(found == text + 3 && found_const == text + 4, "strchr keeps the argument's constness");
    Check(std::strstr(text, "the") == text + 3 && std::strlen(text) == 7, "strstr and strlen");
    Check(std::memchr(text, 'r', sizeof(text)) == text + 6, "memchr");
    Check(std::abs(Opaque(-3)) == 3 && std::abs(Opaque(-4L)) == 4 && std::abs(Opaque(-2.5)) == 2.5,
          "abs overloads");
    const std::ldiv_t quotient = std::div(Opaque(17L), 5L);
    Check(quotient.quot == 3 && quotient.rem == 2, "div overloads");
    Check(std::strtol("42", nullptr, 10) == 42 && std::strtoul("7", nullptr, 10) == 7,
          "strtol and strtoul");
    Check(std::toupper('a') == 'A' && std::isdigit('5') != 0, "cctype");
    char buffer[16];
    Check(std::snprintf(buffer, sizeof(buffer), "%d-%s", 5, "x") == 3 &&
              std::strcmp(buffer, "5-x") == 0,
          "snprintf");
    std::time_t now = std::time(nullptr);
    Check(now > 0 && std::gmtime(&now) != nullptr, "time and gmtime");
    std::uintmax_t parsed = std::strtoumax("123", nullptr, 10);
    Check(parsed == 123 && std::imaxabs(-5) == 5, "cinttypes");
    errno = 0;
    Check(std::strtol("99999999999999999999", nullptr, 10) == LONG_MAX && errno == ERANGE,
          "cerrno");
}

static_assert(alignof(std::aligned_storage_t<24>) == 16 && std::is_pod_v<ThreeWords>);

void CheckAdaptorsAndMath()
{
    std::bitset<100> bits(Opaque(0b1011ULL));
    bits <<= 70;
    bits.flip(99);
    Check(bits.count() == 4 && bits.test(71) && !bits.test(72) &&
              (bits >> 70).to_ulong() == (0b1011UL | 1UL << 29) &&
              std::bitset<6>(5).to_string() == "000101" && std::bitset<8>("1100").to_ulong() == 12,
          "bitset");

    std::queue<int> queue;
    std::stack<std::string> stack;
    for (int value = 0; value != 50; ++value)
    {
        queue.push(Opaque(value));
        stack.push(std::to_string(value));
    }
    queue.pop();
    stack.pop();
    Check(queue.size() == 49 && queue.front() == 1 && queue.back() == 49 && stack.top() == "48",
          "queue and stack");

    std::priority_queue<int> largest;
    std::priority_queue<int, std::vector<int>, std::greater<>> smallest;
    unsigned state = 7;
    for (int index = 0; index != 200; ++index)
    {
        state = state * 1103515245U + 12345U;
        largest.push(static_cast<int>(state >> 20));
        smallest.push(static_cast<int>(state >> 20));
    }
    bool ordered = true;
    for (int previous = largest.top(); !largest.empty(); largest.pop())
    {
        ordered = ordered && largest.top() <= previous;
        previous = largest.top();
    }
    for (int previous = smallest.top(); !smallest.empty(); smallest.pop())
    {
        ordered = ordered && smallest.top() >= previous;
        previous = smallest.top();
    }
    Check(ordered, "priority_queue");

    Check(std::sqrt(Opaque(16)) == 4.0 && std::sqrt(Opaque(2.25f)) == 1.5f &&
              std::pow(Opaque(2), 10) == 1024.0 && std::hypot(Opaque(2.0), 3.0, 6.0) == 7.0 &&
              std::lerp(Opaque(1.0), 3.0, 0.5) == 2.0 && std::isnan(std::sqrt(Opaque(-1.0))) &&
              std::signbit(Opaque(-0.0f)) && std::fpclassify(Opaque(1e-310)) == FP_SUBNORMAL,
          "cmath");

    struct Failure : std::exception
    {
        [[nodiscard]] const char* what() const noexcept override
        {
            return "failure";
        }
    };
    const Failure failure;
    const std::exception& base = failure;
    Check(std::strcmp(base.what(), "failure") == 0 && std::get_terminate() == nullptr &&
              std::uncaught_exceptions() == 0,
          "exception");
}

constexpr int AtomicThreads = 4;
constexpr int AtomicRounds = 5000;

struct AtomicShared
{
    std::atomic<long> counter;
    std::atomic<ThreeWords> words{ThreeWords{{0, 0, 0}}};
    std::atomic<double> real;
    std::atomic_flag lock;
    long guarded = 0;
    int plain = 0;
};

void* AtomicWorker(void* argument)
{
    auto* shared = static_cast<AtomicShared*>(argument);
    const std::atomic_ref<int> plain(shared->plain);
    for (int round = 0; round < AtomicRounds; ++round)
    {
        shared->counter.fetch_add(1, std::memory_order_relaxed);
        ++plain;
        shared->real.fetch_add(0.5);
        ThreeWords current = shared->words.load(std::memory_order_relaxed);
        ThreeWords next;
        do
        {
            next = current;
            ++next.values[0];
            next.values[2] = next.values[0] * 2;
        } while (!shared->words.compare_exchange_weak(current, next));
        while (shared->lock.test_and_set(std::memory_order_acquire))
        {
        }
        ++shared->guarded;
        shared->lock.clear(std::memory_order_release);
    }
    return nullptr;
}

void CheckAtomicsAndClocks()
{
    std::atomic<int> value(Opaque(5));
    int expected = 4;
    const bool refused = !value.compare_exchange_strong(expected, 1);
    const bool swapped = value.compare_exchange_strong(expected, 7);
    const int added = value.fetch_add(3);
    const int masked = (value &= 6);
    const int exchanged = value.exchange(9);
    const int decremented = value--;
    Check(refused && expected == 5 && swapped && added == 7 && masked == 2 && exchanged == 2 &&
              decremented == 9 && value.load(std::memory_order_acquire) == 8,
          "atomic<int> operations");

    std::atomic<signed char> wrapping(Opaque<signed char>(127));
    long items[4] = {};
    std::atomic<long*> pointer(items);
    const signed char wrapped = ++wrapping;
    long* const before_add = pointer.fetch_add(2);
    long* const incremented = ++pointer;
    Check(wrapped == -128 && before_add == items && incremented == items + 3,
          "atomic arithmetic wraps and pointers move by elements");

    std::atomic<ThreeBytes> bytes(ThreeBytes{{'a', 'b', 'c'}});
    ThreeBytes old_bytes = bytes.exchange(ThreeBytes{{'x', 'y', 'z'}});
    Check(!bytes.is_lock_free() && old_bytes.values[1] == 'b' && bytes.load().values[2] == 'z',
          "atomic of a type without a lock-free instruction");

    AtomicShared shared;
    pthread_t threads[AtomicThreads];
    for (pthread_t& thread : threads)
    {
        pthread_create(&thread, nullptr, AtomicWorker, &shared);
    }
    for (pthread_t thread : threads)
    {
        pthread_join(thread, nullptr);
    }
    const ThreeWords words = shared.words.load();
    constexpr long Total = static_cast<long>(AtomicThreads) * AtomicRounds;
    Check(shared.counter == Total && shared.plain == Total && shared.real == Total / 2.0 &&
              words.values[0] == Total && words.values[2] == 2 * Total && shared.guarded == Total,
          "atomics shared by threads");

    const std::time_t before = std::time(nullptr);
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    Check(now >= before && now <= std::time(nullptr), "system_clock follows the wall clock");

    const auto start = std::chrono::steady_clock::now();
    const timespec pause{0, 30'000'000};
    nanosleep(&pause, nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(elapsed >= std::chrono::milliseconds(20) && elapsed < std::chrono::seconds(5),
          "steady_clock measures a sleep");
}

static_assert(std::is_same_v<decltype(std::scoped_lock(std::declval<std::mutex&>(),
                                                       std::declval<std::recursive_mutex&>())),
                             std::scoped_lock<std::mutex, std::recursive_mutex>> &&
              !std::is_copy_constructible_v<std::thread> &&
              std::is_nothrow_move_assignable_v<std::jthread> &&
              std::is_nothrow_default_constructible_v<std::mutex>);

constinit std::mutex constant_mutex;
constinit std::once_flag constant_once;

std::chrono::milliseconds Since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start);
}

void CheckThreads()
{
    using namespace std::chrono_literals;

    int sum = 0;
    std::thread::id seen;
    std::thread adder(
        [&seen](std::unique_ptr<int> left, int& out, int right)
        {
            seen = std::this_thread::get_id();
            out = *left + right;
        },
        std::make_unique<int>(Opaque(40)), std::ref(sum), 2);
    const std::thread::id adder_id = adder.get_id();
    adder.join();
    Check(sum == 42 && seen == adder_id && adder_id != std::this_thread::get_id() &&
              !adder.joinable() && std::thread::hardware_concurrency() > 0 &&
              std::hash<std::thread::id>{}(seen) != std::hash<std::thread::id>{}({}),
          "thread runs a callable with its arguments and joins");

    std::stop_token kept;
    std::atomic<int> rounds;
    int callbacks = 0;
    {
        std::jthread worker(
            [&rounds](const std::stop_token& token)
            {
                while (!token.stop_requested())
                {
                    ++rounds;
                    std::this_thread::yield();
                }
            });
        kept = worker.get_stop_token();
        const std::stop_callback counted(kept, [&callbacks] { ++callbacks; });
        while (rounds == 0)
        {
            std::this_thread::yield();
        }
    }
    const std::stop_callback late(kept, [&callbacks] { callbacks += 10; });
    Check(kept.stop_requested() && callbacks == 10 && rounds > 0,
          "jthread requests stop and joins; only registered stop_callbacks run");

    constexpr int Threads = 4;
    constexpr int Rounds = 2000;
    long counter = 0;
    std::recursive_mutex recursive;
    {
        std::vector<std::jthread> threads;
        threads.reserve(Threads);
        for (int index = 0; index < Threads; ++index)
        {
            threads.emplace_back(
                [&]
                {
                    for (int round = 0; round < Rounds; ++round)
                    {
                        const std::scoped_lock lock(constant_mutex, recursive);
                        const std::lock_guard again(recursive);
                        ++counter;
                    }
                });
        }
    }
    Check(counter == Threads * Rounds, "mutex, recursive_mutex and scoped_lock under contention");

    std::timed_mutex timed;
    timed.lock();
    bool taken = true;
    std::chrono::milliseconds waited{};
    std::thread(
        [&]
        {
            const auto start = std::chrono::steady_clock::now();
            taken = timed.try_lock_for(30ms);
            waited = Since(start);
        })
        .join();
    timed.unlock();
    Check(!taken && waited >= 30ms && waited < 5s, "timed_mutex times out");

    std::atomic<int> calls;
    std::atomic<int> early;
    {
        std::vector<std::jthread> threads;
        threads.reserve(Threads);
        for (int index = 0; index < Threads; ++index)
        {
            threads.emplace_back(
                [&]
                {
                    std::call_once(constant_once,
                                   [&calls]
                                   {
                                       std::this_thread::sleep_for(10ms);
                                       ++calls;
                                   });
                    if (calls != 1)
                    {
                        ++early;
                    }
                });
        }
    }
    Check(calls == 1 && early == 0, "call_once runs once and the others wait for it");

    std::shared_mutex shared;
    long value = 0;
    std::atomic<long> odd_reads;
    {
        std::vector<std::jthread> threads;
        threads.reserve(Threads);
        for (int index = 0; index < Threads; ++index)
        {
            threads.emplace_back(
                [&, index]
                {
                    for (int round = 0; round < 500; ++round)
                    {
                        if (index == 0)
                        {
                            const std::lock_guard lock(shared);
                            ++value;
                            ++value;
                        }
                        else
                        {
                            const std::shared_lock lock(shared);
                            odd_reads += value % 2;
                        }
                    }
                });
        }
    }
    Check(value == 1000 && odd_reads == 0, "shared_mutex keeps readers from a writer");

    std::mutex mutex;
    std::condition_variable ready;
    std::deque<int> queue;
    bool done = false;
    long total = 0;
    std::thread consumer(
        [&]
        {
            std::unique_lock lock(mutex);
            while (true)
            {
                ready.wait(lock, [&] { return !queue.empty() || done; });
                if (queue.empty())
                {
                    return;
                }
                total += queue.front();
                queue.pop_front();
            }
        });
    for (int item = 1; item <= 1000; ++item)
    {
        {
            const std::lock_guard lock(mutex);
            queue.push_back(item);
        }
        ready.notify_one();
    }
    {
        const std::lock_guard lock(mutex);
        done = true;
    }
    ready.notify_all();
    consumer.join();
    std::unique_lock lock(mutex);
    const auto start = std::chrono::steady_clock::now();
    const std::cv_status status = ready.wait_for(lock, 30ms);
    const std::chrono::milliseconds timed_out = Since(start);
    lock.unlock();
    Check(total == 500500 && status == std::cv_status::timeout && timed_out >= 30ms,
          "condition_variable hands items over and times out");

    std::condition_variable_any any;
    bool woken = true;
    {
        std::jthread waiter(
            [&](std::stop_token token)
            {
                std::unique_lock held(mutex);
                woken = any.wait(held, std::move(token), [] { return false; });
            });
        std::this_thread::sleep_for(20ms);
    }
    Check(!woken, "a stop request wakes condition_variable_any");

    const auto before_sleep = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(20ms);
    std::this_thread::sleep_until(std::chrono::system_clock::now() + 10ms);
    Check(Since(before_sleep) >= 25ms, "sleep_for and sleep_until");
}

static_assert(std::binary_semaphore::max() == 1 && std::counting_semaphore<8>::max() == 8 &&
              std::latch::max() > 0 && std::barrier<>::max() > 0);

constinit std::counting_semaphore<1000> constant_semaphore(0);

void CheckWaiting()
{
    using namespace std::chrono_literals;

    constexpr int Rounds = 1000;
    std::atomic<int> turn(0);
    std::atomic_flag finished;
    std::thread partner(
        [&]
        {
            for (int round = 0; round < Rounds; ++round)
            {
                turn.wait(0);
                turn.store(0);
                turn.notify_one();
            }
            finished.test_and_set();
            finished.notify_all();
        });
    for (int round = 0; round < Rounds; ++round)
    {
        turn.store(1);
        turn.notify_one();
        turn.wait(1);
    }
    finished.wait(false);
    partner.join();
    Check(turn.load() == 0 && finished.test(), "atomic wait and notify take turns");

    std::atomic<long> taken;
    {
        std::vector<std::jthread> consumers;
        consumers.reserve(4);
        for (int index = 0; index < 4; ++index)
        {
            consumers.emplace_back(
                [&]
                {
                    for (int item = 0; item < 250; ++item)
                    {
                        constant_semaphore.acquire();
                        ++taken;
                    }
                });
        }
        for (int item = 0; item < 1000; item += 2)
        {
            constant_semaphore.release(2);
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const bool acquired = constant_semaphore.try_acquire_for(30ms);
    Check(taken == 1000 && !acquired && Since(start) >= 30ms,
          "counting_semaphore hands out items and times out");

    constexpr int Workers = 4;
    std::latch ready(Workers + 1);
    std::latch done(Workers);
    long values[Workers] = {};
    {
        std::vector<std::jthread> workers;
        workers.reserve(Workers);
        for (int index = 0; index < Workers; ++index)
        {
            workers.emplace_back(
                [&, index]
                {
                    ready.arrive_and_wait();
                    values[index] = index + 1;
                    done.count_down();
                });
        }
        ready.count_down();
        done.wait();
    }
    Check(done.try_wait() && values[0] + values[1] + values[2] + values[3] == 10,
          "latch starts and collects workers");

    int completions = 0;
    bool whole_phases = true;
    long slots[Workers] = {};
    std::barrier sync(Workers,
                      [&]() noexcept
                      {
                          ++completions;
                          for (const long slot : slots)
                          {
                              whole_phases = whole_phases && slot == completions;
                          }
                      });
    {
        std::vector<std::jthread> workers;
        workers.reserve(Workers);
        for (int index = 0; index < Workers; ++index)
        {
            workers.emplace_back(
                [&, index]
                {
                    for (int phase = 1; phase <= 100; ++phase)
                    {
                        slots[index] = phase;
                        sync.arrive_and_wait();
                    }
                });
        }
    }
    Check(completions == 100 && whole_phases, "barrier runs its completion once per phase");

    int dropped_completions = 0;
    std::barrier shrinking(Workers + 1, [&]() noexcept { ++dropped_completions; });
    {
        std::vector<std::jthread> workers;
        workers.reserve(Workers);
        for (int index = 0; index < Workers; ++index)
        {
            workers.emplace_back(
                [&, index]
                {
                    for (int phase = 0; phase < index; ++phase)
                    {
                        shrinking.arrive_and_wait();
                    }
                    shrinking.arrive_and_drop();
                });
        }
        for (int phase = 0; phase < Workers + 1; ++phase)
        {
            shrinking.wait(shrinking.arrive());
        }
    }
    Check(dropped_completions == Workers + 1, "barrier arrive_and_drop shrinks the phases");
}

struct Temperature
{
    double degrees;
};

struct Scores
{
    std::vector<int> values;
};

} // namespace

// A user type formatted with double's spec and a unit.
template <> struct std::formatter<Temperature> : std::formatter<double>
{
    template <class FormatContext> auto format(Temperature value, FormatContext& context) const
    {
        auto out = std::formatter<double>::format(value.degrees, context);
        *out++ = 'C';
        return out;
    }
};

static_assert(std::formattable<Temperature, char> && std::formattable<const char (&)[3], char> &&
              !std::formattable<Base, char> && !std::formattable<int*, char>);

// A user type that reuses range_formatter with brackets and a separator of its own.
template <> struct std::formatter<Scores>
{
    std::range_formatter<int> values;

    constexpr formatter()
    {
        values.set_separator("/");
        values.set_brackets("<", ">");
    }
    constexpr auto parse(std::format_parse_context& context)
    {
        return values.parse(context);
    }
    template <class FormatContext> auto format(const Scores& scores, FormatContext& context) const
    {
        return values.format(scores.values, context);
    }
};

static_assert(std::format_kind<std::vector<int>> == std::range_format::sequence &&
              std::format_kind<std::map<int, int>> == std::range_format::map &&
              std::format_kind<std::unordered_set<int>> == std::range_format::set &&
              std::formattable<std::pair<int, std::string>, char> &&
              std::formattable<std::thread::id, char> &&
              !std::formattable<std::vector<Base>, char>);

namespace
{

void CheckFormatting()
{
    Check(std::format("{} {:x} {:#b} {:+05} {:*^7}", 42, 255, 5, 7, -3) ==
              "42 ff 0b101 +0007 **-3***",
          "format of integers");
    Check(std::format("{} {:.3f} {:e} {:g} {:a} {}", 0.1, 2.0 / 3, 1e-7, 1e20, 1.0, 0.1F) ==
              "0.1 0.667 1.000000e-07 1e+20 1p+0 0.1",
          "format of double and float");
    Check(std::format("{} {:.2e} {:+08.3f}", 2.5L, 0.125L, -0.5L) == "2.5 1.25e-01 -000.500",
          "format of long double");
    Check(std::format("[{:<6}][{:>6.3}][{:^7}]", "ab", std::string("abcdef"),
                      std::string_view("\u4e2d\u6587")) == "[ab    ][   abc][ \u4e2d\u6587  ]",
          "format of strings counts wide columns");
    Check(std::format("{} {:d} {:c} {:x} {} {}", true, false, 'A', 'A', nullptr,
                      static_cast<void*>(nullptr)) == "true 0 A 41 0x0 0x0",
          "format of bool, char and pointers");
    Check(std::format("{1}{0}{{}}{2:>{3}}", 'a', 'b', 7, 4) == "ba{}   7",
          "format with manual indices, escapes and a width argument");
    Check(std::format("{:.1f}|{:>7.2f}", Temperature{21.5}, Temperature{-3}) == "21.5C|  -3.00C",
          "format of a user type");

    char buffer[16] = {};
    *std::format_to(buffer, "{}-{}", 1, "x") = '\0';
    std::string text;
    std::format_to(std::back_inserter(text), "{:03}{}", 7, std::string(300, 'z'));
    const auto limited = std::format_to_n(buffer, 3, "{}", 123456);
    Check(std::string_view(buffer, 3) == "123" && limited.size == 6 && limited.out == buffer + 3 &&
              text.size() == 303 && text.starts_with("007z") &&
              std::formatted_size("{:>20}", 1) == 20,
          "format_to, format_to_n and formatted_size");

    const char* path = "/tmp/cxxtest-print";
    std::FILE* file = std::fopen(path, "w");
    std::print(file, "{}+{}=", 2, 2);
    std::println(file, "{:>3}", 4);
    std::fclose(file);
    file = std::fopen(path, "r");
    char line[32] = {};
    const bool read = file != nullptr && std::fgets(line, sizeof(line), file) != nullptr;
    if (file != nullptr)
    {
        std::fclose(file);
    }
    std::remove(path);
    Check(read && std::string_view(line) == "2+2=  4\n", "print and println to a file");
}

void CheckRangeFormatting()
{
    const std::vector<int> numbers = {1, -2, 300};
    const std::deque<std::vector<int>> nested = {{1}, {}, {2, 3}};
    Check(std::format("{} {:n} {::#x} {:*^14} {}", numbers, numbers, std::array<int, 2>{10, 11},
                      numbers, nested) ==
              "[1, -2, 300] 1, -2, 300 [0xa, 0xb] *[1, -2, 300]* [[1], [], [2, 3]]",
          "format of sequences");
    const std::map<std::string, int> ages = {{"ann", 3}, {"bo", 41}};
    const std::set<char> letters = {'b', 'a'};
    const std::vector<std::pair<int, int>> pairs = {{1, 2}, {3, 4}};
    Check(std::format("{} {:n} {} {::d} {:m}", ages, ages, letters, letters, pairs) ==
              "{\"ann\": 3, \"bo\": 41} \"ann\": 3, \"bo\": 41 {'a', 'b'} {97, 98} {1: 2, 3: 4}",
          "format of maps and sets");
    Check(std::format("{} {:m} {:n} [{:>10}] {}", std::pair<int, std::string>(1, "x"),
                      std::pair<char, double>('k', 2.5), std::tuple<int, bool, char>(7, true, 'c'),
                      std::tuple<int, int>(1, 2),
                      std::tuple<>()) == "(1, \"x\") 'k': 2.5 7, true, 'c' [    (1, 2)] ()",
          "format of pairs and tuples");
    Check(std::format("{:?} {:?} {:?} [{:>8?}] {:.3?}", "tab\there \"q\"\n", '\'', "\x01\u00a0",
                      'x', std::string("a\nb")) ==
              "\"tab\\there \\\"q\\\"\\n\" '\\'' \"\\u{1}\\u{a0}\" [     'x'] \"a\\",
          "format of debug strings and characters");
    const std::vector<char> characters = {'h', 'i', '\n'};
    std::vector<bool> bits = {true, false};
    Check(std::format("{} {:s} {:?s} {} {:d}", characters, characters, characters, bits, bits[0]) ==
              "['h', 'i', '\\n'] hi\n \"hi\\n\" [true, false] 1",
          "format of character ranges and vector<bool>");
    const std::thread::id none;
    const std::string current = std::format("{}", std::this_thread::get_id());
    Check(std::format("{} [{:>3}] [{:*<2}]", none, none, none) == "0 [  0] [0*]" &&
              !current.empty() && current != "0" &&
              current.find_first_not_of("0123456789") == std::string::npos,
          "format of thread::id");
    Check(std::format("{} {::+} [{:>12}]", Scores{{1, 2, 3}}, Scores{{4}}, Scores{{5, 6}}) ==
              "<1/2/3> <+4> [       <5/6>]",
          "range_formatter in a user formatter");
}

static_assert(std::chrono::sys_days(std::chrono::year(2024) / std::chrono::February / 29)
                      .time_since_epoch()
                      .count() == 19782 &&
              std::chrono::hh_mm_ss<std::chrono::milliseconds>::fractional_width == 3 &&
              std::formattable<std::chrono::sys_seconds, char> &&
              std::formattable<std::chrono::year_month_day, char>);

void CheckChronoFormatting()
{
    using namespace std::chrono;
    using namespace std::chrono_literals;
    Check(std::format("{} {} {} {:%H:%M:%S} {:%Q%q} {:%j %T}", 42s, -1500ms, 90min, 3725s, 5ms,
                      -(50h + 1s)) == "42s -1500ms 90min 01:02:05 5ms -2 02:00:01",
          "format of durations");
    Check(std::format("{} {:.2} {:%T} {}", duration<double>(1.5),
                      duration<double, std::milli>(2.0 / 3), 1234567us,
                      duration<int, std::ratio<2, 3>>(5)) == "1.5s 0.67ms 00:00:01.234567 5[2/3]s",
          "format of floating-point durations and odd periods");
    const sys_time<milliseconds> leap_day = sys_days(2024y / February / 29) + 13h + 5min + 9s + 7ms;
    Check(std::format("{} | {:%a %d %b %Y %R %Z} | {:%G-W%V-%u %j} | {}", leap_day, leap_day,
                      leap_day, floor<days>(leap_day)) ==
              "2024-02-29 13:05:09.007 | Thu 29 Feb 2024 13:05 UTC | 2024-W09-4 060 | 2024-02-29",
          "format of time points");
    const year_month_day date = sys_days(days(-1));
    Check(date == 1969y / December / 31 && weekday(sys_days(date)) == Wednesday &&
              !(2023y / February / 29).ok() &&
              (2024y / January / 31 + months(1)).month() == February &&
              sys_days(2023y / February / 30) == sys_days(2023y / March / 2) &&
              December + months(2) == February && Sunday - Monday == days(6),
          "calendar arithmetic and conversions");
    Check(std::format("{} {} {} {} {} {} {}", 2024y / 3 / 9, day(5), month(13), March, year(-5),
                      Sunday, 2023y / 2 / 29) ==
              "2024-03-09 05 13 is not a valid month Mar -0005 Sun 2023-02-29 is not a valid date",
          "format of calendar types");
    Check(std::format("{} {:%I:%M %p}", hh_mm_ss(-(13h + 5min + 1s + 250ms)),
                      hh_mm_ss(13h + 5min)) == "-13:05:01.250 01:05 PM",
          "format of hh_mm_ss");
    Check(std::format("[{:>12}] [{:*<10%R}] [{:^{}}]", 5s, 2h + 3min, Monday, 5) ==
              "[          5s] [02:03*****] [ Mon ]",
          "format of chrono types with fills and widths");
    const system_clock::time_point now = system_clock::now();
    const time_t now_seconds = system_clock::to_time_t(now);
    char expected[32] = {};
    std::strftime(expected, sizeof(expected), "%Y-%m-%d %H:%M:%S %a %j", std::gmtime(&now_seconds));
    Check(std::format("{:%F %T %a %j}", floor<std::chrono::seconds>(now)) == expected,
          "format of system_clock::now agrees with strftime");
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc != 1)
    {
        fprintf(stderr, "%s\n", Help);
        return 1;
    }
    CheckLanguageSupport();
    CheckUtility();
    CheckOrdering();
    CheckLimitsAndBits();
    CheckCLibrary();
    CheckAdaptorsAndMath();
    CheckVocabulary();
    CheckSumTypes();
    CheckOwnership();
    CheckCallables();
    CheckAlgorithms();
    CheckContainers();
    CheckAssociative();
    CheckStrings();
    CheckAtomicsAndClocks();
    CheckThreads();
    CheckWaiting();
    CheckFormatting();
    CheckRangeFormatting();
    CheckChronoFormatting();
    if (failed)
    {
        return 1;
    }
    printf("cxxtest: %llu checks passed\n", static_cast<unsigned long long>(passed_checks));
    return 0;
}
