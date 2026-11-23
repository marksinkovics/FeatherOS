#include "identify.h"

#include <stddef.h>

namespace FeatherOS::Storage::Ata
{
namespace
{

using Words = std::span<const uint16_t, IdentifyWords>;

constexpr uint32_t WordSerial = 10;
constexpr uint32_t WordFirmware = 23;
constexpr uint32_t WordModel = 27;
constexpr uint32_t WordSupported1 = 82;
constexpr uint32_t WordSupported2 = 83;
constexpr uint32_t WordEnabled1 = 85;
constexpr uint32_t WordEnabled2 = 86;
constexpr uint32_t WordLba48Count = 100;
constexpr uint32_t WordSectorInfo = 106;
constexpr uint32_t WordSectorWords = 117;
constexpr uint32_t WordIntegrity = 255;

constexpr uint16_t ValidMask = 0xC000;
constexpr uint16_t ValidPattern = 0x4000;
constexpr uint16_t WriteCacheBit = 1U << 5;
constexpr uint16_t FlushBit = 1U << 12;
constexpr uint16_t FlushExtBit = 1U << 13;
constexpr uint16_t Lba48Bit = 1U << 10;
constexpr uint16_t LongSectorBit = 1U << 12;
constexpr uint8_t IntegritySignature = 0xA5;

template <size_t N> void CopyText(Words words, uint32_t first, char (&out)[N])
{
    static_assert(N % 2 == 1);
    constexpr uint32_t Chars = N - 1;
    char raw[Chars];
    for (uint32_t i = 0; i < Chars; i++)
    {
        const uint16_t word = words[first + i / 2];
        raw[i] = static_cast<char>((i % 2 == 0) ? (word >> 8) : (word & 0xFF));
    }
    uint32_t begin = 0;
    uint32_t end = Chars;
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\0'))
    {
        end--;
    }
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\0'))
    {
        begin++;
    }
    size_t used = 0;
    for (uint32_t i = begin; i < end; i++)
    {
        const auto c = static_cast<uint8_t>(raw[i]);
        out[used++] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
    }
    for (size_t i = used; i < N; i++)
    {
        out[i] = '\0';
    }
}

bool ChecksumHolds(Words words)
{
    uint8_t sum = 0;
    for (const uint16_t word : words)
    {
        sum = static_cast<uint8_t>(sum + (word & 0xFF) + (word >> 8));
    }
    return sum == 0;
}

uint32_t LogicalSectorBytes(Words words)
{
    const uint16_t info = words[WordSectorInfo];
    if ((info & ValidMask) == ValidPattern && (info & LongSectorBit) != 0)
    {
        const uint64_t sector_words =
            words[WordSectorWords] | (static_cast<uint64_t>(words[WordSectorWords + 1]) << 16);
        return sector_words > 0x7FFFFFFF ? 0 : static_cast<uint32_t>(sector_words * 2);
    }
    return 512;
}

} // namespace

AtaIdentity ParseIdentify(Words words)
{
    AtaIdentity data{};
    CopyText(words, WordModel, data.model);
    CopyText(words, WordSerial, data.serial);
    CopyText(words, WordFirmware, data.firmware);

    const uint16_t supported2 = words[WordSupported2];
    const bool supported2_valid = (supported2 & ValidMask) == ValidPattern;
    data.lba48 =
        supported2_valid && (supported2 & Lba48Bit) != 0 && (words[WordEnabled2] & Lba48Bit) != 0;
    data.flush_cache_supported = supported2_valid && (supported2 & FlushBit) != 0;
    data.flush_cache_ext_supported = supported2_valid && (supported2 & FlushExtBit) != 0;
    const uint16_t supported1 = words[WordSupported1];
    data.write_cache_supported =
        (supported1 & ValidMask) == ValidPattern && (supported1 & WriteCacheBit) != 0;
    const uint16_t enabled1 = words[WordEnabled1];
    data.write_cache_enabled =
        (enabled1 & ValidMask) == ValidPattern && (enabled1 & WriteCacheBit) != 0;

    for (uint32_t i = 0; i < 4; i++)
    {
        data.sector_count |= static_cast<uint64_t>(words[WordLba48Count + i]) << (16 * i);
    }
    data.sector_size = LogicalSectorBytes(words);

    const uint16_t integrity = words[WordIntegrity];
    data.checksum_present = (integrity & 0xFF) == IntegritySignature;
    if (data.checksum_present && !ChecksumHolds(words))
    {
        data.refusal = Refusal::Checksum;
    }
    else if (!data.lba48)
    {
        data.refusal = Refusal::NoLba48;
    }
    else if (data.sector_size != 512 && data.sector_size != 4096)
    {
        data.refusal = Refusal::SectorSize;
    }
    else if (data.sector_count == 0)
    {
        data.refusal = Refusal::Empty;
    }
    return data;
}

const char* RefusalText(Refusal refusal)
{
    switch (refusal)
    {
    case Refusal::None:
        return "accepted";
    case Refusal::Checksum:
        return "IDENTIFY data checksum is wrong";
    case Refusal::NoLba48:
        return "48-bit LBA not supported";
    case Refusal::SectorSize:
        return "logical sector size other than 512 or 4096 not supported";
    case Refusal::Empty:
        return "no sectors";
    }
    return "refused";
}

} // namespace FeatherOS::Storage::Ata
