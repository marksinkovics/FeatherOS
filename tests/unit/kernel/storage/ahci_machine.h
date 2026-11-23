#ifndef FEATHEROS_TESTS_STORAGE_AHCI_MACHINE_H
#define FEATHEROS_TESTS_STORAGE_AHCI_MACHINE_H

#include "fake_ahci.h"
#include "storage_rig.h"

#include "support/scheduler_stubs.h"

#include "kernel/log_ring.h"
#include "kernel/memory/dma.h"
#include "kernel/storage/ahci/hba.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/write_policy.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AhciTest
{

namespace S = FeatherOS::Storage;
namespace A = FeatherOS::Storage::Ahci;
using FeatherDisk::Error;
using S::Testing::FakeAhci;
using S::Testing::PumpEnvironment;
using Event = FakeAhci::Event;

inline std::string LogText()
{
    static char text[FeatherOS::KernelLog::Capacity];
    return std::string(text, FeatherOS::KernelLog::Snapshot(text, sizeof(text)));
}

inline size_t CountIn(const std::string& all, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = all.find(needle); at != std::string::npos; at = all.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

inline std::vector<std::string> scanned;
inline std::vector<std::string> removed;

inline void OnScan(S::Disk& disk)
{
    scanned.emplace_back(disk.Name());
}

inline void OnRemove(S::Disk& disk)
{
    removed.emplace_back(disk.Name());
}

inline std::vector<uint8_t> Pattern(uint32_t seed, size_t bytes)
{
    std::vector<uint8_t> data(bytes);
    uint32_t state = seed * 2654435761U + 1;
    for (uint8_t& byte : data)
    {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>(state >> 24);
    }
    return data;
}

// One HBA on its own fake, a clock that only waits move, and the registry cleaned up at the end,
// so the next test may reuse the names.
class Machine
{
public:
    Machine() : env([this] { fake.Pump(); }), log_before(LogText())
    {
        S::Initialize();
        S::SetScanHandler(OnScan);
        S::SetRemovalHandler(OnRemove);
        scanned.clear();
        removed.clear();
        index = next_index++;
    }

    ~Machine()
    {
        RemoveDisks();
        hba.reset();
        S::SetScanHandler(nullptr);
        S::SetRemovalHandler(nullptr);
    }

    void RemoveDisks()
    {
        for (const std::string& name : scanned)
        {
            if (S::Disk* disk = S::Acquire(name.c_str()))
            {
                S::Remove(*disk);
                disk->Release();
            }
        }
    }

    // How many times a line appeared since this machine was made: the kernel log is shared.
    size_t Lines(const std::string& needle) const
    {
        return CountIn(LogText(), needle) - CountIn(log_before, needle);
    }

    bool Start(S::WritePolicy policy = S::WritePolicy::Allowed)
    {
        hba.reset(A::Hba::Create(fake, env, index, policy, "00:05.0"));
        return hba->Start();
    }

    S::Disk* Open(size_t ordinal = 0)
    {
        return ordinal < scanned.size() ? S::Acquire(scanned[ordinal].c_str()) : nullptr;
    }

    std::string Line(const std::string& text) const
    {
        return "Storage: ahci" + std::to_string(index) + ": " + text;
    }

    uint64_t Elapsed()
    {
        return env.NowMilliseconds() - start_time;
    }

    FeatherOS::Testing::PhysicalMemory memory;
    FakeAhci fake{memory};
    PumpEnvironment env;
    std::unique_ptr<A::Hba> hba;
    std::string log_before;
    uint16_t index = 0;
    uint64_t start_time = 1'000'000;
    static inline uint16_t next_index = 0;
};

inline std::vector<FakeAhci::Record> Of(const FakeAhci& fake, std::initializer_list<Event> kinds)
{
    std::vector<FakeAhci::Record> out;
    for (const FakeAhci::Record& record : fake.Log())
    {
        if (std::find(kinds.begin(), kinds.end(), record.event) != kinds.end())
        {
            out.push_back(record);
        }
    }
    return out;
}

} // namespace AhciTest

#endif
