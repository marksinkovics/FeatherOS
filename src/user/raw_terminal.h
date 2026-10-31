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
    // False when standard input is not the console, or another group has it. With
    // wake_on_output, ReadKeyWithin(0) also returns without a key after other output (see
    // FEATHEROS_WAKE_ON_OUTPUT).
    bool Enter(bool wake_on_output = false);

    void Leave();
    // False when a read fails, as after a signal (EINTR); errno says why.
    bool ReadKey(KeyPress* press);
    // Waits at most tenths of a second, up to 255 (0 waits for a key); *pressed says whether a key
    // came in time.
    bool ReadKeyWithin(unsigned tenths, KeyPress* press, bool* pressed);

private:
    termios m_original = {};
    termios m_raw = {};
    bool m_active = false;
    // The minimum and timeout last set, so a read changes them only when it needs others.
    cc_t m_minimum = 1;
    cc_t m_timeout = 0;
    KeyDecoder m_decoder;
    KeyPress m_presses[KeyDecoder::MaximumPresses] = {};
    size_t m_press_count = 0;
    size_t m_next_press = 0;
    char m_bytes[16] = {};
    size_t m_byte_count = 0;
    size_t m_next_byte = 0;

    // Reads more bytes, waiting at most tenths of a second (0 waits for ever), or only a tenth
    // while a sequence is incomplete; in both cases none may come.
    bool Fill(unsigned tenths);
};

} // namespace FeatherOS::User

#endif
