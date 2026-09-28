#include "klogger.h"

#include "kernel/framebuffer/term.h"

namespace FeatherOS
{

void KLogger::Init(Term* term)
{
    m_term = term;
}

void KLogger::Debug(const char* message)
{
    Write("D", message);
}

void KLogger::Info(const char* message)
{
    Write("I", message);
}

void KLogger::Warning(const char* message)
{
    Write("W", message);
}

void KLogger::Error(const char* message)
{
    Write("E", message);
}

void KLogger::Write(const char* level, const char* message)
{
    if (m_term == nullptr || message == nullptr)
    {
        return;
    }

    char line[256];
    uint32_t length = 0;
    const char* prefix = "KERN/";
    while (*prefix != '\0')
    {
        line[length++] = *prefix++;
    }
    while (*level != '\0')
    {
        line[length++] = *level++;
    }
    line[length++] = ':';
    line[length++] = ' ';

    while (*message != '\0' && length < sizeof(line) - 1)
    {
        line[length++] = *message++;
    }
    if (*message != '\0')
    {
        line[sizeof(line) - 4] = '.';
        line[sizeof(line) - 3] = '.';
        line[sizeof(line) - 2] = '.';
        length = sizeof(line) - 1;
    }
    line[length] = '\0';
    m_term->PrintLn(line);
}

}