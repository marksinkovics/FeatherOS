#include "kernel/arch/rtc.h"

#include "user/calendar.h"

namespace FeatherOS::Arch::RealTimeClock
{
namespace
{

constexpr uint16_t CmosAddressPort = 0x70;
constexpr uint16_t CmosDataPort = 0x71;
constexpr uint8_t RegisterSeconds = 0x00;
constexpr uint8_t RegisterMinutes = 0x02;
constexpr uint8_t RegisterHours = 0x04;
constexpr uint8_t RegisterDay = 0x07;
constexpr uint8_t RegisterMonth = 0x08;
constexpr uint8_t RegisterYear = 0x09;
constexpr uint8_t RegisterStatusA = 0x0A;
constexpr uint8_t RegisterStatusB = 0x0B;
// QEMU and most PCs keep the century here; the ACPI FADT can name another register.
constexpr uint8_t RegisterCentury = 0x32;
constexpr uint8_t StatusAUpdateInProgress = 0x80;
constexpr uint8_t StatusB24Hour = 0x02;
constexpr uint8_t StatusBBinary = 0x04;
constexpr uint8_t HourPm = 0x80;
constexpr uint32_t MaximumReadAttempts = 8;
constexpr uint32_t MaximumUpdateWait = 1000000;

struct RawTime
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
};

void WritePort(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t ReadPort(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint8_t ReadRegister(uint8_t index)
{
    WritePort(CmosAddressPort, index);
    return ReadPort(CmosDataPort);
}

bool ReadRawTime(RawTime* time)
{
    // The RTC updates its registers once a second; never read in the middle of an update.
    uint32_t wait = 0;
    while ((ReadRegister(RegisterStatusA) & StatusAUpdateInProgress) != 0)
    {
        if (++wait == MaximumUpdateWait)
        {
            return false;
        }
    }
    time->second = ReadRegister(RegisterSeconds);
    time->minute = ReadRegister(RegisterMinutes);
    time->hour = ReadRegister(RegisterHours);
    time->day = ReadRegister(RegisterDay);
    time->month = ReadRegister(RegisterMonth);
    time->year = ReadRegister(RegisterYear);
    time->century = ReadRegister(RegisterCentury);
    return true;
}

bool SameRawTime(const RawTime& left, const RawTime& right)
{
    return left.second == right.second && left.minute == right.minute && left.hour == right.hour &&
           left.day == right.day && left.month == right.month && left.year == right.year &&
           left.century == right.century;
}

uint32_t FromBcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

} // namespace

bool ReadUnixSeconds(uint64_t* seconds)
{
    if (seconds == nullptr)
    {
        return false;
    }
    // An update can still start between the check and the reads, so require two equal reads.
    RawTime previous;
    RawTime current;
    if (!ReadRawTime(&current))
    {
        return false;
    }
    uint32_t attempt = 0;
    do
    {
        if (++attempt == MaximumReadAttempts)
        {
            return false;
        }
        previous = current;
        if (!ReadRawTime(&current))
        {
            return false;
        }
    } while (!SameRawTime(previous, current));

    const uint8_t status_b = ReadRegister(RegisterStatusB);
    const bool binary = (status_b & StatusBBinary) != 0;
    const bool pm = (current.hour & HourPm) != 0;
    const uint8_t hour = static_cast<uint8_t>(current.hour & ~HourPm);
    const auto decode = [binary](uint8_t value) -> uint32_t
    { return binary ? value : FromBcd(value); };

    User::CalendarTime time;
    time.second = decode(current.second);
    time.minute = decode(current.minute);
    time.hour = decode(hour);
    time.day = decode(current.day);
    time.month = decode(current.month);
    if ((status_b & StatusB24Hour) == 0)
    {
        // 12-hour mode counts 12, 1, ..., 11 and flags afternoon hours.
        time.hour = time.hour % 12 + (pm ? 12 : 0);
    }
    const uint32_t century = decode(current.century);
    time.year = (century >= 19 && century <= 99 ? century * 100 : 2000) + decode(current.year);
    return User::UnixSecondsFromCalendar(time, seconds);
}

} // namespace FeatherOS::Arch::RealTimeClock
