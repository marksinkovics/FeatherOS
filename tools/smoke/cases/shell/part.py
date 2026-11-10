class Part:
    """A piece of the scripted shell session: what it types, the texts its
    output must contain, and an optional check(result) that returns a failure
    message or None. A part that needs a program the build may skip names it
    in requires, with the note printed when it is missing. interrupts and
    suspends count the ^C and ^Z the part makes the terminal echo."""

    def __init__(
        self,
        script,
        expected=(),
        check=None,
        requires=None,
        missing_note=None,
        interrupts=0,
        suspends=0,
    ):
        self.script = list(script)
        self.expected = tuple(expected)
        self.check = check
        self.requires = requires
        self.missing_note = missing_note
        self.interrupts = interrupts
        self.suspends = suspends
