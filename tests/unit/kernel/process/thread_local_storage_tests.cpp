#include <catch2/catch_test_macros.hpp>

#include "kernel/thread_local_storage.h"

using FeatherOS::ThreadLocalStorage::Layout;
using FeatherOS::ThreadLocalStorage::Place;
using FeatherOS::ThreadLocalStorage::Variant;

TEST_CASE("x86-64 TLS ends at the thread pointer, below its self pointer")
{
    Layout layout{};
    REQUIRE(Place(Variant::BlockBelowPointer, 0x10000, 20, 8, &layout));
    REQUIRE(layout.thread_pointer == 0x10000 - 16);
    REQUIRE(layout.block == layout.thread_pointer - 24);
    REQUIRE(layout.bottom % 16 == 0);
    REQUIRE(layout.bottom <= layout.block);
    // A larger alignment aligns the pointer and so the block.
    REQUIRE(Place(Variant::BlockBelowPointer, 0x10000, 100, 64, &layout));
    REQUIRE(layout.thread_pointer % 64 == 0);
    REQUIRE(layout.block % 64 == 0);
    REQUIRE(layout.thread_pointer - layout.block == 128);
    REQUIRE(layout.thread_pointer + 16 <= 0x10000);
    // A program without TLS still gets its control block.
    REQUIRE(Place(Variant::BlockBelowPointer, 0x10000, 0, 1, &layout));
    REQUIRE(layout.block == layout.thread_pointer);
}

TEST_CASE("AArch64 TLS starts 16 bytes past the thread pointer")
{
    Layout layout{};
    REQUIRE(Place(Variant::BlockAbovePointer, 0x10000, 20, 8, &layout));
    REQUIRE(layout.block == layout.thread_pointer + 16);
    REQUIRE(layout.block + 20 <= 0x10000);
    REQUIRE(layout.bottom == layout.thread_pointer);
    REQUIRE(layout.thread_pointer % 16 == 0);
    REQUIRE(Place(Variant::BlockAbovePointer, 0x10000, 100, 64, &layout));
    REQUIRE(layout.block == layout.thread_pointer + 64);
    REQUIRE(layout.block % 64 == 0);
    REQUIRE(layout.block + 100 <= 0x10000);
}

TEST_CASE("a TLS area that does not fit or a bad alignment is refused")
{
    Layout layout{};
    REQUIRE_FALSE(Place(Variant::BlockBelowPointer, 64, 100, 8, &layout));
    REQUIRE_FALSE(Place(Variant::BlockAbovePointer, 0x10000, 8, 3, &layout));
    REQUIRE_FALSE(Place(Variant::BlockAbovePointer, 0x10000, 8, 0, &layout));
}
