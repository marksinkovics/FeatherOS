from smoke.cases.shell.part import Part

# fbtest takes console 1's screen for its checks, which it gives back each
# time; the pty run is a program on a pseudo-terminal, which has no screen.
# fbdemo's pattern is checked by GRAPHICS_TEST; here only its help and the
# same refusal; fontview likewise.
FRAMEBUFFER_INPUT = (
    b"fbtest\n",
    b'script -q -c "fbtest pty" /tmp/fbpty.txt\n',
    b"fbdemo -h\n",
    b'script -q -c "fbdemo 1" /tmp/fbdemo.txt\n',
    b"fontview -h\n",
    b'script -q -c "fontview --card" /tmp/fontview.txt\n',
    b"echo after fbtest | upper\n",
)
EXPECTED_FRAMEBUFFER_OUTPUT = (
    "fbtest: ok",
    "fbtest pty: ok",
    "Takes the screen of the console it runs on",
    "fbdemo: FB_ACQUIRE: Not a terminal",
    "fontview [name] shows the glyph table",
    "fontview: FB_ACQUIRE: Not a terminal",
    "\nAFTER FBTEST",
)

PART = Part(FRAMEBUFFER_INPUT, EXPECTED_FRAMEBUFFER_OUTPUT)
