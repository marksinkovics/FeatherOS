#ifndef FEATHEROS_USB_TEXT_H
#define FEATHEROS_USB_TEXT_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb
{

// Builds a log line in a fixed buffer without allocating; Finish says whether it all fit.
class Text
{
public:
    Text(char* destination, size_t capacity);

    void Put(char value);
    void Words(const char* text);
    void Hex(uint32_t value, uint32_t digits);
    void Decimal(uint32_t value);
    bool Finish();

private:
    char* out_;
    size_t capacity_;
    size_t length_ = 0;
};

} // namespace FeatherOS::Usb

#endif
