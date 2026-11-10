from smoke.cases.shell.part import Part
from smoke.script import Lines

PIPES_INPUT = (
    b"echo first redirected line > /tmp/r.txt\n"
    b"echo second redirected line >> /tmp/r.txt\n"
    b"upper < /tmp/r.txt\ncat < /tmp/r.txt\ncat < /tmp/none\n"
    b"echo through the pipe | upper\nls /dev|upper\n"
    b"echo two stages|upper|cat > /tmp/p.txt\ncat /tmp/p.txt\n"
    b"echo a | | upper\n"
    # A pipeline is one foreground group, so its first command can read the
    # console.
    b"upper | upper\ntwo uppers\n"
)
EXPECTED_PIPES_OUTPUT = (
    # Redirection; cat's lines start lines of their own, unlike the typed
    # commands that contain the same words.
    "FIRST REDIRECTED LINE",
    "SECOND REDIRECTED LINE",
    "\nfirst redirected line",
    "\nsecond redirected line",
    "shell: /tmp/none: No such file or directory",
    # Pipelines; the typed commands are lower case.
    "THROUGH THE PIPE",
    "RANDOM",
    "\nTWO STAGES",
    "shell: a pipeline has an empty command",
    "TWO UPPERS",
)

PART = Part(Lines(PIPES_INPUT), EXPECTED_PIPES_OUTPUT)
