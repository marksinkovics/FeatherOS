#ifndef FEATHEROS_X86_64_SERIAL_H
#define FEATHEROS_X86_64_SERIAL_H

namespace FeatherOS
{
namespace Serial
{

void Init();
void Write(const char* text);

} // namespace Serial
} // namespace FeatherOS

#endif