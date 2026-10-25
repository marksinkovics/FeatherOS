#ifndef FEATHEROS_USER_RAW_TERMINAL_H
#define FEATHEROS_USER_RAW_TERMINAL_H

#include "key_decoder.h"

#include <stddef.h>
#include <termios.h>
#include <unistd.h>

namespace FeatherOS::User
{

// Switches the console to raw input, with Ctrl+C as a key instead of a signal, and reads one
// key at a time.
class RawTerminal
{
public:
    // False when standard input is not the console, or another group has it.
    bool Enter()
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

    void Leave()
    {
        if (m_active)
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &m_original);
            m_active = false;
        }
    }

    // False when a read fails, as after a signal (EINTR); errno says why.
    bool ReadKey(KeyPress* press)
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

private:
    termios m_original = {};
    termios m_raw = {};
    termios m_waiting = {};
    bool m_active = false;
    bool m_waiting_active = false;
    KeyDecoder m_decoder;
    KeyPress m_presses[KeyDecoder::MaximumPresses] = {};
    size_t m_press_count = 0;
    size_t m_next_press = 0;
    char m_bytes[16] = {};
    size_t m_byte_count = 0;
    size_t m_next_byte = 0;

    // Reads more bytes, waiting only briefly while a sequence is incomplete; then none may come.
    bool Fill()
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
};

} // namespace FeatherOS::User

#endif
