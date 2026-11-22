#include "report.h"

#include <limits.h>

namespace FeatherOS::Usb::Hid
{
namespace
{

constexpr uint16_t PageGenericDesktop = 0x01;
constexpr uint16_t PageKeyboard = 0x07;
constexpr uint16_t PageLed = 0x08;
constexpr uint16_t PageButton = 0x09;
constexpr uint16_t PageConsumer = 0x0C;
constexpr uint16_t UsageX = 0x30;
constexpr uint16_t UsageY = 0x31;
constexpr uint16_t UsageWheel = 0x38;
constexpr uint16_t UsageAcPan = 0x238;
constexpr uint16_t FirstModifier = 0xE0;
constexpr uint16_t LastModifier = 0xE7;
constexpr uint16_t RolloverUsage = 1;
constexpr uint16_t LastErrorUsage = 3;

constexpr size_t MaxUsages = 64;
constexpr size_t MaxCollectionDepth = 32;
constexpr uint32_t FlagConstant = 0x01;
constexpr uint32_t FlagVariable = 0x02;
constexpr uint32_t FlagRelative = 0x04;

enum class Application : uint8_t
{
    None,
    Keyboard,
    Mouse,
    Other,
};

struct Globals
{
    uint16_t usage_page = 0;
    int32_t logical_minimum = 0;
    int32_t logical_maximum_signed = 0;
    uint32_t logical_maximum_raw = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t report_id = 0;
};

// Usages given in four bytes carry their page; the others take the page current at the main
// item.
struct Locals
{
    uint32_t usages[MaxUsages] = {};
    uint64_t extended_usages = 0;
    size_t usage_count = 0;
    uint32_t minimum = 0;
    uint32_t maximum = 0;
    bool minimum_extended = false;
    bool maximum_extended = false;
    bool has_minimum = false;
    bool has_maximum = false;
};

int32_t SignExtend(uint32_t raw, size_t bytes)
{
    if (bytes == 1)
    {
        return static_cast<int8_t>(raw);
    }
    if (bytes == 2)
    {
        return static_cast<int16_t>(raw);
    }
    return static_cast<int32_t>(raw);
}

class Parser
{
public:
    explicit Parser(ReportMap* map) : m_map(map) {}

    Status Run(std::span<const uint8_t> descriptor);

private:
    ReportMap* m_map;
    Globals m_global;
    Globals m_stack[MaxPushDepth];
    size_t m_stack_depth = 0;
    Locals m_local;
    size_t m_collection_depth = 0;
    Application m_application = Application::None;
    uint16_t m_input_bits[MaxReportIds] = {};
    uint16_t m_output_bits[MaxReportIds] = {};

    Status GlobalItem(uint8_t tag, uint32_t raw, size_t bytes);
    Status LocalItem(uint8_t tag, uint32_t raw, size_t bytes);
    Status MainItem(uint8_t tag, uint32_t raw);
    Status Collection(uint32_t type);
    Status Data(bool output, uint32_t flags);
    Status AddVariable(bool output, uint32_t flags, size_t offset);
    Status Add(const Field& field);
    Status AddRun(bool output, uint32_t flags, uint16_t page, uint16_t usage, size_t elements,
                  size_t offset);
    Status AddArray(size_t offset);
    uint32_t Resolve(uint32_t value, bool extended) const;
    int32_t LogicalMaximum() const;
    void Finish();
    int FindSlot(uint8_t id);
};

uint32_t Parser::Resolve(uint32_t value, bool extended) const
{
    return extended ? value : (static_cast<uint32_t>(m_global.usage_page) << 16) | (value & 0xFFFF);
}

int32_t Parser::LogicalMaximum() const
{
    if (m_global.logical_minimum >= 0 && m_global.logical_maximum_signed < 0)
    {
        return m_global.logical_maximum_raw > static_cast<uint32_t>(INT32_MAX)
                   ? INT32_MAX
                   : static_cast<int32_t>(m_global.logical_maximum_raw);
    }
    return m_global.logical_maximum_signed;
}

int Parser::FindSlot(uint8_t id)
{
    for (size_t i = 0; i < m_map->report_count; ++i)
    {
        if (m_map->reports[i].id == id)
        {
            return static_cast<int>(i);
        }
    }
    if (m_map->report_count == MaxReportIds)
    {
        return -1;
    }
    m_map->reports[m_map->report_count].id = id;
    return m_map->report_count++;
}

Status Parser::Run(std::span<const uint8_t> descriptor)
{
    if (descriptor.empty())
    {
        return Status::Malformed;
    }
    if (descriptor.size() > MaxDescriptorBytes)
    {
        return Status::TooLarge;
    }
    size_t position = 0;
    while (position < descriptor.size())
    {
        const uint8_t prefix = descriptor[position++];
        if (prefix == 0xFE)
        {
            return Status::Malformed;
        }
        const size_t size_code = prefix & 3U;
        const size_t bytes = size_code == 3 ? 4 : size_code;
        if (position + bytes > descriptor.size())
        {
            return Status::Malformed;
        }
        uint32_t raw = 0;
        for (size_t i = 0; i < bytes; ++i)
        {
            raw |= static_cast<uint32_t>(descriptor[position + i]) << (8 * i);
        }
        position += bytes;
        const uint8_t type = (prefix >> 2) & 3U;
        const uint8_t tag = prefix >> 4;
        Status status = Status::Ok;
        if (type == 0)
        {
            status = MainItem(tag, raw);
            m_local = Locals{};
        }
        else if (type == 1)
        {
            status = GlobalItem(tag, raw, bytes);
        }
        else if (type == 2)
        {
            status = LocalItem(tag, raw, bytes);
        }
        if (status != Status::Ok)
        {
            return status;
        }
    }
    if (m_collection_depth != 0)
    {
        return Status::Malformed;
    }
    Finish();
    return Status::Ok;
}

Status Parser::GlobalItem(uint8_t tag, uint32_t raw, size_t bytes)
{
    switch (tag)
    {
    case 0x0:
        if (raw > 0xFFFF)
        {
            return Status::Malformed;
        }
        m_global.usage_page = static_cast<uint16_t>(raw);
        break;
    case 0x1:
        m_global.logical_minimum = SignExtend(raw, bytes);
        break;
    case 0x2:
        m_global.logical_maximum_signed = SignExtend(raw, bytes);
        m_global.logical_maximum_raw = raw;
        break;
    case 0x7:
        if (raw > MaxFieldBits)
        {
            return Status::TooLarge;
        }
        m_global.report_size = raw;
        break;
    case 0x8:
        if (raw == 0 || raw > 0xFF)
        {
            return Status::Malformed;
        }
        for (size_t i = 0; i < m_map->report_count; ++i)
        {
            if (m_map->reports[i].id == 0)
            {
                return Status::Malformed;
            }
        }
        m_map->uses_ids = true;
        m_global.report_id = static_cast<uint8_t>(raw);
        break;
    case 0x9:
        if (raw > MaxReportBytes * 8)
        {
            return Status::TooLarge;
        }
        m_global.report_count = raw;
        break;
    case 0xA:
        if (m_stack_depth == MaxPushDepth)
        {
            return Status::TooLarge;
        }
        m_stack[m_stack_depth++] = m_global;
        break;
    case 0xB:
        if (m_stack_depth == 0)
        {
            return Status::Malformed;
        }
        m_global = m_stack[--m_stack_depth];
        break;
    default:
        break;
    }
    return Status::Ok;
}

Status Parser::LocalItem(uint8_t tag, uint32_t raw, size_t bytes)
{
    const bool extended = bytes == 4;
    switch (tag)
    {
    case 0x0:
        if (m_local.usage_count == MaxUsages)
        {
            return Status::TooLarge;
        }
        if (extended)
        {
            m_local.extended_usages |= uint64_t{1} << m_local.usage_count;
        }
        m_local.usages[m_local.usage_count++] = raw;
        break;
    case 0x1:
        m_local.minimum = raw;
        m_local.minimum_extended = extended;
        m_local.has_minimum = true;
        break;
    case 0x2:
        m_local.maximum = raw;
        m_local.maximum_extended = extended;
        m_local.has_maximum = true;
        break;
    default:
        break;
    }
    return Status::Ok;
}

Status Parser::MainItem(uint8_t tag, uint32_t raw)
{
    switch (tag)
    {
    case 0x8:
        return Data(false, raw);
    case 0x9:
        return Data(true, raw);
    case 0xA:
        return Collection(raw & 0xFF);
    case 0xC:
        if (m_collection_depth == 0)
        {
            return Status::Malformed;
        }
        if (--m_collection_depth == 0)
        {
            m_application = Application::None;
        }
        return Status::Ok;
    default:
        return Status::Ok;
    }
}

Status Parser::Collection(uint32_t type)
{
    if (m_collection_depth == MaxCollectionDepth)
    {
        return Status::TooLarge;
    }
    if (m_collection_depth == 0)
    {
        m_application = Application::Other;
        if (type == 1 && m_local.usage_count > 0)
        {
            const uint32_t usage = Resolve(m_local.usages[0], (m_local.extended_usages & 1U) != 0);
            if (usage == ((uint32_t{PageGenericDesktop} << 16) | 6))
            {
                m_application = Application::Keyboard;
            }
            else if (usage == ((uint32_t{PageGenericDesktop} << 16) | 1) ||
                     usage == ((uint32_t{PageGenericDesktop} << 16) | 2))
            {
                m_application = Application::Mouse;
            }
        }
    }
    ++m_collection_depth;
    return Status::Ok;
}

Status Parser::Add(const Field& field)
{
    if (m_map->field_count == MaxFields)
    {
        return Status::TooLarge;
    }
    m_map->fields[m_map->field_count++] = field;
    return Status::Ok;
}

Status Parser::Data(bool output, uint32_t flags)
{
    const size_t bits = static_cast<size_t>(m_global.report_size) * m_global.report_count;
    if (bits == 0)
    {
        return Status::Ok;
    }
    if (m_map->uses_ids && m_global.report_id == 0)
    {
        return Status::Malformed;
    }
    const int slot = FindSlot(m_global.report_id);
    if (slot < 0)
    {
        return Status::TooLarge;
    }
    uint16_t& used = output ? m_output_bits[slot] : m_input_bits[slot];
    const size_t capacity = (MaxReportBytes - (m_map->uses_ids ? 1 : 0)) * 8;
    if (used + bits > capacity)
    {
        return Status::TooLarge;
    }
    const size_t offset = used;
    used = static_cast<uint16_t>(used + bits);
    if ((flags & FlagConstant) != 0 || m_application == Application::None ||
        m_application == Application::Other)
    {
        return Status::Ok;
    }
    return AddVariable(output, flags, offset);
}

Status Parser::AddVariable(bool output, uint32_t flags, size_t offset)
{
    const size_t size = m_global.report_size;
    const size_t count = m_global.report_count;
    if ((flags & FlagVariable) == 0)
    {
        return output ? Status::Ok : AddArray(offset);
    }
    if (m_local.has_minimum && m_local.has_maximum)
    {
        const uint32_t first = Resolve(m_local.minimum, m_local.minimum_extended);
        const uint32_t last = Resolve(m_local.maximum, m_local.maximum_extended);
        if (last < first || (first >> 16) != (last >> 16))
        {
            return Status::Malformed;
        }
        const size_t elements = count < last - first + 1 ? count : last - first + 1;
        return AddRun(output, flags, static_cast<uint16_t>(first >> 16),
                      static_cast<uint16_t>(first & 0xFFFF), elements, offset);
    }
    size_t index = 0;
    const size_t listed = m_local.usage_count < count ? m_local.usage_count : count;
    while (index < listed)
    {
        const uint32_t first =
            Resolve(m_local.usages[index], ((m_local.extended_usages >> index) & 1U) != 0);
        const uint16_t page = static_cast<uint16_t>(first >> 16);
        size_t elements = 1;
        const bool bits_page = page == PageKeyboard || page == PageLed || page == PageButton;
        while (bits_page && index + elements < listed &&
               Resolve(m_local.usages[index + elements],
                       ((m_local.extended_usages >> (index + elements)) & 1U) != 0) ==
                   first + elements)
        {
            ++elements;
        }
        const Status status = AddRun(output, flags, page, static_cast<uint16_t>(first & 0xFFFF),
                                     elements, offset + index * size);
        if (status != Status::Ok)
        {
            return status;
        }
        index += elements;
    }
    return Status::Ok;
}

Status Parser::AddArray(size_t offset)
{
    if (m_application != Application::Keyboard || !m_local.has_minimum || !m_local.has_maximum)
    {
        return Status::Ok;
    }
    const uint32_t first = Resolve(m_local.minimum, m_local.minimum_extended);
    if ((first >> 16) != PageKeyboard)
    {
        return Status::Ok;
    }
    Field field = {};
    field.kind = FieldKind::KeyArray;
    field.is_signed = m_global.logical_minimum < 0;
    field.report_id = m_global.report_id;
    field.bit_size = static_cast<uint8_t>(m_global.report_size);
    field.bit_offset = static_cast<uint16_t>(offset);
    field.count = static_cast<uint16_t>(m_global.report_count);
    field.usage_base = static_cast<uint16_t>(first & 0xFFFF);
    field.logical_minimum = m_global.logical_minimum;
    field.logical_maximum = LogicalMaximum();
    return Add(field);
}

Status Parser::AddRun(bool output, uint32_t flags, uint16_t page, uint16_t usage, size_t elements,
                      size_t offset)
{
    Field field = {};
    field.is_signed = m_global.logical_minimum < 0;
    field.report_id = m_global.report_id;
    field.bit_size = static_cast<uint8_t>(m_global.report_size);
    field.bit_offset = static_cast<uint16_t>(offset);
    field.count = static_cast<uint16_t>(elements);
    field.usage_base = usage;
    field.logical_minimum = m_global.logical_minimum;
    field.logical_maximum = LogicalMaximum();
    const bool bit = m_global.report_size == 1;
    if (m_application == Application::Keyboard)
    {
        if (output && page == PageLed && bit)
        {
            field.kind = FieldKind::Led;
            return Add(field);
        }
        if (!output && page == PageKeyboard && bit)
        {
            const bool modifiers = usage >= FirstModifier && usage + elements - 1 <= LastModifier;
            field.kind = modifiers ? FieldKind::Modifier : FieldKind::KeyBitmap;
            return Add(field);
        }
        return Status::Ok;
    }
    if (output)
    {
        return Status::Ok;
    }
    if (page == PageButton && bit)
    {
        field.kind = FieldKind::Button;
        return Add(field);
    }
    for (size_t i = 0; i < elements; ++i)
    {
        const uint16_t element = static_cast<uint16_t>(usage + i);
        field.bit_offset = static_cast<uint16_t>(offset + i * m_global.report_size);
        field.count = 1;
        field.usage_base = element;
        const bool relative = (flags & FlagRelative) != 0;
        if (page == PageGenericDesktop && element == UsageX && relative)
        {
            field.kind = FieldKind::X;
        }
        else if (page == PageGenericDesktop && element == UsageY && relative)
        {
            field.kind = FieldKind::Y;
        }
        else if (page == PageGenericDesktop && element == UsageWheel)
        {
            field.kind = FieldKind::Wheel;
        }
        else if (page == PageConsumer && element == UsageAcPan)
        {
            field.kind = FieldKind::Pan;
        }
        else
        {
            continue;
        }
        const Status status = Add(field);
        if (status != Status::Ok)
        {
            return status;
        }
    }
    return Status::Ok;
}

void Parser::Finish()
{
    for (size_t i = 0; i < m_map->report_count; ++i)
    {
        const size_t header = m_map->uses_ids ? 1 : 0;
        ReportInfo& info = m_map->reports[i];
        info.input_bytes =
            m_input_bits[i] == 0 ? 0 : static_cast<uint8_t>(header + (m_input_bits[i] + 7) / 8);
        info.output_bytes =
            m_output_bits[i] == 0 ? 0 : static_cast<uint8_t>(header + (m_output_bits[i] + 7) / 8);
    }
    bool x = false;
    bool y = false;
    for (size_t i = 0; i < m_map->field_count; ++i)
    {
        switch (m_map->fields[i].kind)
        {
        case FieldKind::Modifier:
        case FieldKind::KeyArray:
        case FieldKind::KeyBitmap:
            m_map->has_keyboard = true;
            break;
        case FieldKind::X:
            x = true;
            break;
        case FieldKind::Y:
            y = true;
            break;
        default:
            break;
        }
    }
    m_map->has_mouse = x && y;
}

bool ReadBits(std::span<const uint8_t> report, size_t bit, size_t size, bool is_signed,
              int64_t* value)
{
    if (size == 0 || size > MaxFieldBits || bit + size > report.size() * 8)
    {
        return false;
    }
    const size_t first = bit / 8;
    const size_t shift = bit % 8;
    const size_t bytes = (shift + size + 7) / 8;
    uint64_t raw = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        raw |= static_cast<uint64_t>(report[first + i]) << (8 * i);
    }
    raw = (raw >> shift) & ((uint64_t{1} << size) - 1);
    if (is_signed && (raw >> (size - 1) & 1U) != 0)
    {
        *value = static_cast<int64_t>(raw) - (int64_t{1} << size);
    }
    else
    {
        *value = static_cast<int64_t>(raw);
    }
    return true;
}

const ReportInfo* FindReport(const ReportMap& map, std::span<const uint8_t> report, size_t* base)
{
    uint8_t id = 0;
    *base = 0;
    if (map.uses_ids)
    {
        if (report.empty())
        {
            return nullptr;
        }
        id = report[0];
        *base = 8;
    }
    for (size_t i = 0; i < map.report_count; ++i)
    {
        if (map.reports[i].id == id && map.reports[i].input_bytes != 0)
        {
            return &map.reports[i];
        }
    }
    return nullptr;
}

void AddKey(KeySet* keys, uint16_t usage)
{
    if (usage == 0 || usage <= LastErrorUsage)
    {
        return;
    }
    if (usage >= FirstModifier && usage <= LastModifier)
    {
        keys->modifiers = static_cast<uint8_t>(keys->modifiers | (1U << (usage - FirstModifier)));
        return;
    }
    for (size_t i = 0; i < keys->count; ++i)
    {
        if (keys->keys[i] == usage)
        {
            return;
        }
    }
    if (keys->count < MaxKeys)
    {
        keys->keys[keys->count++] = usage;
    }
}

constexpr uint8_t BootKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

constexpr uint8_t BootMouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15,
    0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};

} // namespace

Status ParseReportDescriptor(std::span<const uint8_t> descriptor, ReportMap* map)
{
    *map = ReportMap{};
    Parser parser(map);
    const Status status = parser.Run(descriptor);
    if (status != Status::Ok)
    {
        *map = ReportMap{};
    }
    return status;
}

bool ExtractKeyboard(const ReportMap& map, std::span<const uint8_t> report, KeySet* keys)
{
    size_t base;
    const ReportInfo* info = FindReport(map, report, &base);
    if (info == nullptr)
    {
        return false;
    }
    KeySet result;
    bool found = false;
    size_t slots = 0;
    size_t rollover_slots = 0;
    for (size_t f = 0; f < map.field_count; ++f)
    {
        const Field& field = map.fields[f];
        if (field.report_id != info->id)
        {
            continue;
        }
        if (field.kind != FieldKind::Modifier && field.kind != FieldKind::KeyArray &&
            field.kind != FieldKind::KeyBitmap)
        {
            continue;
        }
        found = true;
        for (size_t i = 0; i < field.count; ++i)
        {
            int64_t value;
            if (!ReadBits(report, base + field.bit_offset + i * field.bit_size, field.bit_size,
                          field.is_signed, &value))
            {
                return false;
            }
            if (field.kind == FieldKind::KeyArray)
            {
                const int64_t usage = field.usage_base + (value - field.logical_minimum);
                ++slots;
                if (usage == RolloverUsage)
                {
                    ++rollover_slots;
                }
                if (usage > 0 && usage <= 0xFFFF)
                {
                    AddKey(&result, static_cast<uint16_t>(usage));
                }
            }
            else if (value != 0)
            {
                AddKey(&result, static_cast<uint16_t>(field.usage_base + i));
            }
        }
    }
    if (!found)
    {
        return false;
    }
    result.rollover = slots > 0 && rollover_slots == slots;
    *keys = result;
    return true;
}

bool ExtractMouse(const ReportMap& map, std::span<const uint8_t> report, MouseReport* mouse)
{
    size_t base;
    const ReportInfo* info = FindReport(map, report, &base);
    if (info == nullptr)
    {
        return false;
    }
    MouseReport result;
    bool found = false;
    for (size_t f = 0; f < map.field_count; ++f)
    {
        const Field& field = map.fields[f];
        if (field.report_id != info->id)
        {
            continue;
        }
        if (field.kind != FieldKind::Button && field.kind != FieldKind::X &&
            field.kind != FieldKind::Y && field.kind != FieldKind::Wheel &&
            field.kind != FieldKind::Pan)
        {
            continue;
        }
        found = true;
        for (size_t i = 0; i < field.count; ++i)
        {
            int64_t value;
            if (!ReadBits(report, base + field.bit_offset + i * field.bit_size, field.bit_size,
                          field.is_signed, &value))
            {
                return false;
            }
            const auto number = static_cast<int32_t>(value);
            switch (field.kind)
            {
            case FieldKind::Button:
            {
                const size_t usage = field.usage_base + i;
                if (value != 0 && usage >= 1 && usage <= 32)
                {
                    result.buttons |= uint32_t{1} << (usage - 1);
                }
                break;
            }
            case FieldKind::X:
                result.dx = number;
                break;
            case FieldKind::Y:
                result.dy = number;
                break;
            case FieldKind::Wheel:
                result.wheel = number;
                break;
            default:
                result.pan = number;
                break;
            }
        }
    }
    if (!found)
    {
        return false;
    }
    *mouse = result;
    return true;
}

bool BuildLedReport(const ReportMap& map, uint8_t led_state, uint8_t* report, size_t capacity,
                    size_t* length)
{
    const Field* first = nullptr;
    for (size_t f = 0; f < map.field_count && first == nullptr; ++f)
    {
        if (map.fields[f].kind == FieldKind::Led)
        {
            first = &map.fields[f];
        }
    }
    if (first == nullptr)
    {
        return false;
    }
    const ReportInfo* info = nullptr;
    for (size_t i = 0; i < map.report_count; ++i)
    {
        if (map.reports[i].id == first->report_id)
        {
            info = &map.reports[i];
        }
    }
    if (info == nullptr || info->output_bytes == 0 || info->output_bytes > capacity)
    {
        return false;
    }
    for (size_t i = 0; i < info->output_bytes; ++i)
    {
        report[i] = 0;
    }
    const size_t base = map.uses_ids ? 8 : 0;
    if (map.uses_ids)
    {
        report[0] = info->id;
    }
    for (size_t f = 0; f < map.field_count; ++f)
    {
        const Field& field = map.fields[f];
        if (field.kind != FieldKind::Led || field.report_id != info->id)
        {
            continue;
        }
        for (size_t i = 0; i < field.count; ++i)
        {
            const size_t usage = field.usage_base + i;
            if (usage < 1 || usage > MaxLedUsages || (led_state >> (usage - 1) & 1U) == 0)
            {
                continue;
            }
            const size_t bit = base + field.bit_offset + i;
            report[bit / 8] = static_cast<uint8_t>(report[bit / 8] | (1U << (bit % 8)));
        }
    }
    *length = info->output_bytes;
    return true;
}

ReportMap BootKeyboard()
{
    ReportMap map;
    ParseReportDescriptor(BootKeyboardDescriptor, &map);
    return map;
}

ReportMap BootMouse()
{
    ReportMap map;
    ParseReportDescriptor(BootMouseDescriptor, &map);
    return map;
}

} // namespace FeatherOS::Usb::Hid
