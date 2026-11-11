#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/memory"

namespace
{

namespace fstd = featheros_std;

struct Tracked
{
    static inline int alive = 0;
    int value;
    explicit Tracked(int initial = 0) : value(initial) { ++alive; }
    Tracked(const Tracked& other) : value(other.value) { ++alive; }
    Tracked& operator=(const Tracked&) = default;
    ~Tracked() { --alive; }
};

struct Base
{
    int base_value = 1;
    virtual ~Base() = default;
};
struct Derived : Base
{
    int derived_value = 2;
};

struct alignas(64) OverAligned
{
    char byte;
};

struct CountingDeleter
{
    int* calls;
    void operator()(int* pointer) const
    {
        ++*calls;
        delete pointer;
    }
};

// A pointer type of unique_ptr's deleter's own: anything comparable with nullptr.
struct Handle
{
    int id = 0;
    Handle() = default;
    Handle(fstd::nullptr_t) {}
    explicit Handle(int value) : id(value) {}
    friend bool operator==(Handle, Handle) = default;
};

struct HandleDeleter
{
    using pointer = Handle;
    int* last_closed;
    void operator()(Handle handle) const { *last_closed = handle.id; }
};

// Minimal allocator: only value_type, allocate and deallocate.
template <class T> struct MinimalAllocator
{
    using value_type = T;
    MinimalAllocator() = default;
    template <class U> MinimalAllocator(const MinimalAllocator<U>&) {}
    T* allocate(size_t count) { return fstd::allocator<T>().allocate(count); }
    void deallocate(T* pointer, size_t count) { fstd::allocator<T>().deallocate(pointer, count); }
};

struct StatefulAllocator
{
    using value_type = int;
    using size_type = unsigned short;
    using propagate_on_container_swap = fstd::true_type;
    int id = 0;
    int* allocate(size_t count) { return fstd::allocator<int>().allocate(count); }
    void deallocate(int* pointer, size_t count) { fstd::allocator<int>().deallocate(pointer, count); }
    size_type max_size() const { return 7; }
};

// Takes an allocator after allocator_arg, at the end, or not at all.
struct LeadingAllocator
{
    using allocator_type = fstd::allocator<int>;
    int value;
    bool had_allocator;
    constexpr explicit LeadingAllocator(int initial) : value(initial), had_allocator(false) {}
    constexpr LeadingAllocator(fstd::allocator_arg_t, const allocator_type&, int initial)
        : value(initial), had_allocator(true)
    {
    }
};

struct TrailingAllocator
{
    using allocator_type = fstd::allocator<int>;
    int value;
    bool had_allocator;
    constexpr explicit TrailingAllocator(int initial) : value(initial), had_allocator(false) {}
    constexpr TrailingAllocator(int initial, const allocator_type&)
        : value(initial), had_allocator(true)
    {
    }
};

struct Node : fstd::enable_shared_from_this<Node>
{
    int value = 5;
};

constexpr int AllocateAndSum()
{
    using Traits = fstd::allocator_traits<fstd::allocator<int>>;
    fstd::allocator<int> allocator;
    int* values = Traits::allocate(allocator, 3);
    for (int index = 0; index != 3; ++index)
    {
        Traits::construct(allocator, values + index, index + 1);
    }
    const int sum = values[0] + values[1] + values[2];
    Traits::deallocate(allocator, values, 3);
    return sum;
}

constexpr int UniqueInConstantEvaluation()
{
    auto first = fstd::make_unique<int>(4);
    fstd::unique_ptr<int> second = fstd::move(first);
    second.reset(new int(6));
    auto array = fstd::make_unique<int[]>(3);
    array[1] = 2;
    return *second + array[1] + (first ? 100 : 0);
}

} // namespace

TEST_CASE("allocator allocates, also over-aligned types and in constant evaluation", "[libcxx]")
{
    STATIC_REQUIRE(AllocateAndSum() == 6);

    fstd::allocator<int> allocator;
    int* values = allocator.allocate(4);
    values[3] = 9;
    REQUIRE(values[3] == 9);
    allocator.deallocate(values, 4);

    fstd::allocator<OverAligned> aligned;
    auto [pointer, count] = aligned.allocate_at_least(2);
    REQUIRE(count == 2);
    REQUIRE(reinterpret_cast<__UINTPTR_TYPE__>(pointer) % 64 == 0);
    aligned.deallocate(pointer, count);

    STATIC_REQUIRE(fstd::allocator<int>() == fstd::allocator<long>());
    STATIC_REQUIRE(fstd::is_nothrow_constructible_v<fstd::allocator<int>, fstd::allocator<char>>);
}

TEST_CASE("allocator_traits fills in what an allocator leaves out", "[libcxx]")
{
    using Minimal = fstd::allocator_traits<MinimalAllocator<int>>;
    STATIC_REQUIRE(fstd::is_same_v<Minimal::pointer, int*>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::const_pointer, const int*>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::void_pointer, void*>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::const_void_pointer, const void*>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::difference_type, ptrdiff_t>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::size_type, size_t>);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::propagate_on_container_swap, fstd::false_type>);
    STATIC_REQUIRE(Minimal::is_always_equal::value);
    STATIC_REQUIRE(fstd::is_same_v<Minimal::rebind_alloc<char>, MinimalAllocator<char>>);
    STATIC_REQUIRE(Minimal::max_size(MinimalAllocator<int>()) ==
                   static_cast<size_t>(-1) / sizeof(int));

    using Stateful = fstd::allocator_traits<StatefulAllocator>;
    STATIC_REQUIRE(fstd::is_same_v<Stateful::size_type, unsigned short>);
    STATIC_REQUIRE(Stateful::propagate_on_container_swap::value);
    STATIC_REQUIRE(!Stateful::is_always_equal::value);
    StatefulAllocator stateful{.id = 3};
    REQUIRE(Stateful::max_size(stateful) == 7);
    REQUIRE(Stateful::select_on_container_copy_construction(stateful).id == 3);

    MinimalAllocator<Tracked> allocator;
    using Traits = fstd::allocator_traits<MinimalAllocator<Tracked>>;
    Tracked* tracked = Traits::allocate(allocator, 1, nullptr);
    Traits::construct(allocator, tracked, 8);
    REQUIRE((Tracked::alive == 1 && tracked->value == 8));
    Traits::destroy(allocator, tracked);
    REQUIRE(Tracked::alive == 0);
    Traits::deallocate(allocator, tracked, 1);
}

TEST_CASE("uses-allocator construction passes the allocator where it is taken", "[libcxx]")
{
    const fstd::allocator<int> allocator;
    STATIC_REQUIRE(fstd::uses_allocator_v<LeadingAllocator, fstd::allocator<int>>);
    STATIC_REQUIRE(!fstd::uses_allocator_v<int, fstd::allocator<int>>);
    STATIC_REQUIRE(fstd::uses_allocator_v<fstd::tuple<int>, fstd::allocator<int>>);

    STATIC_REQUIRE(fstd::make_obj_using_allocator<LeadingAllocator>(fstd::allocator<int>(), 2)
                       .had_allocator);
    STATIC_REQUIRE(fstd::make_obj_using_allocator<TrailingAllocator>(fstd::allocator<int>(), 3)
                       .had_allocator);
    STATIC_REQUIRE(fstd::make_obj_using_allocator<int>(fstd::allocator<int>(), 4) == 4);
    STATIC_REQUIRE(fstd::tuple_size_v<decltype(fstd::uses_allocator_construction_args<
                                                   LeadingAllocator>(allocator, 1))> == 3);

    const auto pair = fstd::make_obj_using_allocator<fstd::pair<LeadingAllocator, int>>(
        allocator, fstd::piecewise_construct, fstd::forward_as_tuple(5), fstd::forward_as_tuple(6));
    REQUIRE((pair.first.had_allocator && pair.first.value == 5 && pair.second == 6));
    const auto from_values =
        fstd::make_obj_using_allocator<fstd::pair<int, TrailingAllocator>>(allocator, 1, 2);
    REQUIRE((from_values.second.had_allocator && from_values.second.value == 2));
    const fstd::pair<int, int> source(7, 8);
    const auto from_pair =
        fstd::make_obj_using_allocator<fstd::pair<LeadingAllocator, long>>(allocator, source);
    REQUIRE((from_pair.first.value == 7 && from_pair.second == 8));

    alignas(TrailingAllocator) unsigned char storage[sizeof(TrailingAllocator)];
    auto* built = fstd::uninitialized_construct_using_allocator(
        reinterpret_cast<TrailingAllocator*>(storage), allocator, 9);
    REQUIRE((built->had_allocator && built->value == 9));
}

TEST_CASE("assume_aligned and align", "[libcxx]")
{
    alignas(16) int values[4] = {1, 2, 3, 4};
    REQUIRE(fstd::assume_aligned<16>(values)[2] == 3);

    alignas(16) unsigned char buffer[64];
    void* pointer = buffer + 1;
    size_t space = 63;
    REQUIRE(fstd::align(16, 8, pointer, space) == buffer + 16);
    REQUIRE((pointer == buffer + 16 && space == 48));
    pointer = buffer + 1;
    space = 20;
    REQUIRE(fstd::align(16, 8, pointer, space) == nullptr);
    REQUIRE((pointer == buffer + 1 && space == 20));
}

TEST_CASE("unique_ptr owns one object", "[libcxx]")
{
    STATIC_REQUIRE(sizeof(fstd::unique_ptr<int>) == sizeof(int*));
    STATIC_REQUIRE(UniqueInConstantEvaluation() == 8);
    STATIC_REQUIRE(!fstd::is_copy_constructible_v<fstd::unique_ptr<int>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::unique_ptr<int>>);
    STATIC_REQUIRE(!fstd::is_convertible_v<int*, fstd::unique_ptr<int>>);

    {
        auto owner = fstd::make_unique<Tracked>(3);
        REQUIRE((Tracked::alive == 1 && owner->value == 3 && (*owner).value == 3));
        Tracked* released = owner.release();
        REQUIRE((!owner && Tracked::alive == 1));
        owner.reset(released);
        owner.reset(new Tracked(4));
        REQUIRE((Tracked::alive == 1 && owner->value == 4));
        owner = nullptr;
        REQUIRE(Tracked::alive == 0);
        owner = fstd::make_unique<Tracked>(5);
    }
    REQUIRE(Tracked::alive == 0);

    fstd::unique_ptr<Base> base = fstd::make_unique<Derived>();
    REQUIRE(base->base_value == 1);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::unique_ptr<Base>, fstd::unique_ptr<Derived>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::unique_ptr<Derived>, fstd::unique_ptr<Base>>);

    auto overwritten = fstd::make_unique_for_overwrite<int>();
    *overwritten = 2;
    REQUIRE(*overwritten == 2);
}

TEST_CASE("unique_ptr calls its deleter, held by value or by reference", "[libcxx]")
{
    STATIC_REQUIRE(!fstd::is_default_constructible_v<fstd::unique_ptr<int, void (*)(int*)>>);
    int calls = 0;
    {
        fstd::unique_ptr<int, CountingDeleter> owner(new int(1), CountingDeleter{&calls});
        owner.reset(new int(2));
        REQUIRE(calls == 1);
    }
    REQUIRE(calls == 2);

    CountingDeleter shared{&calls};
    {
        fstd::unique_ptr<int, CountingDeleter&> owner(new int(3), shared);
        REQUIRE(&owner.get_deleter() == &shared);
        STATIC_REQUIRE(
            !fstd::is_constructible_v<fstd::unique_ptr<int, CountingDeleter&>, int*, CountingDeleter>);
        fstd::unique_ptr<int, CountingDeleter> copied_deleter(fstd::move(owner));
        REQUIRE(copied_deleter.get_deleter().calls == &calls);
    }
    REQUIRE(calls == 3);

    int closed = 0;
    {
        fstd::unique_ptr<int, HandleDeleter> handle(Handle(42), HandleDeleter{&closed});
        STATIC_REQUIRE(fstd::is_same_v<fstd::unique_ptr<int, HandleDeleter>::pointer, Handle>);
        REQUIRE(handle.get().id == 42);
    }
    REQUIRE(closed == 42);
}

TEST_CASE("unique_ptr of an array", "[libcxx]")
{
    {
        auto values = fstd::make_unique<Tracked[]>(3);
        REQUIRE((Tracked::alive == 3 && values[2].value == 0));
        values.reset();
        REQUIRE(Tracked::alive == 0);
    }
    auto numbers = fstd::make_unique<int[]>(2);
    REQUIRE((numbers[0] == 0 && numbers[1] == 0));
    fstd::unique_ptr<const int[]> constant(fstd::move(numbers));
    REQUIRE(constant[1] == 0);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::unique_ptr<Base[]>, Derived*>);
    STATIC_REQUIRE(fstd::is_constructible_v<fstd::unique_ptr<const int[]>, int*>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::unique_ptr<Base[]>, fstd::unique_ptr<Derived[]>>);
}

TEST_CASE("unique_ptr compares and hashes by address", "[libcxx]")
{
    int values[2] = {};
    fstd::unique_ptr<int, void (*)(int*)> first(&values[0], [](int*) {});
    fstd::unique_ptr<int, void (*)(int*)> second(&values[1], [](int*) {});
    REQUIRE((first < second && second > first && first <= first && first != second));
    REQUIRE((first <=> second) < 0);
    REQUIRE((first != nullptr && !(first == nullptr) && nullptr < first && first > nullptr));
    REQUIRE((first <=> nullptr) > 0);
    REQUIRE(fstd::hash<fstd::unique_ptr<int, void (*)(int*)>>()(first) ==
            fstd::hash<int*>()(&values[0]));
}

TEST_CASE("shared_ptr counts its owners", "[libcxx]")
{
    {
        fstd::shared_ptr<Tracked> first = fstd::make_shared<Tracked>(7);
        REQUIRE((Tracked::alive == 1 && first.use_count() == 1 && first->value == 7));
        fstd::shared_ptr<Tracked> second = first;
        REQUIRE((first.use_count() == 2 && second.get() == first.get()));
        fstd::shared_ptr<Tracked> third = fstd::move(second);
        REQUIRE((!second && second.use_count() == 0 && third.use_count() == 2));
        first.reset();
        REQUIRE((Tracked::alive == 1 && third.use_count() == 1));
        third = fstd::shared_ptr<Tracked>(new Tracked(8));
        REQUIRE((Tracked::alive == 1 && third->value == 8));
    }
    REQUIRE(Tracked::alive == 0);

    fstd::shared_ptr<Base> base = fstd::make_shared<Derived>();
    REQUIRE(base->base_value == 1);
    auto derived = fstd::static_pointer_cast<Derived>(base);
    REQUIRE((derived->derived_value == 2 && base.use_count() == 2));
    fstd::shared_ptr<const Derived> constant = derived;
    auto mutable_again = fstd::const_pointer_cast<Derived>(constant);
    REQUIRE(mutable_again.get() == derived.get());

    // Aliasing: owns the whole object, points at a member.
    fstd::shared_ptr<int> member(derived, &derived->derived_value);
    REQUIRE((*member == 2 && derived.use_count() == 5));

    STATIC_REQUIRE(!fstd::is_convertible_v<Base*, fstd::shared_ptr<Base>>);
    STATIC_REQUIRE(!fstd::is_constructible_v<fstd::shared_ptr<Derived>, fstd::shared_ptr<Base>>);
    STATIC_REQUIRE(fstd::is_nothrow_move_constructible_v<fstd::shared_ptr<int>>);
}

TEST_CASE("shared_ptr runs its deleter once and finds it with get_deleter", "[libcxx]")
{
    int calls = 0;
    {
        fstd::shared_ptr<int> first(new int(1), CountingDeleter{&calls});
        fstd::shared_ptr<int> second = first;
        REQUIRE(fstd::get_deleter<CountingDeleter>(second)->calls == &calls);
        REQUIRE(fstd::get_deleter<int>(second) == nullptr);
        first.reset();
        REQUIRE(calls == 0);
    }
    REQUIRE(calls == 1);

    bool null_deleted = false;
    {
        fstd::shared_ptr<int> null_owner(nullptr, [&null_deleted](nullptr_t) { null_deleted = true; });
        REQUIRE((!null_owner && null_owner.use_count() == 1));
    }
    REQUIRE(null_deleted);

    {
        fstd::shared_ptr<Tracked> from_unique(fstd::make_unique<Tracked>(2));
        REQUIRE((Tracked::alive == 1 && from_unique.use_count() == 1));
        fstd::unique_ptr<Tracked> empty;
        from_unique = fstd::move(empty);
        REQUIRE((Tracked::alive == 0 && !from_unique && from_unique.use_count() == 0));
    }

    {
        fstd::shared_ptr<Tracked[]> array(new Tracked[3]);
        REQUIRE((Tracked::alive == 3 && array[1].value == 0));
        fstd::shared_ptr<Tracked[2]> bounded(new Tracked[2]);
        REQUIRE(Tracked::alive == 5);
    }
    REQUIRE(Tracked::alive == 0);
}

TEST_CASE("weak_ptr observes without owning", "[libcxx]")
{
    fstd::weak_ptr<Tracked> observer;
    REQUIRE((observer.expired() && !observer.lock()));
    {
        auto owner = fstd::make_shared<Tracked>(4);
        observer = owner;
        REQUIRE((!observer.expired() && observer.use_count() == 1 && observer.lock()->value == 4));
        fstd::shared_ptr<Tracked> locked(observer);
        REQUIRE(owner.use_count() == 2);
    }
    REQUIRE((Tracked::alive == 0 && observer.expired() && !observer.lock()));

    // Converting an expired weak_ptr to a base must not read the object.
    fstd::weak_ptr<Derived> derived_observer;
    {
        auto owner = fstd::make_shared<Derived>();
        derived_observer = owner;
    }
    fstd::weak_ptr<Base> base_observer = derived_observer;
    REQUIRE(base_observer.expired());

    auto first = fstd::make_shared<int>(1);
    auto second = fstd::make_shared<int>(2);
    fstd::weak_ptr<int> weak_first = first;
    const fstd::owner_less<> before;
    REQUIRE((!before(first, weak_first) && !before(weak_first, first)));
    REQUIRE(before(first, second) != before(second, first));
}

TEST_CASE("enable_shared_from_this finds the object's owner", "[libcxx]")
{
    auto node = fstd::make_shared<Node>();
    fstd::shared_ptr<Node> again = node->shared_from_this();
    REQUIRE((again == node && node.use_count() == 2));
    REQUIRE(node->weak_from_this().lock() == node);
    const Node& constant = *node;
    REQUIRE(constant.shared_from_this()->value == 5);

    fstd::shared_ptr<Node> adopted(new Node);
    REQUIRE(adopted->shared_from_this() == adopted);

    Node unowned;
    REQUIRE(unowned.weak_from_this().expired());
}

TEST_CASE("shared_ptr compares and hashes by address", "[libcxx]")
{
    auto first = fstd::make_shared<int>(1);
    fstd::shared_ptr<const int> same = first;
    REQUIRE((first == same && !(first != same) && (first <=> same) == 0));
    fstd::shared_ptr<int> empty;
    REQUIRE((empty == nullptr && first != nullptr && (first <=> nullptr) > 0));
    REQUIRE(fstd::hash<fstd::shared_ptr<int>>()(first) == fstd::hash<int*>()(first.get()));
    fstd::shared_ptr<int> other = fstd::make_shared<int>(1);
    swap(first, other);
    REQUIRE(*other == 1);
}

TEST_CASE("the uninitialized-memory algorithms construct and destroy in place", "[libcxx]")
{
    alignas(Tracked) unsigned char storage[4 * sizeof(Tracked)];
    auto* elements = reinterpret_cast<Tracked*>(storage);
    const Tracked source[2] = {Tracked(1), Tracked(2)};
    REQUIRE(Tracked::alive == 2);

    REQUIRE(fstd::uninitialized_copy(source, source + 2, elements) == elements + 2);
    REQUIRE(fstd::uninitialized_fill_n(elements + 2, 2, Tracked(7)) == elements + 4);
    REQUIRE((Tracked::alive == 6 && elements[1].value == 2 && elements[3].value == 7));
    fstd::destroy(elements, elements + 2);
    REQUIRE(fstd::destroy_n(elements + 2, 2) == elements + 4);
    REQUIRE(Tracked::alive == 2);

    fstd::uninitialized_copy_n(source, 2, elements);
    fstd::uninitialized_move(elements, elements + 2, elements + 2);
    REQUIRE((Tracked::alive == 6 && elements[3].value == 2));
    fstd::destroy(elements, elements + 4);
    fstd::uninitialized_fill(elements, elements + 3, Tracked(4));
    const auto [read, written] = fstd::uninitialized_move_n(elements, 1, elements + 3);
    REQUIRE((read == elements + 1 && written == elements + 4 && elements[3].value == 4));
    fstd::destroy(elements, elements + 4);
    REQUIRE(Tracked::alive == 2);

    int numbers[3] = {5, 5, 5};
    fstd::uninitialized_value_construct(numbers, numbers + 2);
    REQUIRE((numbers[0] == 0 && numbers[1] == 0 && numbers[2] == 5));
    REQUIRE(fstd::uninitialized_value_construct_n(numbers, 3) == numbers + 3);
    REQUIRE(numbers[2] == 0);
    fstd::uninitialized_default_construct(elements, elements + 2);
    REQUIRE(fstd::uninitialized_default_construct_n(elements + 2, 2) == elements + 4);
    REQUIRE(Tracked::alive == 6);
    fstd::destroy(elements, elements + 4);
}
