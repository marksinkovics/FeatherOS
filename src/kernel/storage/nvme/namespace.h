#ifndef FEATHEROS_STORAGE_NVME_NAMESPACE_H
#define FEATHEROS_STORAGE_NVME_NAMESPACE_H

#include "kernel/storage/disk_driver.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

class Controller;

// One active namespace. Info is cached, so it takes no lock and does no I/O.
class Namespace final : public DiskDriver
{
public:
    void Initialize(Controller& controller, const DiskInfo& info);

    uint32_t Id() const;
    uint32_t SectorSize() const;
    uint64_t SectorCount() const;
    bool ReadOnly() const;
    // A reset found this namespace changed or gone: only its disk goes, the controller lives on.
    void MarkGone();
    bool Vanished() const;
    const Controller& Owner() const;

    DiskInfo Info() override;
    bool Gone() override;
    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override;
    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override;
    FeatherDisk::Status Flush() override;
    FeatherDisk::Status Stop() override;
    void Release() override;

private:
    Controller* controller_ = nullptr;
    DiskInfo info_{};
    std::atomic<bool> vanished_ = false;
};

} // namespace FeatherOS::Storage::Nvme

#endif
