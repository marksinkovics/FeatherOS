#include "fake_hardware.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace FeatherOS::Usb::Testing
{

FakeHardware::FakeHardware(FakeXhci& controller, PhysicalMemory& memory)
    : controller_(controller), memory_(memory)
{
}

uint32_t FakeHardware::Read32(uint32_t offset)
{
    return controller_.Read32(offset);
}

void FakeHardware::Write32(uint32_t offset, uint32_t value)
{
    if (defer_doorbells_ && offset >= FakeXhci::DoorbellBase &&
        offset < FakeXhci::DoorbellBase + (FakeXhci::MaxSlots + 1) * 4)
    {
        const std::lock_guard guard(doorbell_mutex_);
        doorbells_.emplace_back(offset, value);
        return;
    }
    controller_.Write32(offset, value);
}

void FakeHardware::DeferDoorbells(bool defer)
{
    defer_doorbells_ = defer;
}

bool FakeHardware::PumpDoorbells()
{
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    {
        const std::lock_guard guard(doorbell_mutex_);
        taken.swap(doorbells_);
    }
    for (const auto& [offset, value] : taken)
    {
        controller_.Write32(offset, value);
    }
    return !taken.empty();
}

void FakeHardware::Write64(uint32_t offset, uint64_t value)
{
    controller_.Write64(offset, value);
}

uint32_t FakeHardware::ReadConfig32(uint16_t offset)
{
    const auto found = config_.find(offset);
    return found == config_.end() ? 0 : found->second;
}

void FakeHardware::WriteConfig32(uint16_t offset, uint32_t value)
{
    config_writes_.push_back({offset, value});
}

void FakeHardware::SetConfig(uint16_t offset, uint32_t value)
{
    config_[offset] = value;
}

bool FakeHardware::AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer)
{
    if (allocations_before_failure == 0)
    {
        return false;
    }
    if (allocations_before_failure != ~0U)
    {
        --allocations_before_failure;
    }
    const Memory::Dma::Buffer allocated = memory_.Allocate(bytes);
    if (allocated.physical + allocated.size > below || allocated.physical + allocated.size > allocation_limit)
    {
        return false;
    }
    *buffer = allocated;
    live_.push_back(allocated);
    return true;
}

void FakeHardware::FreeDma(const Memory::Dma::Buffer& buffer)
{
    for (auto found = live_.begin(); found != live_.end(); ++found)
    {
        if (found->physical == buffer.physical)
        {
            live_.erase(found);
            return;
        }
    }
    FAIL("a buffer was freed that is not live");
}

uint64_t FakeHardware::NowMilliseconds()
{
    return now_;
}

uint64_t FakeHardware::Now() const
{
    return now_;
}

void FakeHardware::Pause(uint32_t milliseconds)
{
    now_ += milliseconds;
    if (pause_microseconds != 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(pause_microseconds));
    }
    if (on_idle)
    {
        on_idle();
    }
}

bool FakeHardware::WaitUntil(bool (*done)(const void* context), const void* context,
                             uint64_t deadline_milliseconds)
{
    for (;;)
    {
        if (done(context))
        {
            return true;
        }
        if (now_ >= deadline_milliseconds)
        {
            return false;
        }
        Pause(1);
    }
}

void FakeHardware::WakeWaiters()
{
    ++wakes_;
}

void FakeHardware::SignalEvents()
{
    ++signals_;
}

void FakeHardware::WaitForEvents(uint64_t deadline_milliseconds)
{
    if (now_ < deadline_milliseconds)
    {
        now_ = deadline_milliseconds;
    }
}

Xhci::InterruptKind FakeHardware::SetUpInterrupts(bool allow, void (*handler)(void* context),
                                                  void* context)
{
    requested_ = true;
    allowed_ = allow;
    handler_ = handler;
    handler_context_ = context;
    return allow ? available : Xhci::InterruptKind::Polling;
}

void FakeHardware::FireInterrupt()
{
    if (handler_ != nullptr)
    {
        handler_(handler_context_);
    }
}

uint32_t FakeHardware::Outstanding() const
{
    return static_cast<uint32_t>(live_.size());
}

uint32_t FakeHardware::Wakes() const
{
    return wakes_;
}

uint32_t FakeHardware::Signals() const
{
    return signals_;
}

bool FakeHardware::InterruptsAllowed() const
{
    return allowed_;
}

bool FakeHardware::InterruptsRequested() const
{
    return requested_;
}

const std::vector<FakeHardware::ConfigWrite>& FakeHardware::ConfigWrites() const
{
    return config_writes_;
}

const std::vector<Memory::Dma::Buffer>& FakeHardware::Live() const
{
    return live_;
}

} // namespace FeatherOS::Usb::Testing
