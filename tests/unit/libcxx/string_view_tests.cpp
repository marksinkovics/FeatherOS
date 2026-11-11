#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/array"
#include "libcxx/include/string_view"

namespace
{

namespace fstd = featheros_std;
using namespace fstd::literals;

using View = fstd::string_view;
constexpr auto npos = View::npos;

struct Text
{
    char characters[4] = {'a', 'b', 'c', 'd'};
    const char* data() const { return characters; }
    fstd::size_t size() const { return 4; }
    const char* begin() const { return characters; }
    const char* end() const { return characters + 4; }
};

} // namespace

TEST_CASE("char_traits compares, finds and copies", "[libcxx]")
{
    using Traits = fstd::char_traits<char>;
    STATIC_REQUIRE(Traits::length("four") == 4);
    STATIC_REQUIRE(Traits::compare("abc", "abd", 3) < 0 && Traits::compare("abc", "abc", 3) == 0);
    STATIC_REQUIRE(Traits::lt('\x7F', '\x80'));
    STATIC_REQUIRE(Traits::eof() == -1 && Traits::not_eof(Traits::eof()) != Traits::eof());
    STATIC_REQUIRE(Traits::to_int_type('\xFF') == 255);
    STATIC_REQUIRE(fstd::char_traits<char16_t>::length(u"wide") == 4);
    STATIC_REQUIRE(fstd::char_traits<char32_t>::compare(U"b", U"a", 1) > 0);

    // Unsigned order at run time too, where compare uses memcmp.
    const char high[] = {'\x80'};
    const char low[] = {'\x01'};
    REQUIRE(Traits::compare(low, high, 1) < 0);

    char buffer[8] = "abcdef";
    Traits::move(buffer + 1, buffer, 4);
    REQUIRE(View(buffer) == "aabcdf");
    Traits::move(buffer, buffer + 2, 3);
    REQUIRE(View(buffer) == "bcdcdf");
    Traits::assign(buffer, 3, 'x');
    REQUIRE(View(buffer) == "xxxcdf");
    Traits::copy(buffer, "hi", 2);
    REQUIRE(View(buffer) == "hixcdf");
    REQUIRE(Traits::find(buffer, 5, 'd') == buffer + 4);
    REQUIRE(Traits::find(buffer, 5, 'q') == nullptr);

    constexpr auto moved = [] {
        fstd::array<char, 6> text{'a', 'b', 'c', 'd', 'e', '\0'};
        Traits::move(text.data() + 1, text.data(), 3);
        return text;
    }();
    STATIC_REQUIRE(View(moved.data()) == "aabce");
}

TEST_CASE("string_view constructs and slices", "[libcxx]")
{
    constexpr View empty;
    STATIC_REQUIRE(empty.empty() && empty.data() == nullptr);
    constexpr View hello = "hello, world";
    STATIC_REQUIRE(hello.size() == 12 && hello.front() == 'h' && hello.back() == 'd');
    STATIC_REQUIRE(hello.substr(7) == "world" && hello.substr(0, 5) == "hello");
    STATIC_REQUIRE(hello.substr(7, 100) == "world");
    STATIC_REQUIRE(View("abc", 2) == "ab");
    STATIC_REQUIRE(!fstd::is_constructible_v<View, fstd::nullptr_t>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<View>);

    static constexpr fstd::array<char, 3> characters{'x', 'y', 'z'};
    constexpr View from_iterators(characters.begin(), characters.end());
    STATIC_REQUIRE(from_iterators == "xyz");
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::basic_string_view(characters.begin(),
                                                                     characters.end())),
                                   View>);

    Text text;
    View from_range(text);
    REQUIRE(from_range == "abcd");
    STATIC_REQUIRE(!fstd::is_convertible_v<Text, View>);

    View view = "  trim  ";
    view.remove_prefix(2);
    view.remove_suffix(2);
    REQUIRE(view == "trim");

    char copied[4] = {};
    REQUIRE(hello.copy(copied, 3, 7) == 3);
    REQUIRE(View(copied, 3) == "wor");

    View left = "left";
    View right = "right";
    left.swap(right);
    REQUIRE((left == "right" && right == "left"));

    REQUIRE("literal"sv.size() == 7);
    STATIC_REQUIRE(fstd::is_same_v<decltype(u"wide"sv), fstd::u16string_view>);
    STATIC_REQUIRE(fstd::ranges::contiguous_range<View> && fstd::ranges::borrowed_range<View>);

    int reversed = 0;
    for (auto it = View("123").rbegin(); it != View("123").rend(); ++it)
    {
        reversed = reversed * 10 + (*it - '0');
    }
    REQUIRE(reversed == 321);
}

TEST_CASE("string_view compares", "[libcxx]")
{
    STATIC_REQUIRE("abc"sv == "abc" && "abc" == "abc"sv && "abc"sv != "abd");
    STATIC_REQUIRE("abc"sv < "abd"sv && "ab"sv < "abc"sv && "b"sv > "abc");
    STATIC_REQUIRE(("abc"sv <=> "abc"sv) == 0);
    STATIC_REQUIRE(fstd::is_same_v<decltype("a"sv <=> "b"sv), fstd::strong_ordering>);
    STATIC_REQUIRE("abc"sv.compare("abd") < 0 && "abd"sv.compare("abc") > 0);
    STATIC_REQUIRE("abc"sv.compare("ab") > 0 && "ab"sv.compare("abc") < 0);
    STATIC_REQUIRE("xabcx"sv.compare(1, 3, "abc") == 0);
    STATIC_REQUIRE("xabcx"sv.compare(1, 3, "zabcz"sv, 1, 3) == 0);
    STATIC_REQUIRE("xabcx"sv.compare(1, 2, "abc", 2) == 0);
    STATIC_REQUIRE("prefix-rest"sv.starts_with("prefix") && "prefix-rest"sv.starts_with('p'));
    STATIC_REQUIRE("prefix-rest"sv.ends_with("rest"sv) && !"prefix-rest"sv.ends_with('x'));
    STATIC_REQUIRE(!""sv.starts_with('a') && ""sv.starts_with(""));
    STATIC_REQUIRE("needle"sv.contains("eed") && "needle"sv.contains('l') && !"needle"sv.contains("x"));
}

TEST_CASE("string_view searches", "[libcxx]")
{
    constexpr View text = "abcabc";
    STATIC_REQUIRE(text.find("bc") == 1 && text.find("bc", 2) == 4 && text.find("bd") == npos);
    STATIC_REQUIRE(text.find('c') == 2 && text.find('c', 3) == 5 && text.find('z') == npos);
    STATIC_REQUIRE(text.find("") == 0 && text.find("", 6) == 6 && text.find("", 7) == npos);
    STATIC_REQUIRE(text.find("abcabcabc") == npos && text.find('a', 6) == npos);
    STATIC_REQUIRE(text.find("cab", 0, 2) == 2);

    STATIC_REQUIRE(text.rfind("bc") == 4 && text.rfind("bc", 3) == 1 && text.rfind("x") == npos);
    STATIC_REQUIRE(text.rfind('a') == 3 && text.rfind('a', 2) == 0 && text.rfind("") == 6);
    STATIC_REQUIRE(text.rfind("abc", 0) == 0 && View().rfind('a') == npos);

    STATIC_REQUIRE(text.find_first_of("cb") == 1 && text.find_first_of("cb", 3) == 4);
    STATIC_REQUIRE(text.find_first_of("xyz") == npos && text.find_first_of('c') == 2);
    STATIC_REQUIRE(text.find_last_of("ab") == 4 && text.find_last_of("ab", 2) == 1);
    STATIC_REQUIRE(text.find_last_of('a') == 3 && View().find_last_of("a") == npos);
    STATIC_REQUIRE(text.find_first_not_of("ab") == 2 && text.find_first_not_of("abc") == npos);
    STATIC_REQUIRE(text.find_first_not_of('a') == 1);
    STATIC_REQUIRE(text.find_last_not_of("c") == 4 && text.find_last_not_of("bc", 3) == 3);
    STATIC_REQUIRE(text.find_last_not_of('c') == 4 && text.find_last_not_of("abc") == npos);

    // The same at run time, where the char searches leave constant evaluation.
    View runtime = text;
    REQUIRE(runtime.find("ca") == 2);
    REQUIRE(runtime.rfind('b') == 4);
    REQUIRE(runtime.find_last_not_of("bc") == 3);
}
