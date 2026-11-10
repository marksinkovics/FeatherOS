from smoke.cases.shell.part import Part

PART = Part(
    [b"exit\n"],
    (
        "init: FeatherOS is starting its services",
        # init restarts the console service after the shell's exit command.
        "init: shell exited with status 0",
        "init: restarting shell",
    ),
)
