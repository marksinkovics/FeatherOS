from smoke.cases.shell.part import Part
from smoke.script import TypeWhen

# Without -n, top redraws in place on the alternate screen until q.
PART = Part((b"top -d 200\n", TypeWhen("; q quits", b"q")), ("; q quits",))
