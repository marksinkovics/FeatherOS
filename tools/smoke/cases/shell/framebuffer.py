from smoke.cases.shell.part import Part

# fbtest takes console 1's screen for its checks, which it gives back each
# time; the pty run is a program on a pseudo-terminal, which has no screen.
FRAMEBUFFER_INPUT = (
    b"fbtest\n",
    b'script -q -c "fbtest pty" /tmp/fbpty.txt\n',
    b"echo after fbtest | upper\n",
)
EXPECTED_FRAMEBUFFER_OUTPUT = (
    "fbtest: ok",
    "fbtest pty: ok",
    "\nAFTER FBTEST",
)

PART = Part(FRAMEBUFFER_INPUT, EXPECTED_FRAMEBUFFER_OUTPUT)
