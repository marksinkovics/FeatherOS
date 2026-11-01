#include "raw_terminal.h"

#include <errno.h>
#include <unistd.h>

namespace FeatherOS::User
{
namespace
{

volatile sig_atomic_t stop_requested = 0;

void NoteStopRequest(int)
{
    stop_requested = 1;
}

bool TakeStopRequest()
{
    if (stop_requested == 0)
    {
        return false;
    }
    stop_requested = 0;
    return true;
}

} // namespace

bool RawTerminal::Enter(bool wake_on_output)
{
    if (tcgetattr(STDIN_FILENO, &m_original) != 0)
    {
        return false;
    }
    m_raw = m_original;
    cfmakeraw(&m_raw);
    if (wake_on_output)
    {
        m_raw.c_lflag |= FEATHEROS_WAKE_ON_OUTPUT;
    }
    m_minimum = m_raw.c_cc[VMIN];
    m_timeout = m_raw.c_cc[VTIME];
    m_active = tcsetattr(STDIN_FILENO, TCSANOW, &m_raw) == 0;
    if (m_active)
    {
        struct sigaction caught = {};
        caught.sa_handler = NoteStopRequest;
        sigaction(SIGTSTP, &caught, &m_previous_stop_action);
    }
    return m_active;
}

void RawTerminal::Leave()
{
    if (m_active)
    {
        sigaction(SIGTSTP, &m_previous_stop_action, nullptr);
        tcsetattr(STDIN_FILENO, TCSANOW, &m_original);
        m_active = false;
    }
}

void RawTerminal::Suspend()
{
    if (!m_active)
    {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &m_original);
    struct sigaction caught = {};
    const struct sigaction default_action = {};
    sigaction(SIGTSTP, &default_action, &caught);
    // Returns once the group continues; an orphaned group does not stop at all.
    kill(0, SIGTSTP);
    sigaction(SIGTSTP, &caught, nullptr);
    termios settings = m_raw;
    settings.c_cc[VMIN] = m_minimum;
    settings.c_cc[VTIME] = m_timeout;
    tcsetattr(STDIN_FILENO, TCSANOW, &settings);
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
        if (TakeStopRequest())
        {
            *press = {Key::Control, 'z'};
            *pressed = true;
            return true;
        }
        if (m_next_byte == m_byte_count)
        {
            const bool sequence_pending = m_decoder.Pending();
            if (!Fill(tenths))
            {
                if (errno == EINTR && TakeStopRequest())
                {
                    *press = {Key::Control, 'z'};
                    *pressed = true;
                    return true;
                }
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
    // One byte at a time, as readline reads, so what was typed after a line's Enter stays queued
    // for whoever reads next instead of being lost with this object.
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
