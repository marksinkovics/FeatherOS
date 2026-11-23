#ifndef FEATHEROS_TESTS_STORAGE_FAKE_AHCI_H
#define FEATHEROS_TESTS_STORAGE_FAKE_AHCI_H

#include "model_disk.h"

#include "kernel/memory/dma.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/support/physical_memory.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FeatherOS::Storage::Testing
{

// An AHCI HBA at register level: it executes the command in slot 0 of the command list a port
// points at, updates PxCI, PxIS, PxTFD and PRDBC, and keeps a list of faults, the ways a driver can
// break the specification that real hardware would answer with corruption or a hang.
class FakeAhci final : public ControllerHardware
{
public:
    struct Options
    {
        uint32_t slots = 32;
        bool s64a = true;
        bool sss = false;
        bool boh = false;
        uint32_t version = 0x00010301;
        InterruptKind interrupts = InterruptKind::MsiX;
        uint16_t subsystem_vendor = 0x1AF4;
        uint16_t subsystem = 0x1100;
        // BOHC.BOS is set at the start; it drops that many BOHC reads after BOHC.OOS is set.
        bool firmware_owns = false;
        uint32_t bios_release_reads = 0;
        bool bios_never_releases = false;
        // BOHC.BB stays set for these reads after the release.
        uint32_t bios_busy_reads = 0;
        bool bios_stays_busy = false;
        bool hr_never_clears = false;
        uint32_t hr_delay_reads = 0;
        uint32_t engine_stop_delay_reads = 0;
        bool engine_never_stops = false;
        bool defer_processing = false;
        bool drop_interrupts = false;
        uint32_t allocations_before_failure = ~0U;
    };

    enum class Device : uint8_t
    {
        None,
        Ata,
        Atapi,
        PortMultiplier,
        Semb,
    };

    struct PortOptions
    {
        Device device = Device::Ata;
        uint32_t sector_size = 512;
        uint64_t sectors = 2048;
        bool write_cache_supported = true;
        bool write_cache_enabled = true;
        bool flush_ext = true;
        bool flush = true;
        bool lba48 = true;
        // PxTFD shows BSY for this many reads after the link comes up.
        uint32_t busy_reads = 0;
        bool busy_forever = false;
        // PxSSTS.DET reads 1 until a COMRESET.
        bool det1_until_comreset = false;
        // PxSSTS reads before the link is up after reset or SUD.
        uint32_t link_delay_reads = 0;
        // Firmware left the engine running with a command list of its own.
        bool engine_running = false;
        std::string model = "FAKE SATA DISK";
        std::string serial = "FEATHER0001";
    };

    enum class Failure : uint8_t
    {
        // TFES with status 51h, error 40h.
        TaskFile,
        // The command is fetched and never completes.
        Hang,
        // A read completes with half the bytes in PRDBC.
        ShortRead,
    };

    enum class Event : uint8_t
    {
        HandoffRequested,
        HbaReset,
        SpinUp,
        LinkUp,
        Comreset,
        EngineStarted,
        EngineStopped,
        Identify,
        Read,
        Write,
        Flush,
        FlushExt,
        Standby,
    };

    struct Record
    {
        Event event;
        uint32_t port;
        uint64_t lba;
        uint32_t sectors;
    };

    explicit FakeAhci(FeatherOS::Testing::PhysicalMemory& memory);

    ModelDisk& AddPort(uint32_t number, const PortOptions& options);
    ModelDisk& AddPort(uint32_t number);
    ModelDisk& PortDisk(uint32_t number);

    uint32_t Read32(uint32_t offset) override;
    void Write32(uint32_t offset, uint32_t value) override;
    uint64_t Read64(uint32_t offset) override;
    void Write64(uint32_t offset, uint64_t value) override;
    uint32_t ReadConfig32(uint16_t offset) override;
    bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) override;
    void FreeDma(const Memory::Dma::Buffer& buffer) override;
    InterruptKind SetUpInterrupts(void (*handler)(void* context), void* context) override;

    // With deferred processing the environment's pump runs the commands here.
    void Pump();
    void Inject(uint32_t port, Failure failure, uint32_t count = 1);

    uint32_t Count(Event event) const;
    uint32_t Count(Event event, uint32_t port) const;
    const std::vector<Record>& Log() const;
    const std::vector<std::string>& Faults() const;
    const std::vector<uint64_t>& Limits() const;
    uint32_t Interrupts() const;
    uint32_t LiveBuffers() const;
    bool EngineRunning(uint32_t port) const;
    bool ReceivingFis(uint32_t port) const;
    uint32_t Resets() const;

    Options options;

private:
    static constexpr uint32_t PortCount = 32;

    enum class Link : uint8_t
    {
        Down,
        Pending,
        Comm,
        NoComm,
    };

    struct PortState
    {
        bool present = false;
        PortOptions config;
        std::unique_ptr<ModelDisk> disk;
        uint64_t clb = 0;
        uint64_t fb = 0;
        uint32_t is = 0;
        uint32_t ie = 0;
        uint32_t cmd = 0;
        uint32_t serr = 0;
        uint32_t sctl = 0;
        uint32_t sact = 0;
        uint32_t ci = 0;
        bool cr = false;
        bool fr = false;
        bool cr_stopping = false;
        bool fr_stopping = false;
        uint32_t cr_reads = 0;
        uint32_t fr_reads = 0;
        Link link = Link::Down;
        uint32_t link_reads = 0;
        uint32_t busy_left = 0;
        uint32_t tfd_status = 0x50;
        uint32_t tfd_error = 0;
        Failure failure = Failure::TaskFile;
        uint32_t failures = 0;
        bool hanging = false;
        std::vector<uint64_t> dma_bases;
    };

    struct Segment
    {
        volatile uint8_t* host;
        uint32_t length;
    };

    class Scope;
    friend class Scope;

    void Fault(const std::string& what);
    void Initialize();
    void ResetPort(uint32_t number);
    void ResetHba();
    void BeginLink(uint32_t number);
    uint32_t ReadGhc();
    uint32_t ReadBohc();
    uint32_t ReadPort(uint32_t number, uint32_t offset);
    uint32_t ReadCmd(PortState& port);
    uint32_t ReadSsts(uint32_t number, PortState& port);
    uint32_t ReadTfd(PortState& port);
    uint32_t ReadSignature(const PortState& port) const;
    uint32_t ReadIs() const;
    void WriteGhc(uint32_t value);
    void WritePort(uint32_t number, uint32_t offset, uint32_t value);
    void WriteCmd(uint32_t number, PortState& port, uint32_t value);
    void WriteSctl(uint32_t number, PortState& port, uint32_t value);
    void WriteCi(uint32_t number, PortState& port, uint32_t value);
    void WriteAddress(PortState& port, uint32_t offset, uint32_t value);
    void Execute(uint32_t number, PortState& port);
    void Complete(PortState& port, uint64_t prdbc_physical, uint32_t bytes, bool pio);
    void Error(uint32_t number, PortState& port, uint32_t status, uint32_t error);
    bool GatherPrds(PortState& port, volatile uint8_t* table, uint32_t count, uint32_t needed,
                    std::vector<Segment>* segments);
    std::array<uint16_t, 256> IdentifyWords(const PortState& port) const;
    volatile uint8_t* Host(uint64_t physical, uint64_t length) const;
    bool Running(const PortState& port) const;
    void RaiseIfEnabled(const PortState& port);
    uint32_t Implemented() const;

    FeatherOS::Testing::PhysicalMemory& memory_;
    FeatherOS::Testing::PhysicalMemory low_{0x4000'0000};
    std::array<PortState, PortCount> ports_;
    std::vector<Record> log_;
    std::vector<std::string> faults_;
    std::vector<uint64_t> limits_;
    std::vector<Memory::Dma::Buffer> live_;
    uint32_t allocations_ = 0;
    std::recursive_mutex lock_;
    uint32_t depth_ = 0;
    uint32_t interrupts_due_ = 0;
    uint32_t interrupts_ = 0;
    void (*handler_)(void*) = nullptr;
    void* handler_context_ = nullptr;

    bool initialized_ = false;
    uint32_t ghc_ = 0;
    uint32_t resets_ = 0;
    bool hr_pending_ = false;
    uint32_t hr_reads_ = 0;
    bool os_owned_ = false;
    bool bios_owned_ = false;
    bool bios_busy_ = false;
    uint32_t bios_reads_ = 0;
    uint32_t busy_reads_ = 0;
    bool pending_commands_ = false;
};

} // namespace FeatherOS::Storage::Testing

#endif
