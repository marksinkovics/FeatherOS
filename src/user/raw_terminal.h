#ifndef FEATHEROS_USER_RAW_TERMINAL_H
#define FEATHEROS_USER_RAW_TERMINAL_H

#include "key_decoder.h"

#include <stddef.h>
#include <termios.h>

namespace FeatherOS::User
{

// Switches the console to raw input, with Ctrl+C as a key instead of a signal, and reads one
// key at a time.
class RawTerminal
{
public:
    // False when standard input is not the console, or another group has it.
    bool Enter();

    void Leave();
    // False when a read fails, as after a signal (EINTR); errno says why.
    bool ReadKey(KeyPress* press);

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
    bool Fill();
};

} // namespace FeatherOS::User

#endif
