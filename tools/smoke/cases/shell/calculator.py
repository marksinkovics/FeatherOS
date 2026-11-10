from smoke.cases.shell.part import Part
from smoke.script import Lines

PART = Part(
    Lines(b"calc\n1 / 3\n7 / 0\nexit\n"),
    ("calc> ", "0.333333", "calc: division by zero"),
)
