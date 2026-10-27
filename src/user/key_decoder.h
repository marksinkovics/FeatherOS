#ifndef FEATHEROS_USER_KEY_DECODER_H
#define FEATHEROS_USER_KEY_DECODER_H

#include <stddef.h>

namespace FeatherOS::User
{

enum class Key : unsigned char
{
    Character,
    // Ctrl with a letter or one of @[\]^_; character holds it in lower case.
    Control,
    Enter,
    Tab,
    BackTab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    Insert,
    Delete,
    PageUp,
    PageDown,
    Unknown,
};

struct KeyPress
{
    Key key;
    char character;
};

// Decodes what a terminal sends in raw mode, one byte at a time, since a read can end inside an
// escape sequence. Both the CSI (ESC [) and SS3 (ESC O) forms of the cursor keys are understood,
// and Home and End also as ESC [ 1 ~, ESC [ 7 ~, ESC [ 4 ~ and ESC [ 8 ~.
class KeyDecoder
{
public:
    static constexpr size_t MaximumPresses = 2;

    // Returns how many keys byte completed: two when an Escape key is followed by a byte that
    // does not start a sequence.
    size_t Feed(char byte, KeyPress (&presses)[MaximumPresses]);
    // True while a sequence is incomplete, so the caller can wait briefly for its rest.
    bool Pending() const;
    // Ends an incomplete sequence when no more bytes came: a lone ESC is the Escape key.
    size_t Flush(KeyPress (&presses)[MaximumPresses]);

private:
    char m_sequence[8] = {};
    size_t m_length = 0;

    static KeyPress Single(char byte);
    // The parameter before '~' picks the key; any after ';' (modifiers) are ignored.
    unsigned Parameter() const;

    Key Final(char byte) const;
};

} // namespace FeatherOS::User

#endif
