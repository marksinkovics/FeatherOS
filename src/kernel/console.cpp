#include "console.h"

#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"
#include "kernel/framebuffer/term.h"
#include "kernel/input/input.h"
#include "kernel/klogger.h"
#include "kernel/panic.h"
#include "kernel/string/format.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Console
{
namespace
{

constexpr uint64_t TicksPerSecond = 100;

KLogger* console_logger = nullptr;
Term* console_term = nullptr;
uint64_t last_reported_second = 0;
// Programs on different CPUs share one terminal.
Sync::SpinLock display_lock;

} // namespace

void Initialize(KLogger* logger, Term* term)
{
    console_logger = logger;
    console_term = term;
    last_reported_second = Time::TickCount() / TicksPerSecond;
    if (console_term != nullptr)
    {
        console_term->SetUptimeText("Uptime: 0s");
    }
}

void WriteCharacter(char character)
{
    const char text[] = {character, '\0'};
    Sync::SpinLockGuard guard(display_lock);
    Serial::Write(text);
    if (console_term != nullptr)
    {
        console_term->Print(text);
    }
}

void Service()
{
    const uint64_t current_second = Time::TickCount() / TicksPerSecond;
    if (current_second <= last_reported_second)
    {
        return;
    }
    last_reported_second = current_second;

    char uptime_message[64];
    if (!String::FormatTo(uptime_message, sizeof(uptime_message), "Uptime: {} seconds",
                          current_second))
    {
        Panic("Uptime formatting failed");
    }
    char status_message[32];
    if (!String::FormatTo(status_message, sizeof(status_message), "Uptime: {}s", current_second))
    {
        Panic("Uptime status formatting failed");
    }
    Sync::SpinLockGuard guard(display_lock);
    if (console_term != nullptr)
    {
        console_term->SetUptimeText(status_message);
    }
    Serial::Write("KERN/I: ");
    Serial::Write(uptime_message);
    Serial::Write("\n");
}

size_t ReadLine(const char* prompt, char* buffer, size_t capacity)
{
    if (buffer == nullptr || capacity == 0)
    {
        return 0;
    }
    if (prompt == nullptr)
    {
        prompt = "";
    }
    {
        Sync::SpinLockGuard guard(display_lock);
        Serial::Write(prompt);
        if (console_term != nullptr)
        {
            console_term->BeginInput(prompt);
        }
    }

    size_t length = 0;
    buffer[0] = '\0';
    // Syscalls arrive with interrupts masked; timer ticks and keyboard IRQs must run here.
    const uint64_t interrupt_state = Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Enable();
    bool line_complete = false;
    while (!line_complete)
    {
        Service();
        Input::Poll();
        Input::Event event;
        // Stop at Enter so input typed ahead stays queued for the next reader.
        while (!line_complete && Input::TryReadEvent(&event))
        {
            if (event.type == Input::EventType::Character)
            {
                const char echo[] = {event.character, '\0'};
                Serial::Write(echo);
                if (length < capacity - 1)
                {
                    buffer[length++] = event.character;
                    buffer[length] = '\0';
                }
            }
            else if (event.type == Input::EventType::Backspace)
            {
                Serial::Write("\b \b");
                if (length > 0)
                {
                    buffer[--length] = '\0';
                }
            }
            else
            {
                Serial::Write("\n");
                line_complete = true;
            }
            Sync::SpinLockGuard guard(display_lock);
            if (console_term != nullptr)
            {
                console_term->HandleInputEvent(event);
            }
        }
        if (!line_complete)
        {
            // The deadline keeps the uptime display updating.
            Input::WaitForEvent((Time::TickCount() / TicksPerSecond + 1) * TicksPerSecond);
        }
    }
    {
        Sync::SpinLockGuard guard(display_lock);
        if (console_logger != nullptr)
        {
            console_logger->Info("Input line accepted");
        }
        if (console_term != nullptr)
        {
            console_term->EndInput();
        }
    }
    Arch::Interrupts::SaveAndDisable();
    Arch::Interrupts::Restore(interrupt_state);
    return length;
}

} // namespace FeatherOS::Console
