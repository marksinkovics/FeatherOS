#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/array"
#include "libcxx/include/functional"
#include "libcxx/include/iterator"
#include "libcxx/include/limits"
#include "libcxx/include/numeric"

#include <string>
#include <vector>

namespace
{

namespace fstd = featheros_std;

constexpr fstd::array<int, 5> Counted()
{
    fstd::array<int, 5> values{};
    fstd::iota(values.begin(), values.end(), 3);
    return values;
}

} // namespace

TEST_CASE("iota, accumulate and reduce", "[libcxx]")
{
    constexpr auto values = Counted();
    STATIC_REQUIRE(values == fstd::array<int, 5>{3, 4, 5, 6, 7});
    STATIC_REQUIRE(fstd::accumulate(values.begin(), values.end(), 0) == 25);
    STATIC_REQUIRE(fstd::accumulate(values.begin(), values.end(), 1,
                                    [](int left, int right) { return left * right; }) == 2520);
    STATIC_REQUIRE(fstd::reduce(values.begin(), values.end()) == 25);
    STATIC_REQUIRE(fstd::reduce(values.begin(), values.end(), 10L) == 35);
    STATIC_REQUIRE(fstd::transform_reduce(values.begin(), values.end(), 0, fstd::plus<>(),
                                          [](int value) { return value * value; }) == 135);
    STATIC_REQUIRE(fstd::transform_reduce(values.begin(), values.end(), values.begin(), 0) == 135);
    STATIC_REQUIRE(fstd::inner_product(values.begin(), values.end(), values.begin(), 0) == 135);

    // accumulate moves its running value, so building a string does not copy it each step.
    const std::vector<std::string> words{"fea", "ther"};
    REQUIRE(fstd::accumulate(words.begin(), words.end(), std::string()) == "feather");
}

TEST_CASE("the scans and adjacent_difference", "[libcxx]")
{
    const std::vector<int> values{1, 2, 3, 4};
    std::vector<int> out;
    fstd::partial_sum(values.begin(), values.end(), fstd::back_inserter(out));
    REQUIRE(out == std::vector<int>{1, 3, 6, 10});
    out.clear();
    fstd::inclusive_scan(values.begin(), values.end(), fstd::back_inserter(out));
    REQUIRE(out == std::vector<int>{1, 3, 6, 10});
    out.clear();
    fstd::inclusive_scan(values.begin(), values.end(), fstd::back_inserter(out), fstd::plus<>(), 10);
    REQUIRE(out == std::vector<int>{11, 13, 16, 20});
    out.clear();
    fstd::exclusive_scan(values.begin(), values.end(), fstd::back_inserter(out), 0);
    REQUIRE(out == std::vector<int>{0, 1, 3, 6});
    out.clear();
    fstd::transform_inclusive_scan(values.begin(), values.end(), fstd::back_inserter(out),
                                   fstd::plus<>(), [](int value) { return value * 2; });
    REQUIRE(out == std::vector<int>{2, 6, 12, 20});
    out.clear();
    fstd::transform_exclusive_scan(values.begin(), values.end(), fstd::back_inserter(out), 0,
                                   fstd::plus<>(), [](int value) { return value * 2; });
    REQUIRE(out == std::vector<int>{0, 2, 6, 12});
    out.clear();
    const std::vector<int> squares{1, 4, 9, 16};
    fstd::adjacent_difference(squares.begin(), squares.end(), fstd::back_inserter(out));
    REQUIRE(out == std::vector<int>{1, 3, 5, 7});

    // The scans may write over their input.
    std::vector<int> in_place{1, 2, 3};
    fstd::exclusive_scan(in_place.begin(), in_place.end(), in_place.begin(), 0);
    REQUIRE(in_place == std::vector<int>{0, 1, 3});
    std::vector<int> differences{1, 4, 9};
    fstd::adjacent_difference(differences.begin(), differences.end(), differences.begin());
    REQUIRE(differences == std::vector<int>{1, 3, 5});
}

TEST_CASE("gcd, lcm and midpoint", "[libcxx]")
{
    STATIC_REQUIRE(fstd::gcd(12, 18) == 6 && fstd::gcd(-12, 18) == 6 && fstd::gcd(0, 5) == 5);
    STATIC_REQUIRE(fstd::gcd(0, 0) == 0 && fstd::gcd(7u, 21L) == 7);
    STATIC_REQUIRE(fstd::lcm(4, 6) == 12 && fstd::lcm(-4, 6) == 12 && fstd::lcm(0, 6) == 0);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::gcd(short(1), 2L)), long>);
    STATIC_REQUIRE(fstd::gcd(fstd::numeric_limits<int>::min(), 6) == 2);

    STATIC_REQUIRE(fstd::midpoint(1, 4) == 2 && fstd::midpoint(4, 1) == 3);
    STATIC_REQUIRE(fstd::midpoint(-3, 2) == -1 && fstd::midpoint(2, -3) == 0);
    STATIC_REQUIRE(fstd::midpoint(fstd::numeric_limits<int>::max(), fstd::numeric_limits<int>::max() - 2) ==
                   fstd::numeric_limits<int>::max() - 1);
    STATIC_REQUIRE(fstd::midpoint(fstd::numeric_limits<int>::min(), fstd::numeric_limits<int>::max()) == -1);
    STATIC_REQUIRE(fstd::midpoint(250u, 4294967290u) == 2147483770u);
    STATIC_REQUIRE(fstd::midpoint(1.0, 2.0) == 1.5);
    constexpr double huge = fstd::numeric_limits<double>::max();
    STATIC_REQUIRE(fstd::midpoint(huge, huge) == huge);
    STATIC_REQUIRE(fstd::midpoint(-huge, huge) == 0.0);

    constexpr int values[5] = {1, 2, 3, 4, 5};
    STATIC_REQUIRE(fstd::midpoint(values, values + 5) == values + 2);
    STATIC_REQUIRE(fstd::midpoint(values + 5, values) == values + 3);
}
