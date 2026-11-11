#include <catch2/catch_test_macros.hpp>

// FeatherOS's C++ library, built as featheros_std next to the host's std (see
// FEATHEROS_STD_NAMESPACE in tests/unit/CMakeLists.txt).
#include "libcxx/include/type_traits"

// Outside the anonymous namespace: the swap functions are only declared, for the traits to find.
namespace TypeTraitsAdl
{
struct Swappable
{
};
void swap(Swappable&, Swappable&) noexcept;

struct ThrowingSwap
{
};
void swap(ThrowingSwap&, ThrowingSwap&);
} // namespace TypeTraitsAdl

namespace
{

namespace fstd = featheros_std;

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
};
struct NotCopyable
{
    NotCopyable() = default;
    NotCopyable(const NotCopyable&) = delete;
    NotCopyable& operator=(const NotCopyable&) = delete;
};
struct ThrowingDestructor
{
    ~ThrowingDestructor() noexcept(false) {}
};
struct PrivateDestructor
{
private:
    ~PrivateDestructor() = default;
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
    Value
};
struct Callable
{
    int operator()(int value) const { return value; }
    long Member(int value) const { return value; }
    int field = 0;
};

template <class... T>
concept HasCommonType = requires { typename fstd::common_type_t<T...>; };

} // namespace

TEST_CASE("type_traits classifies types", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_void_v<const void> && !fstd::is_void_v<void*>);
    STATIC_REQUIRE(fstd::is_null_pointer_v<decltype(nullptr)>);
    STATIC_REQUIRE(fstd::is_integral_v<bool> && fstd::is_integral_v<const char8_t> &&
                   fstd::is_integral_v<unsigned __int128> && !fstd::is_integral_v<Plain>);
    STATIC_REQUIRE(fstd::is_floating_point_v<volatile double> && !fstd::is_floating_point_v<int>);
    STATIC_REQUIRE(fstd::is_array_v<int[]> && fstd::is_array_v<int[3]> && !fstd::is_array_v<int*>);
    STATIC_REQUIRE(fstd::is_pointer_v<int* const> && !fstd::is_pointer_v<int Callable::*>);
    STATIC_REQUIRE(fstd::is_lvalue_reference_v<int&> && fstd::is_rvalue_reference_v<int&&>);
    STATIC_REQUIRE(fstd::is_function_v<int(int) const> && !fstd::is_function_v<int (&)(int)>);
    STATIC_REQUIRE(fstd::is_member_function_pointer_v<decltype(&Callable::Member)>);
    STATIC_REQUIRE(fstd::is_member_object_pointer_v<decltype(&Callable::field)>);
    STATIC_REQUIRE(fstd::is_class_v<Aggregate> && fstd::is_union_v<Union> &&
                   fstd::is_enum_v<Scoped> && !fstd::is_class_v<Union>);
    STATIC_REQUIRE(fstd::is_scalar_v<Plain> && fstd::is_object_v<Union> &&
                   !fstd::is_object_v<int&> && fstd::is_compound_v<int*>);
    STATIC_REQUIRE(fstd::is_fundamental_v<decltype(nullptr)> && !fstd::is_fundamental_v<int*>);
}

TEST_CASE("type_traits reports properties", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_const_v<const int> && !fstd::is_const_v<const int&>);
    STATIC_REQUIRE(fstd::is_signed_v<long> && !fstd::is_signed_v<unsigned> &&
                   fstd::is_signed_v<float> && !fstd::is_signed_v<Aggregate>);
    STATIC_REQUIRE(fstd::is_unsigned_v<bool> && !fstd::is_unsigned_v<int>);
    STATIC_REQUIRE(fstd::is_polymorphic_v<Base> && fstd::is_final_v<Derived> &&
                   fstd::is_abstract_v<Base> == false && fstd::has_virtual_destructor_v<Derived>);
    STATIC_REQUIRE(fstd::is_aggregate_v<Aggregate> && fstd::is_aggregate_v<int[2]> &&
                   !fstd::is_aggregate_v<Base>);
    STATIC_REQUIRE(fstd::is_trivially_copyable_v<Aggregate> && fstd::is_standard_layout_v<Aggregate>);
    STATIC_REQUIRE(fstd::is_empty_v<Derived> == false && fstd::is_empty_v<TypeTraitsAdl::Swappable>);
    STATIC_REQUIRE(fstd::is_scoped_enum_v<Scoped> && !fstd::is_scoped_enum_v<Plain> &&
                   !fstd::is_scoped_enum_v<int>);
    STATIC_REQUIRE(fstd::is_bounded_array_v<int[2]> && fstd::is_unbounded_array_v<int[]>);
    STATIC_REQUIRE(fstd::rank_v<int[1][2][3]> == 3 && fstd::extent_v<int[1][2][3], 2> == 3 &&
                   fstd::extent_v<int[], 0> == 0 && fstd::extent_v<int[4], 1> == 0);
    STATIC_REQUIRE(fstd::alignment_of_v<double> == alignof(double));
    STATIC_REQUIRE(fstd::has_unique_object_representations_v<int> &&
                   !fstd::has_unique_object_representations_v<float>);
    STATIC_REQUIRE(fstd::is_pod_v<Aggregate> && !fstd::is_pod_v<Base> && fstd::is_pod_v<int[2]>);
}

TEST_CASE("type_traits checks construction, assignment and destruction", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_constructible_v<long, int> && !fstd::is_constructible_v<int*, long>);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<NotCopyable> &&
                   fstd::is_default_constructible_v<NotCopyable>);
    STATIC_REQUIRE(fstd::is_trivially_copy_constructible_v<Aggregate> &&
                   !fstd::is_trivially_default_constructible_v<Base>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<Aggregate>);
    STATIC_REQUIRE(fstd::is_assignable_v<int&, long> && !fstd::is_assignable_v<int, int>);
    STATIC_REQUIRE(!fstd::is_copy_assignable_v<NotCopyable> && fstd::is_move_assignable_v<int>);
    STATIC_REQUIRE(fstd::is_trivially_move_assignable_v<Aggregate>);
    STATIC_REQUIRE(fstd::is_destructible_v<int> && fstd::is_destructible_v<int&> &&
                   fstd::is_destructible_v<Aggregate[2]> && !fstd::is_destructible_v<int[]> &&
                   !fstd::is_destructible_v<void> && !fstd::is_destructible_v<int()> &&
                   !fstd::is_destructible_v<PrivateDestructor>);
    STATIC_REQUIRE(fstd::is_nothrow_destructible_v<int> &&
                   !fstd::is_nothrow_destructible_v<ThrowingDestructor> &&
                   fstd::is_destructible_v<ThrowingDestructor>);
    STATIC_REQUIRE(fstd::is_trivially_destructible_v<Aggregate> &&
                   !fstd::is_trivially_destructible_v<Base> &&
                   !fstd::is_trivially_destructible_v<void>);
}

TEST_CASE("type_traits transforms types", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::remove_cvref_t<const volatile int&&>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::add_lvalue_reference_t<int&&>, int&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::add_rvalue_reference_t<void>, void>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::add_pointer_t<int&>, int*>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::remove_pointer_t<int* const>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::remove_extent_t<int[2][3]>, int[3]>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::decay_t<const int&>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::decay_t<int[3]>, int*>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::decay_t<void(int)>, void (*)(int)>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_signed_t<unsigned long long>, long long>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_signed_t<const volatile char>, const volatile signed char>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_unsigned_t<Plain>, unsigned char>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::make_unsigned_t<char32_t>, unsigned int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::underlying_type_t<Scoped>, short>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::conditional_t<false, int, long>, long>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::enable_if_t<true, char>, char>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::type_identity_t<int>, int>);
    // aligned_storage's default alignment is the largest power of two up to the size, at most
    // new's; aligned_union fits each type and the size.
    STATIC_REQUIRE(sizeof(fstd::aligned_storage_t<3>) == 4 && alignof(fstd::aligned_storage_t<3>) == 2);
    STATIC_REQUIRE(alignof(fstd::aligned_storage_t<12>) == 8);
    STATIC_REQUIRE(sizeof(fstd::aligned_storage_t<24>) == 32 &&
                   alignof(fstd::aligned_storage_t<24>) == __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    STATIC_REQUIRE(sizeof(fstd::aligned_storage_t<5, 8>) == 8 && alignof(fstd::aligned_storage_t<5, 8>) == 8);
    STATIC_REQUIRE(fstd::aligned_union<2, char, double>::alignment_value == alignof(double));
    STATIC_REQUIRE(sizeof(fstd::aligned_union_t<12, char, double>) == 16);
    STATIC_REQUIRE(sizeof(fstd::aligned_union_t<1, char[5]>) == 5);
}

TEST_CASE("type_traits finds common types and references", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_type_t<char, short, int>, int>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_type_t<const int&, long&&>, long>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_type_t<Derived*, Base*, const Base*>, const Base*>);
    STATIC_REQUIRE(!HasCommonType<int, Aggregate>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<int&, const int&>, const int&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<int&, int&&>, const int&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<int&&, const int&&>, const int&&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<Derived&, Base&>, Base&>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<int&, long>, long>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::common_reference_t<int>, int>);
}

TEST_CASE("type_traits checks invocation and conversion", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<fstd::invoke_result_t<Callable&, int>, int>);
    STATIC_REQUIRE(
        fstd::is_same_v<fstd::invoke_result_t<decltype(&Callable::Member), Callable*, int>, long>);
    STATIC_REQUIRE(
        fstd::is_same_v<fstd::invoke_result_t<decltype(&Callable::field), const Callable&>,
                        const int&>);
    STATIC_REQUIRE(
        fstd::is_same_v<fstd::invoke_result_t<decltype(&Callable::field), Callable>, int&&>);
    STATIC_REQUIRE(fstd::is_invocable_v<int (*)(int), char>);
    STATIC_REQUIRE(!fstd::is_invocable_v<Callable, Aggregate>);
    STATIC_REQUIRE(fstd::is_invocable_r_v<void, Callable, int> &&
                   !fstd::is_invocable_r_v<int*, Callable, int>);
    STATIC_REQUIRE(fstd::is_nothrow_invocable_v<void (*)() noexcept> &&
                   !fstd::is_nothrow_invocable_v<void (*)()>);
    STATIC_REQUIRE(fstd::is_convertible_v<Derived*, Base*> && !fstd::is_convertible_v<Base*, Derived*>);
    STATIC_REQUIRE(fstd::is_convertible_v<void, void> && !fstd::is_convertible_v<int, void*>);
    STATIC_REQUIRE(fstd::is_nothrow_convertible_v<int, long>);
    STATIC_REQUIRE(fstd::is_base_of_v<Base, Derived> && !fstd::is_base_of_v<Derived, Base>);
}

TEST_CASE("type_traits checks swapping", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_swappable_v<int> && fstd::is_swappable_v<int[3]>);
    STATIC_REQUIRE(!fstd::is_swappable_v<NotCopyable> && !fstd::is_swappable_v<const int>);
    STATIC_REQUIRE(fstd::is_nothrow_swappable_v<TypeTraitsAdl::Swappable> &&
                   fstd::is_swappable_v<TypeTraitsAdl::ThrowingSwap> &&
                   !fstd::is_nothrow_swappable_v<TypeTraitsAdl::ThrowingSwap>);
    STATIC_REQUIRE(fstd::is_swappable_with_v<int&, int&> && !fstd::is_swappable_with_v<int&, long&>);

    int left = 1;
    int right = 2;
    fstd::swap(left, right);
    REQUIRE((left == 2 && right == 1));
    int first[2] = {1, 2};
    int second[2] = {3, 4};
    fstd::swap(first, second);
    REQUIRE((first[1] == 4 && second[0] == 1));
}

TEST_CASE("type_traits combines traits", "[libcxx]")
{
    STATIC_REQUIRE(fstd::conjunction_v<> && !fstd::disjunction_v<>);
    STATIC_REQUIRE(fstd::conjunction_v<fstd::true_type, fstd::is_integral<int>>);
    // The second operand would not compile; conjunction stops at the first false one.
    STATIC_REQUIRE(!fstd::conjunction_v<fstd::false_type, Aggregate>);
    STATIC_REQUIRE(fstd::disjunction_v<fstd::true_type, Aggregate>);
    STATIC_REQUIRE(fstd::negation_v<fstd::is_void<int>>);
    STATIC_REQUIRE(fstd::integral_constant<int, 4>() == 4 && fstd::bool_constant<true>::value);
    STATIC_REQUIRE(fstd::is_constant_evaluated());
}
