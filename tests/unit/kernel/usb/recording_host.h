#ifndef FEATHEROS_TESTS_USB_RECORDING_HOST_H
#define FEATHEROS_TESTS_USB_RECORDING_HOST_H

#include "kernel/usb/host_controller.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// Passes every call to another controller and writes the ones that change an endpoint, and the
// control requests, into a log, such as "cancel 81" or "control 21 ff".
class RecordingHost final : public HostController
{
public:
    explicit RecordingHost(HostController& inner);

    std::vector<std::string> Log() const;
    void Clear();

    uint32_t RootPorts() const override;
    void ReadRootPort(uint32_t port, RootPortStatus* status, bool acknowledge) override;
    bool ResetRootPort(uint32_t port, Speed* speed) override;
    CreateStatus CreateDevice(const Location& location, uint32_t* slot) override;
    bool SetControlMaxPacket(uint32_t slot, uint32_t max_packet) override;
    bool ConfigureEndpoints(uint32_t slot, std::span<const EndpointDescriptor> endpoints,
                            const HubInfo* hub) override;
    void DestroyDevice(uint32_t slot) override;
    Status Control(uint32_t slot, const SetupPacket& setup, Memory::Dma::Buffer& buffer,
                   uint32_t* actual) override;
    bool Submit(uint32_t slot, Transfer& transfer) override;
    bool Wait(Transfer& transfer, uint64_t deadline_milliseconds) override;
    bool ResetEndpoint(uint32_t slot, uint8_t endpoint) override;
    bool CancelEndpoint(uint32_t slot, uint8_t endpoint) override;
    bool ClearHalt(uint32_t slot, uint8_t endpoint) override;
    void StopEndpoints(uint32_t slot) override;
    bool AllocateBuffer(uint64_t bytes, Memory::Dma::Buffer* buffer) override;
    void FreeBuffer(const Memory::Dma::Buffer& buffer) override;
    void Delay(uint32_t milliseconds) override;
    uint64_t NowMilliseconds() override;
    bool HasWork() const override;
    bool TakePortChange(uint32_t* port) override;
    bool Recover() override;
    void ServiceEndpoints() override;
    void SetRecoveryHandler(RecoveryHandler handler, void* context) override;

private:
    void Record(const std::string& line);

    HostController& inner_;
    mutable std::mutex mutex_;
    std::vector<std::string> log_;
};

} // namespace FeatherOS::Usb::Testing

#endif
