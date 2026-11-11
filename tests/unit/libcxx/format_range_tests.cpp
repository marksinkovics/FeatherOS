#include <catch2/catch_test_macros.hpp>

#include <array>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libcxx/include/array"
#include "libcxx/include/deque"
#include "libcxx/include/format"
#include "libcxx/include/map"
#include "libcxx/include/set"
#include "libcxx/include/string"
#include "libcxx/include/thread"
#include "libcxx/include/tuple"
#include "libcxx/include/unordered_map"
#include "libcxx/include/unordered_set"
#include "libcxx/include/vector"

namespace
{

namespace fstd = featheros_std;

template <class... Args> std::string Ours(fstd::format_string<Args...> pattern, Args&&... args)
{
    const fstd::string text = fstd::format(pattern, std::forward<Args>(args)...);
    return {text.data(), text.size()};
}

// Each pattern formats ours with our library and host with the host's, which is the reference.
template <class Ours, class Host>
void CheckPatterns(std::initializer_list<const char*> patterns, const Ours& ours, const Host& host)
{
    for (const char* pattern : patterns)
    {
        INFO(pattern);
        const fstd::string text = fstd::vformat(pattern, fstd::make_format_args(ours));
        REQUIRE(std::string(text.data(), text.size()) ==
                std::vformat(pattern, std::make_format_args(host)));
    }
}

template <size_t Size> struct Pattern
{
    char text[Size];
    constexpr Pattern(const char (&value)[Size])
    {
        for (size_t index = 0; index != Size; ++index)
        {
            text[index] = value[index];
        }
    }
    constexpr fstd::string_view View() const
    {
        return {text, Size - 1};
    }
};

template <Pattern Text, class... Args>
constexpr bool Accepted = requires {
    typename std::integral_constant<
        int, (fstd::__detail::CheckFormatString<Args...>(Text.View()), 0)>;
};

struct Opaque
{
};

struct Cell
{
    int value;
};

struct Row
{
    fstd::vector<int> cells;
};

struct HostRow
{
    std::vector<int> cells;
};

// A range of characters that its format_kind marks as a string or a debug string.
template <bool Debug> struct Word
{
    const char* text;
    const char* begin() const
    {
        return text;
    }
    const char* end() const
    {
        return text + std::char_traits<char>::length(text);
    }
};

// Iterable only when not const, so it is formatted through a non-const reference.
struct Countdown
{
    int values[3] = {3, 2, 1};
    int* begin()
    {
        return values;
    }
    int* end()
    {
        return values + 3;
    }
};

} // namespace

template <> struct featheros_std::formatter<Cell, char> : featheros_std::formatter<int, char>
{
    template <class FormatContext> auto format(Cell cell, FormatContext& context) const
    {
        auto out = context.out();
        *out++ = '#';
        context.advance_to(out);
        return featheros_std::formatter<int, char>::format(cell.value, context);
    }
};

template <> struct featheros_std::formatter<Row, char>
{
    featheros_std::range_formatter<int> cells;

    constexpr formatter()
    {
        cells.set_separator(" | ");
        cells.set_brackets("<", ">");
    }
    constexpr auto parse(featheros_std::format_parse_context& context)
    {
        return cells.parse(context);
    }
    template <class FormatContext> auto format(const Row& row, FormatContext& context) const
    {
        return cells.format(row.cells, context);
    }
};

template <> struct std::formatter<HostRow, char>
{
    std::range_formatter<int> cells;

    constexpr formatter()
    {
        cells.set_separator(" | ");
        cells.set_brackets("<", ">");
    }
    constexpr auto parse(std::format_parse_context& context)
    {
        return cells.parse(context);
    }
    template <class FormatContext> auto format(const HostRow& row, FormatContext& context) const
    {
        return cells.format(row.cells, context);
    }
};

template <>
constexpr featheros_std::range_format featheros_std::format_kind<Word<false>> =
    featheros_std::range_format::string;
template <>
constexpr featheros_std::range_format featheros_std::format_kind<Word<true>> =
    featheros_std::range_format::debug_string;

static_assert(fstd::format_kind<fstd::vector<int>> == fstd::range_format::sequence &&
              fstd::format_kind<fstd::deque<char>> == fstd::range_format::sequence &&
              fstd::format_kind<fstd::array<int, 2>> == fstd::range_format::sequence &&
              fstd::format_kind<fstd::map<int, int>> == fstd::range_format::map &&
              fstd::format_kind<fstd::multimap<int, int>> == fstd::range_format::map &&
              fstd::format_kind<fstd::unordered_map<int, int>> == fstd::range_format::map &&
              fstd::format_kind<fstd::set<int>> == fstd::range_format::set &&
              fstd::format_kind<fstd::unordered_set<int>> == fstd::range_format::set &&
              fstd::format_kind<fstd::string> == fstd::range_format::sequence);
static_assert(fstd::formattable<fstd::vector<int>, char> &&
              fstd::formattable<fstd::map<fstd::string, fstd::vector<int>>, char> &&
              fstd::formattable<fstd::pair<int, fstd::string>, char> &&
              fstd::formattable<fstd::tuple<>, char> &&
              fstd::formattable<fstd::vector<bool>::reference, char> &&
              fstd::formattable<fstd::thread::id, char> && fstd::formattable<Countdown, char> &&
              fstd::formattable<Word<true>, char>);
static_assert(!fstd::formattable<fstd::vector<Opaque>, char> &&
              !fstd::formattable<fstd::pair<int, Opaque>, char> &&
              !fstd::formattable<fstd::tuple<Opaque>, char>);

static_assert(Accepted<"{::#x}", fstd::vector<int>> && Accepted<"{:n:02}", fstd::vector<int>> &&
              Accepted<"{:*^{}}", fstd::vector<int>, int> &&
              Accepted<"{:m}", fstd::vector<fstd::pair<int, int>>> &&
              Accepted<"{:s}", fstd::vector<char>> && Accepted<"{:>9?s}", fstd::vector<char>> &&
              Accepted<"{:::<4}", fstd::vector<fstd::vector<int>>> &&
              Accepted<"{::m}", fstd::map<int, int>> && Accepted<"{:m}", fstd::pair<int, int>> &&
              Accepted<"{:n}", fstd::tuple<int, char, int>> && Accepted<"{:?}", fstd::string> &&
              Accepted<"{:>5?}", char> && Accepted<"{:.3?}", const char*> &&
              Accepted<"{:*<9}", fstd::thread::id> && Accepted<"{::p>4}", fstd::vector<Cell>>);
static_assert(!Accepted<"{:s}", fstd::vector<int>> && !Accepted<"{:?}", fstd::vector<char>> &&
              !Accepted<"{:m}", fstd::vector<int>> && !Accepted<"{:ns}", fstd::vector<char>> &&
              !Accepted<"{:s:c}", fstd::vector<char>> &&
              !Accepted<"{:m}", fstd::tuple<int, int, int>> &&
              !Accepted<"{:+}", fstd::vector<int>> && !Accepted<"{:.3}", fstd::vector<int>> &&
              !Accepted<"{:05}", fstd::vector<int>> &&
              !Accepted<"{:n:+}", fstd::vector<fstd::string>> && !Accepted<"{:?}", int> &&
              !Accepted<"{:.2}", fstd::pair<int, int>> && !Accepted<"{:s}", fstd::pair<int, int>> &&
              !Accepted<"{:x}", fstd::thread::id> && !Accepted<"{:+?}", char> &&
              !Accepted<"{:{}}", fstd::vector<int>, double>);

TEST_CASE("format of sequences matches the host", "[libcxx][format]")
{
    const auto integer_patterns = {"{}",        "{:n}",    "{::#x}",   "{:*^15}", "{:n:02}",
                                   "{:>12}",    "{:<12}",  "{::+}",    "{:n:*^4}", "{:🙂>9}",
                                   "{:^16:>3}", "{::b}"};
    CheckPatterns(integer_patterns, fstd::vector<int>{}, std::vector<int>{});
    CheckPatterns(integer_patterns, fstd::vector<int>{7}, std::vector<int>{7});
    CheckPatterns(integer_patterns, fstd::vector<int>{1, -2, 300}, std::vector<int>{1, -2, 300});
    CheckPatterns(integer_patterns, fstd::deque<int>{4, 5}, std::deque<int>{4, 5});
    CheckPatterns(integer_patterns, fstd::array<long long, 2>{-1, 1LL << 40},
                  std::array<long long, 2>{-1, 1LL << 40});
    CheckPatterns({"{}", "{:n}", "{:::<4}", "{:>20}", "{::n}"},
                  fstd::vector<fstd::vector<int>>{{1}, {}, {2, 3}},
                  std::vector<std::vector<int>>{{1}, {}, {2, 3}});
    CheckPatterns({"{}", "{::}", "{::>5}", "{:n:.1}", "{::?}", "{:>14}"},
                  fstd::vector<fstd::string>{"a\tb", "c", "中"},
                  std::vector<std::string>{"a\tb", "c", "中"});
    CheckPatterns({"{}", "{::}"}, fstd::vector<const char*>{"x", "y\n"},
                  std::vector<const char*>{"x", "y\n"});
    CheckPatterns({"{}", "{:s}", "{:?s}", "{:>8s}", "{:*<9?s}", "{::d}", "{:n}", "{::c}"},
                  fstd::vector<char>{'h', 'i', '\n'}, std::vector<char>{'h', 'i', '\n'});
    CheckPatterns({"{}", "{:s}"}, fstd::deque<char>{'a', '"'}, std::deque<char>{'a', '"'});
    CheckPatterns({"{}", "{::.2f}", "{::e}"}, fstd::vector<double>{0.5, -1e10},
                  std::vector<double>{0.5, -1e10});
    CheckPatterns({"{}", "{::d}", "{:n:>6}"}, fstd::vector<bool>{true, false, true},
                  std::vector<bool>{true, false, true});

    fstd::vector<bool> bits = {false, true};
    std::vector<bool> host_bits = {false, true};
    CheckPatterns({"{}", "{:d}", "{:>6}"}, bits[1], host_bits[1]);
    REQUIRE(Ours("{} {}", bits[0], bits) == "false [false, true]");

    const fstd::vector<int> dynamic = {1, 2};
    REQUIRE(Ours("[{:>{}}] [{:{}:>{}}]", dynamic, 10, dynamic, 10, 3) ==
            std::format("[{:>{}}] [{:{}:>{}}]", std::vector<int>{1, 2}, 10,
                        std::vector<int>{1, 2}, 10, 3));
}

TEST_CASE("format of maps and sets matches the host", "[libcxx][format]")
{
    const auto patterns = {"{}", "{:n}", "{::}", "{:>30}", "{:*^28}"};
    CheckPatterns(patterns, fstd::map<fstd::string, int>{{"a", 1}, {"b", 2}},
                  std::map<std::string, int>{{"a", 1}, {"b", 2}});
    CheckPatterns(patterns, fstd::multimap<int, char>{{1, 'x'}, {1, 'y'}},
                  std::multimap<int, char>{{1, 'x'}, {1, 'y'}});
    CheckPatterns(patterns, fstd::map<int, fstd::vector<int>>{{1, {2, 3}}},
                  std::map<int, std::vector<int>>{{1, {2, 3}}});
    CheckPatterns(patterns, fstd::map<int, int>{}, std::map<int, int>{});
    CheckPatterns(patterns, fstd::unordered_map<int, int>{{4, 5}},
                  std::unordered_map<int, int>{{4, 5}});
    CheckPatterns({"{}", "{:n}", "{::d}", "{:>12}"}, fstd::set<char>{'a', 'b'},
                  std::set<char>{'a', 'b'});
    CheckPatterns({"{}", "{::x}"}, fstd::set<int>{3, 1, 2}, std::set<int>{3, 1, 2});
    CheckPatterns({"{}", "{:m}", "{:n}", "{:nm}"},
                  fstd::vector<fstd::pair<int, fstd::string>>{{1, "one"}, {2, "two"}},
                  std::vector<std::pair<int, std::string>>{{1, "one"}, {2, "two"}});
    CheckPatterns({"{}", "{:m}"}, fstd::vector<fstd::tuple<char, int>>{{'k', 9}},
                  std::vector<std::tuple<char, int>>{{'k', 9}});
}

TEST_CASE("format of pairs and tuples matches the host", "[libcxx][format]")
{
    const auto pair_patterns = {"{}", "{:m}", "{:n}", "{:>12}", "{:*<14m}", "{:^5}"};
    CheckPatterns(pair_patterns, fstd::pair<int, fstd::string>(1, "x"),
                  std::pair<int, std::string>(1, "x"));
    CheckPatterns(pair_patterns, fstd::pair<char, double>('a', 2.5),
                  std::pair<char, double>('a', 2.5));
    CheckPatterns(pair_patterns, fstd::tuple<int, int>(1, 2), std::tuple<int, int>(1, 2));
    CheckPatterns(pair_patterns, fstd::pair<fstd::vector<int>, fstd::pair<int, int>>({1}, {2, 3}),
                  std::pair<std::vector<int>, std::pair<int, int>>({1}, {2, 3}));
    const auto tuple_patterns = {"{}", "{:n}", "{:>16}", "{:_^20n}"};
    CheckPatterns(tuple_patterns, fstd::tuple<>(), std::tuple<>());
    CheckPatterns(tuple_patterns, fstd::tuple<int>(5), std::tuple<int>(5));
    CheckPatterns(tuple_patterns, fstd::tuple<int, const char*, char>(1, "s", '\''),
                  std::tuple<int, const char*, char>(1, "s", '\''));
    CheckPatterns(tuple_patterns, fstd::tuple<bool, fstd::string_view, double>(true, "\x01", 1e-3),
                  std::tuple<bool, std::string_view, double>(true, "\x01", 1e-3));

    int value = 7;
    REQUIRE(Ours("{}", fstd::tuple<int&, const int&>(value, value)) == "(7, 7)");
    REQUIRE(Ours("[{:>{}}]", fstd::pair<int, int>(1, 2), 8) == "[  (1, 2)]");
}

TEST_CASE("debug strings and characters are escaped as the host escapes them",
          "[libcxx][format]")
{
    const char* const strings[] = {"plain",
                                   "",
                                   "\t\n\r\\\"'",
                                   "\x01\x1f\x7f",
                                   "\u0080\u009f\u00a0\u00ad",
                                   "\u200b\u200d\u2028\u2029\u202e\u2060\ufeff",
                                   "\u3000\u1680 x",
                                   "\u0300a\u0301\u0302",
                                   "\t\u0301",
                                   "\u05d0\u05b8",
                                   "é中文😀",
                                   "\xc3",
                                   "\xff\xfe",
                                   "a\xe4\xb8z",
                                   "\xed\xa0\x80",
                                   "\xf4\x90\x80\x80",
                                   "\ue000\U000F0000\U0010FFFD",
                                   "\ufffe\uffff\ufdd0",
                                   "\U000E0001\U000E0100"};
    for (const char* text : strings)
    {
        INFO(text);
        CheckPatterns({"{:?}", "{:>12?}", "{:.3?}", "{:*^14.5?}"}, text, text);
        CheckPatterns({"{:?}"}, fstd::string(text), std::string(text));
        CheckPatterns({"{:?}"}, fstd::string_view(text), std::string_view(text));
    }
    for (int code = 0; code != 256; ++code)
    {
        const auto character = static_cast<char>(code);
        INFO(code);
        CheckPatterns({"{:?}", "{:>6?}", "{:^7?}"}, character, character);
    }
    char array[] = "a\"b";
    REQUIRE(Ours("{:?}", array) == R"("a\"b")");
    // Unassigned code points outside EscapedRanges stay as they are; libc++ escapes them.
    REQUIRE(Ours("{:?}", "\u0378") == "\"\u0378\"");
    REQUIRE(std::format("{:?}", "\u0378") == "\"\\u{378}\"");
}

TEST_CASE("range_formatter and format_kind serve user types", "[libcxx][format]")
{
    const Row row{{1, 2, 3}};
    const HostRow host_row{{1, 2, 3}};
    CheckPatterns({"{}", "{:n}", "{::x}", "{:>16}"}, row, host_row);
    REQUIRE(Ours("{} {:?}", Word<false>{"a\tb"}, Word<false>{"c"}) == "a\tb \"c\"");
    REQUIRE(Ours("{} [{:>6}] {:.2}", Word<true>{"a\tb"}, Word<true>{"x"}, Word<true>{"yz"}) ==
            "\"a\\tb\" [   \"x\"] \"y");
    Countdown countdown;
    REQUIRE(Ours("{} {:n:+}", countdown, countdown) == "[3, 2, 1] +3, +2, +1");
    REQUIRE(Ours("{::p>4}", fstd::vector<Cell>{{1}, {-2}}) == "[#ppp1, #pp-2]");
}

TEST_CASE("format of thread::id writes the thread's number", "[libcxx][format]")
{
    REQUIRE(Ours("{}", fstd::thread::id()) == "0");
    REQUIRE(Ours("[{:>4}] [{:*<3}] [{:^5}]", fstd::thread::id(), fstd::thread::id(),
                 fstd::thread::id()) == "[   0] [0**] [  0  ]");
    const fstd::thread::id current = fstd::this_thread::get_id();
    const size_t key = fstd::hash<fstd::thread::id>()(current);
    REQUIRE(Ours("{}", current) == std::format("{}", key));
    REQUIRE(Ours("{:>30}", current) == std::format("{:>30}", key));
}
