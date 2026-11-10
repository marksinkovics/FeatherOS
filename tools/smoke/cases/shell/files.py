import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

FILES_INPUT = (
    b"mkdir work\ncd work\nwrite a.txt in work\npwd\ncat a.txt\ncd ..\n"
    b"cat work/a.txt\ncd /\nls\ncat hello.txt\ncd nowhere\ncd\npwd\n"
    b"ls /dev\nwrite /tmp/note.txt kept in tmp\ncat /tmp/note.txt\n"
    b"cat /proc/self/status\ncat /proc/meminfo\n"
)
EXPECTED_FILES_OUTPUT = (
    "system/",
    "etc/",
    "home/",
    "FeatherOS initramfs is ready.",
    "cd: nowhere: no such directory",
    # /dev, /tmp, and /proc; the console shows tabs as spaces.
    "random                  0",
    "tty4                    0",
    "kept in tmp",
    "Name: cat",
    "MemTotal: ",
)
# pwd prints the directory alone; prompts follow it with "> ".
WORKING_DIRECTORY_OUTPUT = re.compile(r"^(/home(?:/work)?)\r?$", re.MULTILINE)
WORK_FILE_OUTPUT = re.compile(r"^in work\r?$", re.MULTILINE)


def Check(result):
    directories = WORKING_DIRECTORY_OUTPUT.findall(result.output)
    if directories != ["/home/work", "/home"]:
        return f"pwd did not follow cd: {directories}"
    if len(WORK_FILE_OUTPUT.findall(result.output)) != 2:
        return "a file was not readable through a relative path"
    return None


PART = Part(Lines(FILES_INPUT), EXPECTED_FILES_OUTPUT, Check)
