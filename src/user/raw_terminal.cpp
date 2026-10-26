#include "raw_terminal.h"

#include <unistd.h>

namespace FeatherOS::User
{

bool RawTerminal::Enter()
{
    if (tcgetattr(STDIN_FILENO, &m_original) != 0)
    {
        return false;
    }
    m_raw = m_original;
    cfmakeraw(&m_raw);
    m_minimum = m_raw.c_cc[VMIN];
    m_timeout = m_raw.c_cc[VTIME];
    m_active = tcsetattr(STDIN_FILENO, TCSANOW, &m_raw) == 0;
    return m_active;
}

void RawTerminal::Leave()
{
    if (m_active)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &m_original);
        m_active = false;
    }
}

bool RawTerminal::ReadKey(KeyPress* press)
{
    bool pressed = false;
    while (!pressed)
    {
        if (!ReadKeyWithin(0, press, &pressed))
        {
            return false;
        }
    }
    return true;
}

bool RawTerminal::ReadKeyWithin(unsigned tenths, KeyPress* press, bool* pressed)
{
    *pressed = false;
    while (m_next_press == m_press_count)
    {
        m_next_press = 0;
        m_press_count = 0;
        if (m_next_byte == m_byte_count)
        {
            const bool sequence_pending = m_decoder.Pending();
            if (!Fill(tenths))
            {
                return false;
            }
            if (m_byte_count == 0 && !sequence_pending)
            {
                return true;
            }
        }
        if (m_byte_count == 0)
        {
            m_press_count = m_decoder.Flush(m_presses);
            continue;
        }
        m_press_count = m_decoder.Feed(m_bytes[m_next_byte++], m_presses);
    }
    *press = m_presses[m_next_press++];
    *pressed = true;
    return true;
}

bool RawTerminal::Fill(unsigned tenths)
{
    // A tenth of a second tells a lone Escape key from the start of a sequence.
    const bool sequence_pending = m_decoder.Pending();
    const cc_t minimum = sequence_pending || tenths > 0 ? 0 : 1;
    const cc_t timeout = sequence_pending ? 1 : static_cast<cc_t>(tenths > 255 ? 255 : tenths);
    if (minimum != m_minimum || timeout != m_timeout)
    {
        termios settings = m_raw;
        settings.c_cc[VMIN] = minimum;
        settings.c_cc[VTIME] = timeout;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0)
        {
            return false;
        }
        m_minimum = minimum;
        m_timeout = timeout;
    }
    const ssize_t count = read(STDIN_FILENO, m_bytes, sizeof(m_bytes));
    if (count < 0)
    {
        return false;
    }
    m_byte_count = static_cast<size_t>(count);
    m_next_byte = 0;
    return true;
}

} // namespace FeatherOS::User
