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
    // A tenth of a second tells a lone Escape key from the start of a sequence.
    m_waiting = m_raw;
    m_waiting.c_cc[VMIN] = 0;
    m_waiting.c_cc[VTIME] = 1;
    m_waiting_active = false;
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
    while (m_next_press == m_press_count)
    {
        m_next_press = 0;
        m_press_count = 0;
        if (m_next_byte == m_byte_count && !Fill())
        {
            return false;
        }
        if (m_byte_count == 0)
        {
            m_press_count = m_decoder.Flush(m_presses);
            continue;
        }
        m_press_count = m_decoder.Feed(m_bytes[m_next_byte++], m_presses);
    }
    *press = m_presses[m_next_press++];
    return true;
}

bool RawTerminal::Fill()
{
    const bool wait_briefly = m_decoder.Pending();
    if (wait_briefly != m_waiting_active &&
        tcsetattr(STDIN_FILENO, TCSANOW, wait_briefly ? &m_waiting : &m_raw) != 0)
    {
        return false;
    }
    m_waiting_active = wait_briefly;
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
