#ifndef FEATHEROS_TESTS_STORAGE_STORAGE_RIG_H
#define FEATHEROS_TESTS_STORAGE_STORAGE_RIG_H

#include "kernel/storage/environment.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace FeatherOS::Storage::Testing
{

// One thread: a clock that only waits and sleeps move. While nothing is ready, WaitUntil runs the
// pump (the fake controller's work) and lets time pass in steps up to the deadline.
class PumpEnvironment final : public Environment
{
public:
    static constexpr uint64_t Step = 10;

    explicit PumpEnvironment(std::function<void()> pump = {}, uint64_t start = 1'000'000)
        : pump_(std::move(pump)), now_(start)
    {
    }

    uint64_t NowMilliseconds() override
    {
        return now_;
    }

    bool WaitUntil(const void*, uint64_t deadline_ms, bool (*ready)(void*), void* context) override
    {
        for (;;)
        {
            if (ready(context))
            {
                return true;
            }
            if (now_ >= deadline_ms)
            {
                return false;
            }
            if (pump_)
            {
                pump_();
                if (ready(context))
                {
                    return true;
                }
            }
            now_ += std::min<uint64_t>(deadline_ms - now_, Step);
        }
    }

    void Wake(const void*) override
    {
        ++wakes_;
    }

    void Sleep(uint32_t milliseconds) override
    {
        now_ += milliseconds;
        if (pump_)
        {
            pump_();
        }
    }

    void Advance(uint64_t milliseconds)
    {
        now_ += milliseconds;
    }

    uint32_t Wakes() const
    {
        return wakes_;
    }

private:
    std::function<void()> pump_;
    uint64_t now_;
    uint32_t wakes_ = 0;
};

// Real threads: the clock is the host's, run scale times faster than real time, and Wake ends a
// sleeping WaitUntil early. A wait still re-checks ready every few milliseconds, as the kernel's
// tick step does, because a completion can arrive without a Wake.
class ThreadEnvironment final : public Environment
{
public:
    explicit ThreadEnvironment(uint32_t scale = 1)
        : scale_(scale), start_(std::chrono::steady_clock::now())
    {
    }

    uint64_t NowMilliseconds() override
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        return 1'000'000 + static_cast<uint64_t>(micros) * scale_ / 1000;
    }

    bool WaitUntil(const void*, uint64_t deadline_ms, bool (*ready)(void*), void* context) override
    {
        std::unique_lock lock(mutex_);
        for (;;)
        {
            if (ready(context))
            {
                return true;
            }
            const uint64_t now = NowMilliseconds();
            if (now >= deadline_ms)
            {
                return false;
            }
            const uint64_t real_us = (deadline_ms - now) * 1000 / scale_;
            signal_.wait_for(lock,
                             std::chrono::microseconds(std::min<uint64_t>(real_us + 50, 2000)));
        }
    }

    void Wake(const void*) override
    {
        {
            const std::lock_guard lock(mutex_);
        }
        signal_.notify_all();
    }

    void Sleep(uint32_t milliseconds) override
    {
        std::this_thread::sleep_for(std::chrono::microseconds(milliseconds * 1000ULL / scale_));
    }

private:
    uint32_t scale_;
    std::chrono::steady_clock::time_point start_;
    std::mutex mutex_;
    std::condition_variable signal_;
};

} // namespace FeatherOS::Storage::Testing

#endif
