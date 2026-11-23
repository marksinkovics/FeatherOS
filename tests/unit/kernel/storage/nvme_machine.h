#ifndef FEATHEROS_TESTS_STORAGE_NVME_MACHINE_H
#define FEATHEROS_TESTS_STORAGE_NVME_MACHINE_H

#include "fake_nvme.h"
#include "storage_rig.h"

#include "support/scheduler_stubs.h"

#include "kernel/log_ring.h"
#include "kernel/memory/dma.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/nvme/controller.h"
#include "kernel/storage/write_policy.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NvmeTest
{


namespace S = FeatherOS::Storage;
namespace N = FeatherOS::Storage::Nvme;
using FeatherDisk::Error;
using S::Testing::FakeNvme;
using S::Testing::PumpEnvironment;
using Event = FakeNvme::Event;

inline size_t LogLines(const std::string& needle)
{
    static char text[FeatherOS::KernelLog::Capacity];
    const size_t length = FeatherOS::KernelLog::Snapshot(text, sizeof(text));
    const std::string all(text, length);
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

// One controller on its own fake, a clock that only waits move, and the registry cleaned up at
// the end, so the next test may reuse the names.
class Machine
{
public:
    Machine() : env([this] { fake.Pump(); })
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
        if (controller)
        {
            for (uint32_t i = 0; i < controller->NamespaceCount(); i++)
            {
                if (S::Disk* disk = S::Acquire(DiskName(controller->NamespaceAt(i).Id()).c_str()))
                {
                    S::Remove(*disk);
                    disk->Release();
                }
            }
        }
        controller.reset();
        S::SetScanHandler(nullptr);
        S::SetRemovalHandler(nullptr);
    }

    bool Start(S::WritePolicy policy = S::WritePolicy::Allowed)
    {
        controller.reset(N::Controller::Create(fake, env, index, policy, "00:04.0"));
        return controller->Start();
    }

    std::string DiskName(uint32_t nsid) const
    {
        return "nvme" + std::to_string(index) + "n" + std::to_string(nsid);
    }

    S::Disk* Open(uint32_t nsid = 1)
    {
        return S::Acquire(DiskName(nsid).c_str());
    }

    std::string Line(const std::string& text) const
    {
        return "Storage: nvme" + std::to_string(index) + ": " + text;
    }

    uint64_t Elapsed()
    {
        return env.NowMilliseconds() - start_time;
    }

    FeatherOS::Testing::PhysicalMemory memory;
    FakeNvme fake{memory};
    PumpEnvironment env;
    std::unique_ptr<N::Controller> controller;
    uint16_t index = 0;
    uint64_t start_time = 1'000'000;
    static inline uint16_t next_index = 0;
};

inline bool Contains(const std::vector<Event>& events, Event event)
{
    return std::find(events.begin(), events.end(), event) != events.end();
}

inline size_t Position(const std::vector<Event>& events, Event event, size_t from = 0)
{
    const auto it = std::find(events.begin() + static_cast<std::ptrdiff_t>(from), events.end(), event);
    return static_cast<size_t>(it - events.begin());
}

} // namespace NvmeTest

#endif
