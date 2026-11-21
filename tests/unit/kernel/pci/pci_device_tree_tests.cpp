#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/aarch64/pci_device_tree.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{

using namespace FeatherOS::Arch::AArch64::PciDeviceTree;

class TreeBuilder
{
public:
    void Begin(const char* name)
    {
        Word(1);
        Text(name);
    }

    void End()
    {
        Word(2);
    }

    void Cells(const char* name, const std::vector<uint32_t>& cells)
    {
        Header(name, static_cast<uint32_t>(cells.size() * 4));
        for (uint32_t cell : cells)
        {
            Word(cell);
        }
    }

    void Flag(const char* name)
    {
        Header(name, 0);
    }

    void String(const char* name, const char* value)
    {
        Header(name, static_cast<uint32_t>(std::strlen(value) + 1));
        Text(value);
    }

    std::vector<uint8_t> Finish()
    {
        Word(9);
        constexpr uint32_t HeaderSize = 56;
        const auto structure_size = static_cast<uint32_t>(structure.size());
        const auto strings_size = static_cast<uint32_t>(strings.size());
        std::vector<uint8_t> blob;
        auto put = [&blob](uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                blob.push_back(static_cast<uint8_t>(value >> shift));
            }
        };
        put(0xD00DFEED);
        put(HeaderSize + structure_size + strings_size);
        put(HeaderSize);
        put(HeaderSize + structure_size);
        put(40);
        put(17);
        put(16);
        put(0);
        put(strings_size);
        put(structure_size);
        for (int index = 0; index < 4; ++index)
        {
            put(0);
        }
        blob.insert(blob.end(), structure.begin(), structure.end());
        blob.insert(blob.end(), strings.begin(), strings.end());
        return blob;
    }

private:
    void Word(uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            structure.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    void Text(const char* text)
    {
        structure.insert(structure.end(), text, text + std::strlen(text) + 1);
        while (structure.size() % 4 != 0)
        {
            structure.push_back(0);
        }
    }

    void Header(const char* name, uint32_t length)
    {
        auto found = name_offsets.find(name);
        if (found == name_offsets.end())
        {
            found = name_offsets.emplace(name, static_cast<uint32_t>(strings.size())).first;
            strings.append(name, std::strlen(name) + 1);
        }
        Word(3);
        Word(length);
        Word(found->second);
    }

    std::vector<uint8_t> structure;
    std::string strings;
    std::map<std::string, uint32_t> name_offsets;
};

struct Options
{
    bool coherent = true;
    bool disabled = false;
    uint32_t map_phandle = 1;
    uint32_t ecam_size = 0x1000000;
    bool controller_first = false;
    bool truncate_map = false;
};

void AddController(TreeBuilder& tree)
{
    tree.Begin("intc@8000000");
    tree.Cells("phandle", {1});
    tree.Cells("#address-cells", {2});
    tree.Cells("#size-cells", {2});
    tree.Cells("#interrupt-cells", {3});
    tree.Flag("interrupt-controller");
    tree.End();
}

std::vector<uint8_t> VirtTree(const Options& options = {})
{
    TreeBuilder tree;
    tree.Begin("");
    tree.Cells("#address-cells", {2});
    tree.Cells("#size-cells", {2});
    if (options.controller_first)
    {
        AddController(tree);
    }

    tree.Begin("pcie@3f000000");
    tree.String("compatible", "pci-host-ecam-generic");
    tree.Cells("#address-cells", {3});
    tree.Cells("#size-cells", {2});
    tree.Cells("#interrupt-cells", {1});
    tree.Cells("reg", {0, 0x3F000000, 0, options.ecam_size});
    tree.Cells("bus-range", {0, 0x0F});
    tree.Cells("ranges", {0x01000000, 0, 0, 0, 0x3EFF0000, 0, 0x10000,  //
                          0x02000000, 0, 0x10000000, 0, 0x10000000, 0, 0x2EFF0000,  //
                          0x43000000, 0x80, 0, 0x80, 0, 0x80, 0});
    tree.Cells("interrupt-map-mask", {0x1800, 0, 0, 7});
    std::vector<uint32_t> map;
    for (uint32_t slot = 0; slot < 4; ++slot)
    {
        for (uint32_t pin = 1; pin <= 4; ++pin)
        {
            const std::vector<uint32_t> entry = {slot << 11, 0, 0, pin, options.map_phandle,
                                                 0, 0, 0, 3 + (slot + pin - 1) % 4, 4};
            map.insert(map.end(), entry.begin(), entry.end());
        }
    }
    if (options.truncate_map)
    {
        map.resize(map.size() - 3);
    }
    tree.Cells("interrupt-map", map);
    tree.Cells("msi-parent", {2});
    if (options.coherent)
    {
        tree.Flag("dma-coherent");
    }
    if (options.disabled)
    {
        tree.String("status", "disabled");
    }
    tree.End();

    if (!options.controller_first)
    {
        AddController(tree);
    }
    tree.End();
    return tree.Finish();
}

bool Find(const std::vector<uint8_t>& blob, HostBridge* bridge)
{
    return FindHostBridge(blob.data(), bridge);
}

} // namespace

TEST_CASE("PCI device tree: the host bridge node is read like QEMU virt's")
{
    for (const bool controller_first : {false, true})
    {
        Options options;
        options.controller_first = controller_first;
        const auto blob = VirtTree(options);
        HostBridge bridge;
        REQUIRE(Find(blob, &bridge));
        CHECK(bridge.ecam_base == 0x3F000000);
        CHECK(bridge.ecam_size == 0x1000000);
        CHECK(bridge.first_bus == 0);
        CHECK(bridge.last_bus == 15);
        CHECK(bridge.dma_coherent);
        CHECK(bridge.msi_phandle == 2);
        REQUIRE(bridge.has_io);
        CHECK(bridge.io.cpu_base == 0x3EFF0000);
        CHECK(bridge.io.size == 0x10000);
        REQUIRE(bridge.has_memory32);
        CHECK(bridge.memory32.pci_base == 0x10000000);
        CHECK(bridge.memory32.cpu_base == 0x10000000);
        CHECK(bridge.memory32.size == 0x2EFF0000);
        CHECK_FALSE(bridge.memory32.prefetchable);
        REQUIRE(bridge.has_memory64);
        CHECK(bridge.memory64.pci_base == 0x8000000000ULL);
        CHECK(bridge.memory64.prefetchable);
        CHECK(bridge.interrupt_map_count == 16);
        CHECK(bridge.interrupt_mask[0] == 0x1800);
        CHECK(bridge.interrupt_mask[3] == 7);
    }
}

TEST_CASE("PCI device tree: the interrupt map is looked up with its mask")
{
    const auto blob = VirtTree();
    HostBridge bridge;
    REQUIRE(Find(blob, &bridge));
    uint32_t id = 0;

    REQUIRE(LookupInterrupt(bridge, 0, 0, 0, 1, &id));
    CHECK(id == 32 + 3);
    REQUIRE(LookupInterrupt(bridge, 0, 2, 0, 1, &id));
    CHECK(id == 32 + 5);
    REQUIRE(LookupInterrupt(bridge, 0, 3, 0, 4, &id));
    CHECK(id == 32 + 3 + (3 + 4 - 1) % 4);

    // The mask keeps only the device number bits 11-12, so slot 4 aliases slot 0 and the
    // function does not matter.
    REQUIRE(LookupInterrupt(bridge, 0, 4, 3, 2, &id));
    CHECK(id == 32 + 4);

    CHECK_FALSE(LookupInterrupt(bridge, 0, 0, 0, 0, &id));
    CHECK_FALSE(LookupInterrupt(bridge, 0, 0, 0, 5, &id));
}

TEST_CASE("PCI device tree: refusals")
{
    HostBridge bridge;

    Options not_coherent;
    not_coherent.coherent = false;
    REQUIRE(Find(VirtTree(not_coherent), &bridge));
    CHECK_FALSE(bridge.dma_coherent);

    Options disabled;
    disabled.disabled = true;
    CHECK_FALSE(Find(VirtTree(disabled), &bridge));

    Options unknown_parent;
    unknown_parent.map_phandle = 9;
    CHECK_FALSE(Find(VirtTree(unknown_parent), &bridge));

    Options truncated;
    truncated.truncate_map = true;
    CHECK_FALSE(Find(VirtTree(truncated), &bridge));

    Options small;
    small.ecam_size = 0x400000;
    REQUIRE(Find(VirtTree(small), &bridge));
    CHECK(bridge.last_bus == 3);

    TreeBuilder empty;
    empty.Begin("");
    empty.End();
    CHECK_FALSE(Find(empty.Finish(), &bridge));

    const uint8_t garbage[64] = {};
    CHECK_FALSE(FindHostBridge(garbage, &bridge));
    CHECK_FALSE(FindHostBridge(nullptr, &bridge));
}

TEST_CASE("PCI device tree: the BAR window hands out aligned, non-overlapping addresses")
{
    Window window = {0x10000000, 0x10000000, 0x100000, false, 0x10000000};
    uint64_t first;
    uint64_t second;
    uint64_t third;
    REQUIRE(AllocateFromWindow(window, 0x1000, &first));
    REQUIRE(AllocateFromWindow(window, 0x10000, &second));
    REQUIRE(AllocateFromWindow(window, 0x1000, &third));
    CHECK(first == 0x10000000);
    CHECK(second == 0x10010000);
    CHECK(third == 0x10020000);

    uint64_t last;
    CHECK_FALSE(AllocateFromWindow(window, 0x100000, &last));
    CHECK_FALSE(AllocateFromWindow(window, 0x3000, &last));
    CHECK_FALSE(AllocateFromWindow(window, 0, &last));
    REQUIRE(AllocateFromWindow(window, 0x20000, &last));
    CHECK(last == 0x10040000);
}
