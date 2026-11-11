#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <list>
#include <random>
#include <string>
#include <vector>

#include "libcxx/include/memory"
#include "libcxx/include/string"
#include "libcxx/include/string_view"

namespace
{

namespace fstd = featheros_std;
using namespace fstd::string_literals;

std::string Host(const fstd::string& text)
{
    return std::string(text.data(), text.size());
}

// The contents match and the terminator is in place.
bool Same(const fstd::string& text, const std::string& expected)
{
    return Host(text) == expected && text.c_str()[text.size()] == '\0' &&
           text.size() <= text.capacity();
}

constexpr bool ConstantEvaluation()
{
    fstd::string text = "hello";
    text += ", world: a string past the local capacity";
    text.insert(0, text);
    text.replace(2, 3, "XYZ");
    text.erase(10, 20);
    text.append(3, '!');
    fstd::string other = text.substr(5, 10);
    fstd::string moved = fstd::move(text).substr(1, 4);
    other.swap(moved);
    other.resize_and_overwrite(8, [](char* buffer, fstd::size_t) {
        buffer[4] = 'a';
        buffer[5] = 'b';
        return 6;
    });
    fstd::string sum = other + "-" + fstd::string(2, 'z');
    sum.shrink_to_fit();
    return sum == "eXYZab-zz" && sum.find("ab") == 4 && sum.compare("eXYZ") > 0 &&
           sum.starts_with('e') && sum.ends_with("zz") && moved.size() == 10 &&
           fstd::string("abc") < fstd::string("abd") && "x"s.size() == 1;
}
static_assert(ConstantEvaluation());

constexpr bool ConstantLocal()
{
    fstd::string text;
    text.push_back('a');
    text.push_back('b');
    fstd::string copy = text;
    copy.insert(1, copy);
    return copy == "aabb" && text.capacity() == 15;
}
static_assert(ConstantLocal());

int live_blocks = 0;

// Allocations through it are counted, and two of them compare equal only with the same id.
template <class T> struct CountingAllocator
{
    using value_type = T;
    using propagate_on_container_copy_assignment = fstd::true_type;
    using propagate_on_container_move_assignment = fstd::false_type;
    using propagate_on_container_swap = fstd::true_type;
    using is_always_equal = fstd::false_type;

    int id = 0;

    CountingAllocator() = default;
    explicit CountingAllocator(int identity) : id(identity) {}
    template <class U> CountingAllocator(const CountingAllocator<U>& other) : id(other.id) {}

    T* allocate(fstd::size_t count)
    {
        ++live_blocks;
        return std::allocator<T>().allocate(count);
    }
    void deallocate(T* pointer, fstd::size_t count)
    {
        --live_blocks;
        std::allocator<T>().deallocate(pointer, count);
    }
    friend bool operator==(const CountingAllocator& left, const CountingAllocator& right)
    {
        return left.id == right.id;
    }
};

using CountedString = fstd::basic_string<char, fstd::char_traits<char>, CountingAllocator<char>>;

// Only ++ and *, so the members cannot count it before reading.
struct InputOnly
{
    using iterator_category = std::input_iterator_tag;
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    using pointer = const char*;
    using reference = const char&;
    const char* position;
    const char& operator*() const { return *position; }
    InputOnly& operator++()
    {
        ++position;
        return *this;
    }
    InputOnly operator++(int)
    {
        InputOnly old = *this;
        ++position;
        return old;
    }
    bool operator==(const InputOnly& other) const { return position == other.position; }
};

struct InputRange
{
    const char* first;
    const char* last;
    InputOnly begin() const { return {first}; }
    InputOnly end() const { return {last}; }
};

} // namespace

TEST_CASE("string construction and the local storage boundary", "[libcxx][string]")
{
    STATIC_REQUIRE(sizeof(fstd::string) == 4 * sizeof(void*));
    STATIC_REQUIRE(fstd::contiguous_iterator<fstd::string::iterator>);
    STATIC_REQUIRE(fstd::contiguous_iterator<fstd::string::const_iterator>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::string>);

    const fstd::string empty;
    REQUIRE(Same(empty, ""));
    REQUIRE(empty.capacity() == 15);

    for (std::size_t length = 0; length < 40; ++length)
    {
        const std::string expected(length, 'q');
        const fstd::string filled(length, 'q');
        REQUIRE(Same(filled, expected));
        const fstd::string from_pointer(expected.c_str());
        REQUIRE(Same(from_pointer, expected));
        fstd::string copy = filled;
        REQUIRE(Same(copy, expected));
        fstd::string moved = fstd::move(copy);
        REQUIRE(Same(moved, expected));
        REQUIRE(Same(copy, ""));
        REQUIRE(copy.capacity() == 15);
        REQUIRE((length <= 15) == (moved.capacity() == 15));
    }

    const fstd::string source = "0123456789abcdefghij";
    REQUIRE(Same(fstd::string(source, 5), "56789abcdefghij"));
    REQUIRE(Same(fstd::string(source, 5, 3), "567"));
    REQUIRE(Same(fstd::string(source, 18, 100), "ij"));
    REQUIRE(Same(fstd::string(fstd::string(source), 2, 4), "2345"));
    REQUIRE(Same(fstd::string("abcdef", 3), "abc"));
    REQUIRE(Same(fstd::string({'x', 'y'}), "xy"));
    REQUIRE(Same(fstd::string(fstd::string_view("view")), "view"));
    REQUIRE(Same(fstd::string(fstd::string_view("a view"), 2, 3), "vie"));
    const std::vector<char> characters = {'v', 'e', 'c'};
    REQUIRE(Same(fstd::string(characters.begin(), characters.end()), "vec"));
    const char* text = "input only";
    REQUIRE(Same(fstd::string(InputOnly{text}, InputOnly{text + 10}), "input only"));
    REQUIRE(Same(fstd::string(fstd::from_range, characters), "vec"));
    REQUIRE(Same(fstd::string(fstd::from_range, InputRange{text, text + 5}), "input"));
    const std::list<char> listed = {'l', 'i', 's', 't'};
    REQUIRE(Same(fstd::string(listed.begin(), listed.end()), "list"));
    REQUIRE(Same("literal"s, "literal"));

    fstd::basic_string deduced(characters.begin(), characters.end());
    STATIC_REQUIRE(fstd::is_same_v<decltype(deduced), fstd::string>);
    fstd::basic_string from_view(fstd::string_view("x"));
    STATIC_REQUIRE(fstd::is_same_v<decltype(from_view), fstd::string>);
}

TEST_CASE("string arguments that alias the string itself", "[libcxx][string]")
{
    for (const char* start : {"abcdef", "abcdefghijklmnopqrstuvwxyz0123456789"})
    {
        const std::string original = start;
        std::string expected = original;
        fstd::string text = start;

        text.insert(0, text);
        expected.insert(0, expected);
        REQUIRE(Same(text, expected));

        text.append(text);
        expected.append(expected);
        REQUIRE(Same(text, expected));

        text.replace(1, 2, text.c_str() + 3, 5);
        expected.replace(1, 2, std::string(expected.c_str() + 3, 5));
        REQUIRE(Same(text, expected));

        text.insert(2, text.c_str() + 1, 4);
        expected.insert(2, std::string(expected.c_str() + 1, 4));
        REQUIRE(Same(text, expected));

        text.insert(text.begin() + 3, text.begin(), text.begin() + 7);
        expected.insert(3, expected.substr(0, 7));
        REQUIRE(Same(text, expected));

        text.assign(text.c_str() + 2, 5);
        expected = expected.substr(2, 5);
        REQUIRE(Same(text, expected));

        const fstd::string& self = text;
        text = self;
        REQUIRE(Same(text, expected));
        text.push_back(text[0]);
        expected.push_back(expected[0]);
        REQUIRE(Same(text, expected));
    }
}

TEST_CASE("string against the host's in random operations", "[libcxx][string]")
{
    std::mt19937 generator(23);
    auto below = [&](std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(generator() % bound);
    };
    auto piece = [&] {
        std::string result(below(24), ' ');
        for (char& character : result)
        {
            character = static_cast<char>('a' + below(26));
        }
        return result;
    };

    fstd::string text;
    std::string expected;
    for (int step = 0; step < 6000; ++step)
    {
        const std::size_t size = expected.size();
        const std::size_t position = below(size + 1);
        const std::size_t count = below(size - position + 3);
        const std::string inserted = piece();
        switch (below(17))
        {
        case 0:
            text.insert(position, inserted.c_str());
            expected.insert(position, inserted);
            break;
        case 1:
            text.erase(position, count);
            expected.erase(position, count);
            break;
        case 2:
            text.replace(position, count, inserted.data(), inserted.size());
            expected.replace(position, count, inserted);
            break;
        case 3:
            text.append(inserted.data(), inserted.size());
            expected.append(inserted);
            break;
        case 4:
            text.push_back('#');
            expected.push_back('#');
            break;
        case 5:
            if (size != 0)
            {
                text.pop_back();
                expected.pop_back();
            }
            break;
        case 6:
            text.resize(count * 3, '-');
            expected.resize(count * 3, '-');
            break;
        case 7:
            text.shrink_to_fit();
            break;
        case 8:
            text.reserve(below(200));
            break;
        case 9:
        {
            // A piece of the string itself.
            const std::size_t length = below(size - position + 1);
            const std::size_t at = below(size + 1);
            const std::string copied = expected.substr(position, length);
            text.insert(at, text.data() + position, length);
            expected.insert(at, copied);
            break;
        }
        case 10:
        {
            const std::size_t fill = below(5);
            text.replace(position, count, fill, 'R');
            expected.replace(position, count, fill, 'R');
            break;
        }
        case 11:
        {
            fstd::string copy = text;
            text = fstd::move(copy);
            break;
        }
        case 12:
            text.assign(inserted.c_str());
            expected = inserted;
            break;
        case 13:
            if (size > 10)
            {
                text = text.substr(position / 2, count + 5);
                expected = expected.substr(position / 2, count + 5);
            }
            break;
        case 14:
        {
            fstd::string other = inserted.c_str();
            text.swap(other);
            text.swap(other);
            break;
        }
        case 15:
            text.insert(text.begin() + static_cast<std::ptrdiff_t>(position), count, 'I');
            expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(position), count, 'I');
            break;
        default:
            text.erase(text.begin() + static_cast<std::ptrdiff_t>(position),
                       text.begin() + static_cast<std::ptrdiff_t>(
                                          position + std::min(count, size - position)));
            expected.erase(position, std::min(count, size - position));
            break;
        }
        REQUIRE(Same(text, expected));
    }
}

TEST_CASE("string replace, erase and resize forms", "[libcxx][string]")
{
    fstd::string text = "0123456789";
    text.replace(text.begin() + 1, text.begin() + 3, "ab");
    REQUIRE(Same(text, "0ab3456789"));
    text.replace(text.begin(), text.begin() + 1, {'x', 'y', 'z'});
    REQUIRE(Same(text, "xyzab3456789"));
    text.replace(0, 3, fstd::string_view("<>"));
    REQUIRE(Same(text, "<>ab3456789"));
    text.replace(0, 2, fstd::string_view("[ignored]"), 1, 7);
    REQUIRE(Same(text, "ignoredab3456789"));
    const std::vector<char> range = {'!', '?'};
    text.replace(text.begin(), text.begin() + 7, range.begin(), range.end());
    REQUIRE(Same(text, "!?ab3456789"));
    const char* in = "in++";
    text.replace_with_range(text.begin(), text.begin() + 2, InputRange{in, in + 2});
    REQUIRE(Same(text, "inab3456789"));
    text.replace(0, fstd::string::npos, "short");
    REQUIRE(Same(text, "short"));
    text.insert(text.end(), 'X');
    text.insert(text.begin(), {'(', ')'});
    text.insert(1, fstd::string_view("view"));
    text.insert(0, "tail-ignored", 4);
    REQUIRE(Same(text, "tail(view)shortX"));
    text.insert_range(text.begin(), range);
    REQUIRE(Same(text, "!?tail(view)shortX"));
    text.append_range(InputRange{in + 2, in + 4});
    REQUIRE(Same(text, "!?tail(view)shortX++"));
    text.assign_range(range);
    REQUIRE(Same(text, "!?"));
    text.assign(fstd::string("abcdef"), 2, 2);
    REQUIRE(Same(text, "cd"));
    text.append(fstd::string("uvw"), 1).append(2, '.').append({'e'}).append(fstd::string_view("V"));
    REQUIRE(Same(text, "cdvw..eV"));
    REQUIRE(text.erase(text.begin()) == text.begin());
    REQUIRE(Same(text, "dvw..eV"));
    text.erase(2);
    REQUIRE(Same(text, "dv"));
    text.resize(4);
    REQUIRE(text.size() == 4);
    REQUIRE(text[3] == '\0');
    char buffer[4] = {};
    REQUIRE(fstd::string("copy me").copy(buffer, 3, 2) == 3);
    REQUIRE(std::string(buffer, 3) == "py ");
    text.clear();
    REQUIRE(Same(text, ""));

    fstd::string overwrite = "keep";
    overwrite.resize_and_overwrite(40, [](char* data, fstd::size_t count) {
        for (fstd::size_t index = 4; index < count; ++index)
        {
            data[index] = '+';
        }
        return count - 6;
    });
    REQUIRE(Same(overwrite, "keep" + std::string(30, '+')));
}

TEST_CASE("string searches, comparisons and concatenation", "[libcxx][string]")
{
    const fstd::string text = "the quick brown fox jumps over the lazy dog";
    const std::string expected = Host(text);
    for (const char* needle : {"the", "o", "fox", "", "cat", "dog", "eh"})
    {
        const fstd::string fneedle = needle;
        REQUIRE(text.find(fneedle) == expected.find(needle));
        REQUIRE(text.rfind(needle) == expected.rfind(needle));
        REQUIRE(text.find_first_of(needle, 3) == expected.find_first_of(needle, 3));
        REQUIRE(text.find_last_of(fstd::string_view(needle)) == expected.find_last_of(needle));
        REQUIRE(text.find_first_not_of(needle) == expected.find_first_not_of(needle));
        REQUIRE(text.find_last_not_of(fneedle, 20) == expected.find_last_not_of(needle, 20));
        REQUIRE((text.compare(needle) < 0) == (expected.compare(needle) < 0));
        REQUIRE(text.contains(needle) == (expected.find(needle) != std::string::npos));
    }
    REQUIRE(text.find('q') == 4);
    REQUIRE(text.compare(4, 5, "quick") == 0);
    REQUIRE(text.compare(4, 5, fstd::string("xquickx"), 1, 5) == 0);
    REQUIRE(text.starts_with(fstd::string_view("the q")));
    REQUIRE(text.ends_with('g'));

    const fstd::string a = "abc";
    const fstd::string b = "abd";
    REQUIRE(a < b);
    REQUIRE((a <=> b) == fstd::strong_ordering::less);
    REQUIRE(a == "abc");
    REQUIRE("abc" == a);
    REQUIRE(a != b);
    REQUIRE(a == fstd::string_view("abc"));

    REQUIRE(Same(a + b, "abcabd"));
    REQUIRE(Same(a + "!", "abc!"));
    REQUIRE(Same("!" + a, "!abc"));
    REQUIRE(Same(a + '?', "abc?"));
    REQUIRE(Same('?' + a, "?abc"));
    REQUIRE(Same(fstd::string(a) + fstd::string(b), "abcabd"));
    REQUIRE(Same(fstd::string(a) + b, "abcabd"));
    REQUIRE(Same(a + fstd::string(b), "abcabd"));
    REQUIRE(Same(fstd::string(a) + "x", "abcx"));
    REQUIRE(Same("x" + fstd::string(a), "xabc"));
    REQUIRE(Same(fstd::string(a) + 'y', "abcy"));
    REQUIRE(Same('y' + fstd::string(a), "yabc"));
    fstd::string long_right(40, 'r');
    long_right.reserve(100);
    REQUIRE(Same(fstd::string("l") + fstd::move(long_right), "l" + std::string(40, 'r')));

    fstd::string erased = "a-b-c--d";
    REQUIRE(fstd::erase(erased, '-') == 4);
    REQUIRE(Same(erased, "abcd"));
    REQUIRE(fstd::erase_if(erased, [](char character) { return character < 'c'; }) == 2);
    REQUIRE(Same(erased, "cd"));

    REQUIRE(fstd::hash<fstd::string>{}(a) == fstd::hash<fstd::string_view>{}("abc"));
    REQUIRE(fstd::hash<fstd::u16string>{}(u"hi") == fstd::hash<fstd::u16string_view>{}(u"hi"));
}

TEST_CASE("string allocators propagate and nothing leaks", "[libcxx][string]")
{
    {
        CountedString first("a string long enough for the heap", CountingAllocator<char>(1));
        CountedString second(CountingAllocator<char>(2));
        second = first;
        REQUIRE(second.get_allocator().id == 1);
        CountedString third("x", CountingAllocator<char>(3));
        third = fstd::move(first);
        REQUIRE(third.get_allocator().id == 3);
        REQUIRE(third.size() == 33);
        CountedString fourth(fstd::move(second), CountingAllocator<char>(4));
        REQUIRE(fourth.get_allocator().id == 4);
        REQUIRE(fourth.size() == 33);
        fourth.swap(third);
        REQUIRE(fourth.get_allocator().id == 3);
        CountedString built(fourth, CountingAllocator<char>(5));
        built.append(100, 'z');
        built.shrink_to_fit();
        built.resize(3);
        built.shrink_to_fit();
        REQUIRE(built.capacity() == 15);
    }
    REQUIRE(live_blocks == 0);
}

TEST_CASE("wide strings", "[libcxx][string]")
{
    REQUIRE(fstd::u32string(U"abc").capacity() == 3);
    fstd::u32string text = U"wide";
    REQUIRE(text.capacity() >= 4);
    text += U" characters past the local storage";
    REQUIRE(text.size() == 38);
    REQUIRE(text.find(U"char") == 5);
    text.insert(0, text);
    REQUIRE(text.size() == 76);
    REQUIRE(text.c_str()[76] == U'\0');
    fstd::u16string sixteen(20, u'x');
    sixteen.replace(2, 15, u"ab");
    REQUIRE(sixteen == u"xxabxxx");
    fstd::wstring wide = L"w";
    wide.append(3, L'!');
    REQUIRE(wide == L"w!!!");
    fstd::u8string eight = u8"utf-8";
    REQUIRE(eight.size() == 5);
}

TEST_CASE("string numeric conversions", "[libcxx][string]")
{
    fstd::size_t end = 0;
    REQUIRE(fstd::stoi(fstd::string("  -42abc"), &end) == -42);
    REQUIRE(end == 5);
    REQUIRE(fstd::stoi(fstd::string("ff"), nullptr, 16) == 255);
    REQUIRE(fstd::stol(fstd::string("0x1f"), nullptr, 0) == 31);
    REQUIRE(fstd::stoll(fstd::string("-9223372036854775808")) == INT64_MIN);
    REQUIRE(fstd::stoul(fstd::string("777"), nullptr, 8) == 511);
    REQUIRE(fstd::stoull(fstd::string("18446744073709551615")) == UINT64_MAX);
    REQUIRE(fstd::stod(fstd::string("2.5e3"), &end) == 2500.0);
    REQUIRE(end == 5);
    REQUIRE(fstd::stof(fstd::string("0.25")) == 0.25F);
    REQUIRE(fstd::stold(fstd::string("-1.5")) == -1.5L);

    REQUIRE(Same(fstd::to_string(0), "0"));
    REQUIRE(Same(fstd::to_string(-2147483647 - 1), "-2147483648"));
    REQUIRE(Same(fstd::to_string(4294967295U), "4294967295"));
    REQUIRE(Same(fstd::to_string(INT64_MIN), "-9223372036854775808"));
    REQUIRE(Same(fstd::to_string(UINT64_MAX), "18446744073709551615"));
    REQUIRE(Same(fstd::to_string(1.5), "1.500000"));
    REQUIRE(Same(fstd::to_string(-0.25F), "-0.250000"));
    REQUIRE(Same(fstd::to_string(1e20L), "100000000000000000000.000000"));
    char expected[400];
    std::snprintf(expected, sizeof(expected), "%f", 1e300);
    REQUIRE(Same(fstd::to_string(1e300), expected));
}
