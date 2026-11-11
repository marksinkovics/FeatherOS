#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <string>
#include <type_traits>

#include "libcxx/include/format"
#include "libcxx/include/print"

namespace
{

namespace fstd = featheros_std;

template <class... Args> std::string Ours(fstd::format_string<Args...> pattern, Args&&... args)
{
    const fstd::string text = fstd::format(pattern, std::forward<Args>(args)...);
    return {text.data(), text.size()};
}

// The host's std::format is the reference: each pattern runs through both.
#define CHECK_SAME(pattern, ...) REQUIRE(Ours(pattern, __VA_ARGS__) == std::format(pattern, __VA_ARGS__))

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

// Whether the compile-time check accepts a pattern for these argument types.
template <Pattern Text, class... Args>
constexpr bool Accepted = requires {
    typename std::integral_constant<
        int, (fstd::__detail::CheckFormatString<Args...>(Text.View()), 0)>;
};

struct Point
{
    int x;
    int y;
};

struct Opaque
{
};

} // namespace

// A user type: the spec applies to both coordinates, and "p" asks for a pair of parentheses.
template <> struct featheros_std::formatter<Point, char> : featheros_std::formatter<int, char>
{
    bool parentheses = false;

    constexpr auto parse(featheros_std::format_parse_context& context)
    {
        auto cursor = context.begin();
        if (cursor != context.end() && *cursor == 'p')
        {
            parentheses = true;
            context.advance_to(++cursor);
        }
        return featheros_std::formatter<int, char>::parse(context);
    }

    template <class FormatContext> auto format(const Point& point, FormatContext& context) const
    {
        auto out = context.out();
        if (parentheses)
        {
            *out++ = '(';
        }
        context.advance_to(out);
        out = featheros_std::formatter<int, char>::format(point.x, context);
        out = featheros_std::format_to(out, ", ");
        context.advance_to(out);
        out = featheros_std::formatter<int, char>::format(point.y, context);
        if (parentheses)
        {
            *out++ = ')';
        }
        return out;
    }
};

static_assert(fstd::formattable<int, char> && fstd::formattable<const char*, char> &&
              fstd::formattable<fstd::string, char> && fstd::formattable<Point, char> &&
              fstd::formattable<double&, char> && fstd::formattable<char[4], char>);
static_assert(!fstd::formattable<Opaque, char> && !fstd::formattable<int*, char> &&
              !fstd::formattable<wchar_t, char> && !fstd::formattable<char8_t, char>);

static_assert(Accepted<"{} {}", int, int> && Accepted<"{1} {0} {1}", int, int> &&
              Accepted<"{{}}", int> && Accepted<"{:*^+#012}", int> &&
              Accepted<"{:{}.{}}", double, int, unsigned> && Accepted<"{0:{1}}", char, short> &&
              Accepted<"{:é<5}", int> && Accepted<"{:.3s}", const char*> &&
              Accepted<"{:x}", char> && Accepted<"{:#o}", bool> && Accepted<"{:p}", void*> &&
              Accepted<"{:08P}", nullptr_t> && Accepted<"{:L}", double>);
static_assert(!Accepted<"{} {}", int> && !Accepted<"{0} {}", int, int> &&
              !Accepted<"{} {0}", int, int> && !Accepted<"{", int> && !Accepted<"}", int> &&
              !Accepted<"{:d}", const char*> && !Accepted<"{:.2}", int> &&
              !Accepted<"{:+}", const char*> && !Accepted<"{:#}", char> &&
              !Accepted<"{:{}}", int, double> && !Accepted<"{:{}}", int, bool> &&
              !Accepted<"{:{<5}", int> && !Accepted<"{:L}", const char*> &&
              !Accepted<"{:00}", int> && !Accepted<"{:5", int> && !Accepted<"{01}", int, int> &&
              !Accepted<"{:+c}", int> && !Accepted<"{:#p}", void*> && !Accepted<"{x}", int> &&
              !Accepted<"{:\xC3<5}", int> && !Accepted<"{:2147483648}", int>);

TEST_CASE("format writes text, escapes and argument indices", "[libcxx][format]")
{
    CHECK_SAME("plain text", 0);
    CHECK_SAME("{{}} {{{}}}", 5);
    CHECK_SAME("{1}-{0}-{1}", 'a', "b");
    CHECK_SAME("{}{}{}", 1, 2u, 3LL);
    REQUIRE(Ours("") == "");
    REQUIRE(Ours("{} {}", Point{1, 2}, Point{3, 4}) == "1, 2 3, 4");
    REQUIRE(Ours("[{:p>3}]", Point{1, -2}) == "[(  1,  -2)]");
    REQUIRE(Ours("{:p#x}", Point{255, 16}) == "(0xff, 0x10)");
}

TEST_CASE("format of integers follows the standard spec", "[libcxx][format]")
{
    const char* const patterns[] = {"{}",      "{:d}",    "{:b}",     "{:#B}",   "{:o}",
                                    "{:#o}",   "{:x}",    "{:#X}",    "{:+}",    "{: }",
                                    "{:-}",    "{:08}",   "{:+#010x}", "{:<8}",  "{:>8}",
                                    "{:^9}",   "{:*^9}",  "{:_<+#8b}", "{:#06}", "{:<06}",
                                    "{:L}",    "{:3}",    "{:#}"};
    const long long values[] = {0,     1,         -1,          42,
                                -42,   255,       -255,        65536,
                                1234567890, -2147483648LL, 9223372036854775807LL,
                                std::numeric_limits<long long>::min()};
    for (const char* pattern : patterns)
    {
        for (const long long value : values)
        {
            // libc++ keeps one zero too few for # and g below 1 ("0.10000"); printf and
            // libstdc++ print "0.100000", as do we (checked below).
            if (std::string(pattern).find("#") != std::string::npos &&
                std::string(pattern).find_first_of("gG") != std::string::npos &&
                std::fabs(value) < 1 && value != 0)
            {
                continue;
            }
            INFO(pattern << " with " << value);
            const auto ours = fstd::vformat(pattern, fstd::make_format_args(value));
            REQUIRE(std::string(ours.data(), ours.size()) ==
                    std::vformat(pattern, std::make_format_args(value)));
            const auto narrow = static_cast<int>(value);
            const auto ours_int = fstd::vformat(pattern, fstd::make_format_args(narrow));
            REQUIRE(std::string(ours_int.data(), ours_int.size()) ==
                    std::vformat(pattern, std::make_format_args(narrow)));
            const auto wide = static_cast<unsigned long long>(value);
            const auto ours_unsigned = fstd::vformat(pattern, fstd::make_format_args(wide));
            REQUIRE(std::string(ours_unsigned.data(), ours_unsigned.size()) ==
                    std::vformat(pattern, std::make_format_args(wide)));
        }
    }
    const signed char small = -128;
    const unsigned short medium = 65535;
    const long large = -7;
    CHECK_SAME("{} {} {}", small, medium, large);
    CHECK_SAME("{:c}{:c}", 65, 'z');
    CHECK_SAME("{:#x} {:d} {:b}", 'a', 'a', 'a');
    CHECK_SAME("{:#x}", static_cast<char>(-1));
}

TEST_CASE("format of bool, char and pointers", "[libcxx][format]")
{
    CHECK_SAME("{} {}", true, false);
    CHECK_SAME("{:>6}|{:^7}|{:s}", true, false, true);
    CHECK_SAME("{:d} {:#x} {:+d} {:06b}", true, true, false, true);
    CHECK_SAME("[{}] [{:3}] [{:>3}] [{:^5}] [{:c}]", 'x', 'y', 'z', 'w', 'v');
    CHECK_SAME("{} {:>6} {:08}", nullptr, nullptr, nullptr);
    int target = 0;
    void* pointer = &target;
    const void* constant = &target;
    CHECK_SAME("{} {:p} {:>20} {:020}", pointer, constant, pointer, pointer);
    CHECK_SAME("{:P} {:018P}", pointer, constant);
}

TEST_CASE("format of strings counts columns and truncates by precision", "[libcxx][format]")
{
    const char* const host = "hello";
    const fstd::string ours = "hello";
    const fstd::string_view view = "view";
    const char array[] = "arr";
    const char* pointer = "pointer";
    char buffer[8] = {'a', 'b', '\0', 'c'};
    REQUIRE(Ours("{} {} {} {} {}", ours, view, array, pointer, buffer) ==
            "hello view arr pointer ab");
    CHECK_SAME("[{:8}] [{:>8}] [{:^8}] [{:.3}] [{:*^7.2}] [{:.0}]", host, host, host, host, host,
               host);
    CHECK_SAME("[{:{}.{}}] [{:>{}}]", "dynamic", 9, 3, "x", 4);
    CHECK_SAME("[{:6}] [{:>6}] [{:.2}] [{:.1}]", "中文", "中文", "中文", "中文");
    CHECK_SAME("[{:5}] [{:.3}]", "café", "café");
    CHECK_SAME("[{:🙂^7}] [{:é>4}]", "ab", 1);
    CHECK_SAME("[{:4}] [{:.1}]", "é", "éx");
    CHECK_SAME("[{:3}]", "\xFF\xFE");
}

TEST_CASE("format of floating point matches the host", "[libcxx][format]")
{
    const char* const patterns[] = {
        "{}",      "{:e}",    "{:E}",     "{:f}",    "{:F}",      "{:g}",     "{:G}",
        "{:a}",    "{:A}",    "{:.0}",    "{:.3}",   "{:.17}",    "{:.0e}",   "{:.10e}",
        "{:.0f}",  "{:.12f}", "{:.0g}",   "{:.12g}", "{:.3a}",    "{:.0a}",   "{:#}",
        "{:#.3}",  "{:#.0f}", "{:#.0e}",  "{:#g}",   "{:#.3g}",   "{:#a}",    "{:+}",
        "{: }",    "{:+.2e}", "{:012}",   "{:+012.3f}", "{:<12}",  "{:^12g}",  "{:*>15e}",
        "{:#012g}", "{:L}",   "{:.40f}",  "{:.60e}"};
    const double values[] = {0.0,
                             -0.0,
                             1.0,
                             -1.5,
                             0.1,
                             1e-5,
                             123456.789,
                             1e15,
                             1e16,
                             1e22,
                             1e300,
                             5e-324,
                             2.2250738585072014e-308,
                             std::numeric_limits<double>::max(),
                             0.000123,
                             100.0,
                             9.9999999,
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(),
                             -std::numeric_limits<double>::quiet_NaN()};
    for (const char* pattern : patterns)
    {
        // Our hex form normalizes subnormals (1p-1074), libc++ does not; both read back.
        const bool hex = std::string(pattern).find_first_of("aA") != std::string::npos;
        for (const double value : values)
        {
            if (hex && (std::fpclassify(value) == FP_SUBNORMAL ||
                        std::fpclassify(static_cast<float>(value)) == FP_SUBNORMAL))
            {
                continue;
            }
            // libc++ keeps one zero too few for # and g below 1 ("0.10000"); printf and
            // libstdc++ print "0.100000", as do we (checked below).
            if (std::string(pattern).find("#") != std::string::npos &&
                std::string(pattern).find_first_of("gG") != std::string::npos &&
                std::fabs(value) < 1 && value != 0)
            {
                continue;
            }
            INFO(pattern << " with " << value);
            const auto ours = fstd::vformat(pattern, fstd::make_format_args(value));
            REQUIRE(std::string(ours.data(), ours.size()) ==
                    std::vformat(pattern, std::make_format_args(value)));
            const auto narrow = static_cast<float>(value);
            const auto ours_float = fstd::vformat(pattern, fstd::make_format_args(narrow));
            REQUIRE(std::string(ours_float.data(), ours_float.size()) ==
                    std::vformat(pattern, std::make_format_args(narrow)));
        }
    }
    REQUIRE(Ours("{:#g} {:#.3g} {:#G} {:#.2g}", 0.1, 0.000123, 1e-5, 0.5) ==
            "0.100000 0.000123 1.00000E-05 0.50");
    const long double extended = 2.5L;
    CHECK_SAME("{} {:.3f} {:e}", extended, extended, extended);
}

TEST_CASE("format of random doubles matches the host", "[libcxx][format]")
{
    std::mt19937_64 random(20261111);
    for (int round = 0; round != 3000; ++round)
    {
        double value = 0;
        const uint64_t bits = random();
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
        {
            continue;
        }
        INFO(bits);
        CHECK_SAME("{} {:e} {:.17g} {:+.3f}", value, value, value, value);
        if (std::fabs(value) >= 1)
        {
            CHECK_SAME("{:#g}", value);
        }
        const auto narrow = static_cast<float>(value);
        CHECK_SAME("{} {:g}", narrow, narrow);
        if (std::fpclassify(value) == FP_NORMAL)
        {
            CHECK_SAME("{:a} {:.3A}", value, value);
        }
        if (std::fpclassify(narrow) == FP_NORMAL)
        {
            CHECK_SAME("{:a}", narrow);
        }
    }
}

TEST_CASE("format_to, format_to_n and formatted_size", "[libcxx][format]")
{
    char buffer[32] = {};
    char* const end = fstd::format_to(buffer, "{}-{}", 12, "ab");
    REQUIRE(std::string(buffer, end) == "12-ab");

    fstd::string text;
    fstd::format_to(fstd::back_inserter(text), "{:>4}", 7);
    fstd::format_to(fstd::back_inserter(text), "{}", fstd::string(300, 'z'));
    REQUIRE(text.size() == 304);
    REQUIRE(text.substr(0, 5) == "   7z");

    char limited[8] = {};
    const auto result = fstd::format_to_n(limited, 4, "{}", 123456789);
    REQUIRE(result.size == 9);
    REQUIRE(result.out == limited + 4);
    REQUIRE(std::string(limited, 4) == "1234");
    const auto none = fstd::format_to_n(limited, -3, "{}", 5);
    REQUIRE(none.out == limited);
    REQUIRE(none.size == 1);

    REQUIRE(fstd::formatted_size("{:>1000}", 1) == 1000);
    REQUIRE(fstd::formatted_size("") == 0);

    // A long result passes through the buffer several times.
    const fstd::string many = fstd::format("{:x^1000}{:.2f}", "", 3.14159);
    REQUIRE(many.size() == 1004);
    REQUIRE(many.substr(996) == "xxxx3.14");
}

TEST_CASE("format arguments are visited by their stored type", "[libcxx][format]")
{
    short small = 3;
    unsigned long long wide = 4;
    float real = 1.5F;
    const char* text = "t";
    Point point{1, 2};
    auto store = fstd::make_format_args(small, wide, real, text, point);
    const fstd::format_args args = store;
    int kinds[6] = {};
    for (size_t index = 0; index != 6; ++index)
    {
        fstd::visit_format_arg(
            [&](auto value) {
                using T = decltype(value);
                kinds[index] = std::is_same_v<T, int>                  ? 1
                               : std::is_same_v<T, unsigned long long> ? 2
                               : std::is_same_v<T, float>              ? 3
                               : std::is_same_v<T, const char*>        ? 4
                               : std::is_same_v<T, fstd::basic_format_arg<
                                                       fstd::format_context>::handle>
                                   ? 5
                               : std::is_same_v<T, fstd::monostate> ? 6
                                                                    : 0;
            },
            args.get(index));
    }
    REQUIRE(kinds[0] == 1);
    REQUIRE(kinds[1] == 2);
    REQUIRE(kinds[2] == 3);
    REQUIRE(kinds[3] == 4);
    REQUIRE(kinds[4] == 5);
    REQUIRE(kinds[5] == 6);
    REQUIRE(!args.get(5));
}

TEST_CASE("print and println write to a stream", "[libcxx][format]")
{
    std::FILE* file = std::tmpfile();
    REQUIRE(file != nullptr);
    fstd::print(file, "{}+{}=", 2, 2);
    fstd::println(file, "{:>3}", 4);
    fstd::println(file, "{}", fstd::string(2000, 'q'));
    const char* tail = "end";
    fstd::vprint_nonunicode(file, "{}", fstd::make_format_args(tail));
    std::rewind(file);
    std::string contents;
    char chunk[256];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) != 0)
    {
        contents.append(chunk, read);
    }
    std::fclose(file);
    REQUIRE(contents == "2+2=  4\n" + std::string(2000, 'q') + "\nend");
}
