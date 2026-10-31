#include "dns_message.h"

namespace FeatherOS::LibC::Dns
{
namespace
{

constexpr size_t HeaderSize = 12;
constexpr size_t MaximumNameLength = 253;
constexpr size_t MaximumLabelLength = 63;
constexpr uint16_t ClassInternet = 1;
constexpr uint8_t ResponseFlag = 0x80;
constexpr uint8_t TruncatedFlag = 0x02;
constexpr uint8_t RecursionDesired = 0x01;
constexpr uint8_t ResponseCodeMask = 0x0F;
constexpr uint8_t NameError = 3;
// The top two bits of a length byte mark a pointer to an earlier name (RFC 1035 4.1.4).
constexpr uint8_t PointerBits = 0xC0;

uint16_t Read16(const uint8_t* data)
{
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

void Write16(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

// The offset just past the name at offset, or 0 when it runs out of the message. A pointer ends
// the name in place, so the name's own bytes never loop.
size_t SkipName(const uint8_t* message, size_t length, size_t offset)
{
    while (offset < length)
    {
        const uint8_t label = message[offset];
        if (label == 0)
        {
            return offset + 1;
        }
        if ((label & PointerBits) == PointerBits)
        {
            return offset + 2 <= length ? offset + 2 : 0;
        }
        if ((label & PointerBits) != 0)
        {
            return 0;
        }
        offset += 1 + label;
    }
    return 0;
}

bool IsHostCharacter(uint8_t character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_';
}

// Writes the possibly compressed name at offset as dotted text. A pointer must point before
// itself, so a loop has to pass through labels, and the length limit ends it.
bool ReadName(const uint8_t* message, size_t length, size_t offset, char* text, size_t capacity)
{
    size_t written = 0;
    while (offset < length)
    {
        const uint8_t label = message[offset];
        if (label == 0)
        {
            if (written == 0 || written >= capacity)
            {
                return false;
            }
            text[written] = '\0';
            return true;
        }
        if ((label & PointerBits) == PointerBits)
        {
            if (offset + 2 > length)
            {
                return false;
            }
            const size_t target =
                (static_cast<size_t>(label & ~PointerBits) << 8) | message[offset + 1];
            if (target >= offset)
            {
                return false;
            }
            offset = target;
            continue;
        }
        if ((label & PointerBits) != 0 || offset + 1 + label > length)
        {
            return false;
        }
        if (written > 0)
        {
            text[written++] = '.';
        }
        if (written + label > MaximumNameLength || written + label >= capacity)
        {
            return false;
        }
        for (size_t index = 1; index <= label; ++index)
        {
            if (!IsHostCharacter(message[offset + index]))
            {
                return false;
            }
            text[written++] = static_cast<char>(message[offset + index]);
        }
        offset += 1 + label;
    }
    return false;
}

// Checks the header and question, then calls visit with the data offset and length of each
// answer record of the type.
template <typename Visit>
Outcome VisitAnswers(const uint8_t* message, size_t length, uint16_t id, uint16_t type, Visit visit)
{
    if (message == nullptr || length < HeaderSize || Read16(message) != id ||
        (message[2] & ResponseFlag) == 0 || Read16(message + 4) != 1)
    {
        return Outcome::Unrelated;
    }
    size_t offset = SkipName(message, length, HeaderSize);
    if (offset == 0 || offset + 4 > length || Read16(message + offset) != type)
    {
        return Outcome::Unrelated;
    }
    offset += 4;
    if ((message[2] & TruncatedFlag) != 0)
    {
        return Outcome::Truncated;
    }
    const uint8_t code = message[3] & ResponseCodeMask;
    if (code == NameError)
    {
        return Outcome::NoName;
    }
    if (code != 0)
    {
        return Outcome::Failed;
    }
    const uint16_t answers = Read16(message + 6);
    for (uint16_t answer = 0; answer < answers; ++answer)
    {
        offset = SkipName(message, length, offset);
        if (offset == 0 || offset + 10 > length)
        {
            return Outcome::Failed;
        }
        const uint16_t record_type = Read16(message + offset);
        const uint16_t record_class = Read16(message + offset + 2);
        const uint16_t data_length = Read16(message + offset + 8);
        offset += 10;
        if (offset + data_length > length)
        {
            return Outcome::Failed;
        }
        if (record_type == type && record_class == ClassInternet)
        {
            visit(offset, data_length);
        }
        offset += data_length;
    }
    return Outcome::Answered;
}

} // namespace

size_t BuildQuery(uint8_t* message, size_t capacity, uint16_t id, const char* name, uint16_t type)
{
    size_t name_length = 0;
    while (name[name_length] != '\0')
    {
        ++name_length;
    }
    if (name_length > 0 && name[name_length - 1] == '.')
    {
        --name_length;
    }
    // Each label gains a length byte, and the name a final empty label.
    const size_t encoded = name_length + 2;
    if (name_length == 0 || name_length > MaximumNameLength || HeaderSize + encoded + 4 > capacity)
    {
        return 0;
    }
    for (size_t index = 0; index < HeaderSize; ++index)
    {
        message[index] = 0;
    }
    Write16(message, id);
    message[2] = RecursionDesired;
    Write16(message + 4, 1);

    size_t offset = HeaderSize;
    for (size_t start = 0; start <= name_length;)
    {
        size_t end = start;
        while (end < name_length && name[end] != '.')
        {
            ++end;
        }
        const size_t label = end - start;
        if (label == 0 || label > MaximumLabelLength)
        {
            return 0;
        }
        message[offset++] = static_cast<uint8_t>(label);
        for (size_t index = start; index < end; ++index)
        {
            message[offset++] = static_cast<uint8_t>(name[index]);
        }
        start = end + 1;
    }
    message[offset++] = 0;
    Write16(message + offset, type);
    Write16(message + offset + 2, ClassInternet);
    return offset + 4;
}

Outcome ParseResponse(const uint8_t* message, size_t length, uint16_t id, uint16_t type,
                      Addresses* addresses)
{
    addresses->count = 0;
    const size_t address_size = type == TypeAaaa ? 16 : 4;
    return VisitAnswers(message, length, id, type,
                        [&](size_t data, uint16_t data_length)
                        {
                            if (data_length == address_size && addresses->count < MaximumAddresses)
                            {
                                for (size_t index = 0; index < address_size; ++index)
                                {
                                    addresses->bytes[addresses->count][index] =
                                        message[data + index];
                                }
                                ++addresses->count;
                            }
                        });
}

size_t BuildReverseName(const uint8_t* address, size_t address_length, char* name, size_t capacity)
{
    constexpr char HexDigits[] = "0123456789abcdef";
    size_t written = 0;
    bool fits = true;
    const auto append = [&](char character)
    {
        if (written + 1 >= capacity)
        {
            fits = false;
            return;
        }
        name[written++] = character;
    };
    const auto append_text = [&](const char* text)
    {
        for (; *text != '\0'; ++text)
        {
            append(*text);
        }
    };
    if (address_length == 4)
    {
        for (size_t index = address_length; index-- > 0;)
        {
            const uint8_t byte = address[index];
            if (byte >= 100)
            {
                append(static_cast<char>('0' + byte / 100));
            }
            if (byte >= 10)
            {
                append(static_cast<char>('0' + byte / 10 % 10));
            }
            append(static_cast<char>('0' + byte % 10));
            append('.');
        }
        append_text("in-addr.arpa");
    }
    else if (address_length == 16)
    {
        for (size_t index = address_length; index-- > 0;)
        {
            append(HexDigits[address[index] & 0x0F]);
            append('.');
            append(HexDigits[address[index] >> 4]);
            append('.');
        }
        append_text("ip6.arpa");
    }
    else
    {
        fits = false;
    }
    if (!fits || capacity == 0)
    {
        return 0;
    }
    name[written] = '\0';
    return written;
}

Outcome ParsePointerResponse(const uint8_t* message, size_t length, uint16_t id, char* name,
                             size_t capacity)
{
    if (capacity > 0)
    {
        name[0] = '\0';
    }
    bool found = false;
    return VisitAnswers(message, length, id, TypePtr,
                        [&](size_t data, uint16_t data_length)
                        {
                            if (!found &&
                                SkipName(message, data + data_length, data) == data + data_length)
                            {
                                found = ReadName(message, length, data, name, capacity);
                                if (!found && capacity > 0)
                                {
                                    name[0] = '\0';
                                }
                            }
                        });
}

} // namespace FeatherOS::LibC::Dns
