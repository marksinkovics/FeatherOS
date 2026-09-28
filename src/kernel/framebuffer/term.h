#ifndef TERM_H
#define TERM_H

#include <stdint.h>

#include "kernel/config.h"

extern "C" {
    #include "kernel/framebuffer/font.h"
}

class Framebuffer;

namespace FeatherOS
{

struct TermConfig
{
    static const uint32_t Row = Config::Height / FONT_HEIGHT;
    static const uint32_t Column = Config::Width / FONT_WIDTH;
    static const uint32_t BufferLen = Row * Column;
};

class Term
{
public:
    void Init(Framebuffer* framebuffer);

    void Print(const char* str);
    void PrintLn(const char* str);

    void Refresh();
private:
    Framebuffer *m_framebuffer;
    uint8_t m_buffer[FeatherOS::TermConfig::Row][FeatherOS::TermConfig::Column];
    uint32_t m_bg_color = FeatherOS::Color::BGColor;
    uint32_t m_fg_color = FeatherOS::Color::FGColor;
    uint32_t m_rows = 0;
    uint32_t m_columns = 0;

    uint32_t m_row_index = 0;
    uint32_t m_column_index = 0;

    void Scroll();
};

};

#endif