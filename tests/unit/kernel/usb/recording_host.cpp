#include "recording_host.h"

#include <cstdio>

namespace FeatherOS::Usb::Testing
{
namespace
{

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%02x", value);
    return text;
}

} // namespace

RecordingHost::RecordingHost(HostController& inner) : inner_(inner) {}

std::vector<std::string> RecordingHost::Log() const
{
    const std::lock_guard guard(mutex_);
    return log_;
}

void RecordingHost::Clear()
{
    const std::lock_guard guard(mutex_);
    log_.clear();
}

void RecordingHost::Record(const std::string& line)
{
    const std::lock_guard guard(mutex_);
    log_.push_back(line);
}

uint32_t RecordingHost::RootPorts() const
{
    return inner_.RootPorts();
}

void RecordingHost::ReadRootPort(uint32_t port, RootPortStatus* status, bool acknowledge)
{
    inner_.ReadRootPort(port, status, acknowledge);
}

bool RecordingHost::ResetRootPort(uint32_t port, Speed* speed, bool force)
{
    return inner_.ResetRootPort(port, speed, force);
}

CreateStatus RecordingHost::CreateDevice(const Location& location, uint32_t* slot)
{
    return inner_.CreateDevice(location, slot);
}

bool RecordingHost::SetControlMaxPacket(uint32_t slot, uint32_t max_packet)
{
    return inner_.SetControlMaxPacket(slot, max_packet);
}

bool RecordingHost::ConfigureEndpoints(uint32_t slot, std::span<const EndpointDescriptor> endpoints,
                                       const HubInfo* hub)
{
    return inner_.ConfigureEndpoints(slot, endpoints, hub);
}

void RecordingHost::DestroyDevice(uint32_t slot)
{
    Record("destroy");
    inner_.DestroyDevice(slot);
}

Status RecordingHost::Control(uint32_t slot, const SetupPacket& setup, Memory::Dma::Buffer& buffer,
                              uint32_t* actual)
{
    Record("control " + Hex(setup.request_type) + " " + Hex(setup.request) + " " +
           Hex(setup.value) + " " + Hex(setup.index));
    return inner_.Control(slot, setup, buffer, actual);
}

bool RecordingHost::Submit(uint32_t slot, Transfer& transfer)
{
    return inner_.Submit(slot, transfer);
}

bool RecordingHost::Wait(Transfer& transfer, uint64_t deadline_milliseconds)
{
    return inner_.Wait(transfer, deadline_milliseconds);
}

bool RecordingHost::ResetEndpoint(uint32_t slot, uint8_t endpoint)
{
    Record("reset " + Hex(endpoint));
    return inner_.ResetEndpoint(slot, endpoint);
}

bool RecordingHost::CancelEndpoint(uint32_t slot, uint8_t endpoint)
{
    Record("cancel " + Hex(endpoint));
    return inner_.CancelEndpoint(slot, endpoint);
}

bool RecordingHost::ClearHalt(uint32_t slot, uint8_t endpoint)
{
    Record("clear-halt " + Hex(endpoint));
    return inner_.ClearHalt(slot, endpoint);
}

void RecordingHost::StopEndpoints(uint32_t slot)
{
    inner_.StopEndpoints(slot);
}

bool RecordingHost::AllocateBuffer(uint64_t bytes, Memory::Dma::Buffer* buffer)
{
    return inner_.AllocateBuffer(bytes, buffer);
}

void RecordingHost::FreeBuffer(const Memory::Dma::Buffer& buffer)
{
    inner_.FreeBuffer(buffer);
}

void RecordingHost::Delay(uint32_t milliseconds)
{
    inner_.Delay(milliseconds);
}

uint64_t RecordingHost::NowMilliseconds()
{
    return inner_.NowMilliseconds();
}

bool RecordingHost::HasWork() const
{
    return inner_.HasWork();
}

bool RecordingHost::TakePortChange(uint32_t* port)
{
    return inner_.TakePortChange(port);
}

bool RecordingHost::Recover()
{
    return inner_.Recover();
}

void RecordingHost::ServiceEndpoints()
{
    inner_.ServiceEndpoints();
}

void RecordingHost::SetRecoveryHandler(RecoveryHandler handler, void* context)
{
    inner_.SetRecoveryHandler(handler, context);
}

} // namespace FeatherOS::Usb::Testing
