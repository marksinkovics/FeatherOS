import json
import re

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.cases.graphics import (
    SHELL_PROMPT_TEXT,
    EventStream,
    ExpectText,
    ExpectTextAgain,
)
from smoke.cases.screen import (
    BLACK,
    WIDTH,
    Difference,
    ExpectColor,
    Found,
    PixelAt,
    ScreenCase,
    StepFailed,
)
from smoke.script import Lines
from smoke.terminal import SHELL_PROMPT

DUNGEON_TIMEOUT_SECONDS = 300
SELFTEST_LINE = "wickdeep: selftest ok"
# The same on both architectures: the rules use integers only.
DEMO_RESULTS = {
    1: "turns 2690 depth 8 level 7 score 1945 outcome victory "
    "hash 10821229194343717296",
    2: "turns 703 depth 3 level 2 score 223 outcome dead "
    "hash 18362298982438112458",
}
SAVE_PATH = "/tmp/w.sav"
PACK_SAVE = "/home/pack.sav"
SAVE_OK_LINE = "wickdeep: save ok depth 2 turn 500"
SAVE_REJECTED_LINE = "wickdeep: save rejected length"

# wickdeep's layout (src/user/wickdeep/render.swift) on the 1024x768 screen:
# the floor is 60x40 cells of 16 pixels at (32, 8), the panel 960x112 at
# (32, 656), the overlay box 640x480 at (192, 88).
GAME_SEED = 7
GAME_START_SECONDS = 10.0
FLOOR_X = 32
FLOOR_Y = 8
FLOOR_WIDTH = 960
FLOOR_HEIGHT = 640
PANEL_X = 32
PANEL_Y = 656
BOX_X = 192
BOX_Y = 88
BACKGROUND = 0x0B0D16
PANEL_FILL = 0x1A1620
PANEL_BORDER = 0xB07A3C
OVERLAY_FILL = 0x1E1A2A
SELECTION = 0x3A2F4A
ACCENT = 0xFFC857
TEXT = 0xE8E0D0
# Only the wick-keeper's tile has the glow color inside the floor area.
GLOW = 0xFFF08C
LIT_FLOOR = 0x3E4560
DARK_FRACTION = 0.8
SAVE_FILE = "/home/.wickdeep-save"
BAD_FILE = "/home/.wickdeep-save.bad"
CHECK_LINE = re.compile(r"^wickdeep: save ok depth 1 turn (\d+)\r?\n", re.M)
PACK_LINE = re.compile(r"^wickdeep: save ok depth 2 turn (\d+)\r?\n", re.M)
BAD_LINE = re.compile(r"^(\d+) " + re.escape(BAD_FILE) + r"\r?\n", re.M)
# Four directions in turn, so that walls in a room cannot stop all of them.
WALK = ("left",) * 5 + ("down",) * 5 + ("right",) * 5 + ("up",) * 5
PROMPT_SECONDS = 8.0


def DemoLine(seed):
    return f"wickdeep: demo {DEMO_RESULTS[seed]}"


def FloorBytes(pixels):
    rows = []
    for y in range(FLOOR_Y, FLOOR_Y + FLOOR_HEIGHT):
        start = 3 * (y * WIDTH + FLOOR_X)
        rows.append(bytes(pixels[start : start + 3 * FLOOR_WIDTH]))
    return b"".join(rows)


def BoxBytes(pixels):
    rows = []
    for y in range(BOX_Y, BOX_Y + 480):
        start = 3 * (y * WIDTH + BOX_X)
        rows.append(bytes(pixels[start : start + 3 * 640]))
    return b"".join(rows)


def DarkShare(pixels):
    dark = 0
    total = 0
    for y in range(FLOOR_Y, FLOOR_Y + FLOOR_HEIGHT, 4):
        for x in range(FLOOR_X, FLOOR_X + FLOOR_WIDTH, 4):
            dark += PixelAt(pixels, x, y) == BLACK
            total += 1
    return dark / total


def ExpectFrame(pixels):
    for what, point, wanted in (
        ("the panel border", (PANEL_X, PANEL_Y + 44), PANEL_BORDER),
        ("the panel", (PANEL_X + 948, PANEL_Y + 44), PANEL_FILL),
        ("the background", (10, 400), BACKGROUND),
    ):
        failure = ExpectColor(pixels, what, point, wanted)
        if failure is not None:
            return failure
    return None


def ExpectBox(pixels):
    failure = ExpectColor(
        pixels, "the overlay box", (BOX_X + 8, BOX_Y + 8), OVERLAY_FILL
    )
    if failure is None:
        failure = ExpectColor(
            pixels, "the box's border", (BOX_X, BOX_Y + 100), ACCENT
        )
    return failure


class Dungeon(ScreenCase, Case):
    """Runs wickdeep's self-test and two demo runs (one wins, one dies), then
    saves a demo run at turn 500, checks the file and checks that it is
    refused with a reason once a byte is appended; last it plays wickdeep
    with QMP keys and checks the frame, the lit floor and the wick-keeper,
    the pack, a walk, a console switch, the save on Esc, the resumed floor and
    a corrupted save that is set aside."""

    serial_input = True
    timeout = DUNGEON_TIMEOUT_SECONDS
    passed = (
        "wickdeep's self-test passed and its demo runs gave the expected "
        "turns, depth, score and state hashes, its saved state was accepted, "
        "then refused after a byte was appended, and the played game drew "
        "its frame, floor and panel, opened its pack, redrew after a console "
        "switch, saved on Esc, resumed to the same floor and set a corrupted "
        "save aside"
    )

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.state = 0
        self.finished = False
        self.first_floor = None
        self.last_floor = None
        self.pack_box = None

    def Script(self, session):
        demos = "".join(
            f"wickdeep --demo --seed {seed}\n" for seed in DEMO_RESULTS
        )
        saves = (
            f"wickdeep --demo --seed 1 --save-at 500 {SAVE_PATH}\n"
            f"wickdeep --check-save {SAVE_PATH}\n"
            f"cp {SAVE_PATH} {PACK_SAVE}\n"
            f"echo x >> {SAVE_PATH}\n"
            f"wickdeep --check-save {SAVE_PATH}\n"
        )
        return Lines(f"wickdeep --selftest\n{demos}{saves}".encode())

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.finished

    def Type(self, session, text):
        session.process.stdin.write(text.encode())
        session.process.stdin.flush()

    def ExpectFirstFloor(self, pixels):
        failure = ExpectFrame(pixels)
        if failure is None and not Found(
            pixels, FLOOR_X, FLOOR_Y, FLOOR_WIDTH, FLOOR_HEIGHT, GLOW
        ):
            failure = "the wick-keeper's glow is not on the floor"
        if failure is None and not Found(
            pixels, FLOOR_X, FLOOR_Y, FLOOR_WIDTH, FLOOR_HEIGHT, LIT_FLOOR
        ):
            failure = "no lit floor is drawn"
        if failure is None and DarkShare(pixels) < DARK_FRACTION:
            failure = (
                f"only {DarkShare(pixels):.2f} of the floor is dark at the "
                "start"
            )
        if failure is None and not Found(
            pixels, PANEL_X + 10, PANEL_Y + 8, 20, 20, 0xD83C3C
        ):
            failure = "the heart icon is not in the panel"
        if failure is None:
            self.first_floor = FloorBytes(pixels)
        return failure

    def ExpectFirstFloorAgain(self, pixels):
        failure = ExpectFrame(pixels)
        if failure is None and FloorBytes(pixels) != self.first_floor:
            failure = "the floor differs from the first run of seed 7"
        return failure

    def ExpectFloorClosed(self, pixels):
        if FloorBytes(pixels) != self.first_floor:
            return "the floor was not redrawn after the overlay closed"
        return None

    def ExpectWalked(self, pixels):
        floor = FloorBytes(pixels)
        if floor == self.first_floor:
            return "the floor did not change after 20 arrow presses"
        self.last_floor = floor
        return None

    def ExpectResumed(self, pixels):
        failure = ExpectFrame(pixels)
        if failure is None and FloorBytes(pixels) != self.last_floor:
            failure = "the resumed floor differs from the floor before Esc"
        return failure

    def Selected(self, pixels, row):
        return PixelAt(pixels, BOX_X + 19, BOX_Y + 108 + 26 * row) == SELECTION

    def ExpectItems(self, pixels):
        failure = ExpectBox(pixels)
        if failure is None and not self.Selected(pixels, 0):
            failure = "the first pack row is not highlighted"
        if failure is None and not Found(
            pixels, BOX_X + 24, BOX_Y + 104, 16, 16, 0x14101C
        ):
            failure = "the first pack row shows no item icon"
        if failure is None:
            self.pack_box = BoxBytes(pixels)
        return failure

    def ExpectSecondRow(self, pixels):
        if not self.Selected(pixels, 1) or self.Selected(pixels, 0):
            return "the highlight did not move to the second row"
        self.pack_box = BoxBytes(pixels)
        return None

    def ExpectUsed(self, pixels):
        if BoxBytes(pixels) == self.pack_box:
            return "Enter changed nothing in the pack"
        self.pack_box = BoxBytes(pixels)
        return None

    def ExpectDropped(self, pixels):
        if BoxBytes(pixels) == self.pack_box:
            return "D changed nothing in the pack"
        return None

    def ExpectNotice(self, pixels):
        failure = self.ExpectFirstFloorAgain(pixels)
        if failure is None and not Found(
            pixels, PANEL_X + 448, PANEL_Y + 4, 500, 70, TEXT
        ):
            failure = "no message tells why the save was set aside"
        return failure

    def Leave(self, session, events, what, keys=("esc",)):
        events.origin = len(events.Text())
        self.Tap(*keys)
        if events.WaitText(SHELL_PROMPT_TEXT, PROMPT_SECONDS) is None:
            raise StepFailed(f"the prompt did not return after {what}")
        self.Step(f"after {what}", ExpectTextAgain)

    def Ask(self, session, events, command, pattern):
        events.origin = len(events.Text())
        self.Type(session, command)
        found = events.WaitText(pattern, PROMPT_SECONDS)
        if found is None:
            raise StepFailed(
                f"{command.strip()!r} gave no line {pattern.pattern!r}"
            )
        return found

    def RunGame(self, session):
        events = EventStream(session)
        events.origin = len(events.Text())
        self.Type(session, f"wickdeep --seed {GAME_SEED}\n")
        self.Step(
            "wickdeep's first floor", self.ExpectFirstFloor, GAME_START_SECONDS
        )
        if self.first_floor is None:
            raise StepFailed(self.failure)

        self.Tap("i")
        self.Step("the pack", ExpectBox)
        self.Tap("esc")
        self.Step("after closing the pack", self.ExpectFloorClosed)
        self.Tap("h")
        self.Step("the help", ExpectBox)
        self.Tap("spc")
        self.Step("after closing the help", self.ExpectFloorClosed)

        for key in WALK:
            self.Tap(key)
        self.Step("after 20 arrow presses", self.ExpectWalked)
        if self.last_floor is None:
            raise StepFailed(self.failure)

        before = []
        self.Dump(lambda pixels: before.append(bytes(pixels)) or None)
        self.Chord(("alt", True), ("f2", True))
        self.Step("wickdeep's console hidden", ExpectText)
        self.Chord(("f2", False), ("alt", False))
        self.Chord(("alt", True), ("f1", True))
        self.Chord(("f1", False), ("alt", False))
        self.Step(
            "redrawn after switching back",
            lambda pixels: Difference(pixels, before[0]),
        )

        self.Leave(session, events, "Esc")
        found = self.Ask(
            session, events, f"wickdeep --check-save {SAVE_FILE}\n", CHECK_LINE
        )
        if int(found.group(1)) < 1:
            raise StepFailed("the save holds no turn")

        events.origin = len(events.Text())
        self.Type(session, "wickdeep\n")
        self.Step("the resumed game", self.ExpectResumed, GAME_START_SECONDS)
        self.Leave(session, events, "the resumed game")

        self.Type(session, f"echo x >> {SAVE_FILE}\n")
        events.origin = len(events.Text())
        self.Type(session, f"wickdeep --seed {GAME_SEED}\n")
        self.Step("the new run", self.ExpectNotice, GAME_START_SECONDS)
        self.Leave(session, events, "the new run")
        self.Ask(session, events, f"wc -c {BAD_FILE}\n", BAD_LINE)
        self.Type(session, f"cp {PACK_SAVE} {SAVE_FILE}\n")
        self.RunPack(session, events)

    def RunPack(self, session, events):
        events.origin = len(events.Text())
        self.Type(session, "wickdeep\n")
        self.Step("the demo save resumed", ExpectFrame, GAME_START_SECONDS)
        self.Tap("i")
        self.Step("a pack with items", self.ExpectItems)
        self.Tap("down")
        self.Step("the second row chosen", self.ExpectSecondRow)
        self.Tap("ret")
        self.Step("an item used or equipped", self.ExpectUsed)
        self.Tap("d")
        self.Step("an item dropped", self.ExpectDropped)
        self.Tap("esc")
        self.Leave(session, events, "Ctrl+C", ("ctrl", "c"))
        found = self.Ask(
            session, events, f"wickdeep --check-save {SAVE_FILE}\n", PACK_LINE
        )
        if int(found.group(1)) <= 500:
            raise StepFailed(
                "the save made on Ctrl+C is not of the later turn"
            )

    def OnOutput(self, session, clean_output):
        if self.state != 0:
            return
        marker = clean_output.rfind(SAVE_REJECTED_LINE.encode())
        if marker < 0 or SHELL_PROMPT.search(clean_output, marker) is None:
            return
        self.state = 1
        try:
            self.OpenScreen()
            self.RunGame(session)
        except StepFailed as failure:
            if self.failure is None:
                self.failure = str(failure)
        except (
            OSError,
            RuntimeError,
            ValueError,
            json.JSONDecodeError,
        ) as error:
            self.failure = f"screendump or key injection failed: {error}"
        self.finished = True

    def Stop(self):
        self.CloseScreen()

    def Check(self, result):
        lines = result.output.replace("\r", "").split("\n")
        if SELFTEST_LINE not in lines:
            failed = [line for line in lines if "wickdeep: selftest" in line]
            return (
                f"wickdeep --selftest did not pass: {failed}"
                f"{result.limit_note}"
            )
        for seed in DEMO_RESULTS:
            if DemoLine(seed) not in lines:
                demos = [line for line in lines if "wickdeep: demo" in line]
                return (
                    f"wickdeep --demo --seed {seed} gave no line "
                    f"{DemoLine(seed)!r}: {demos}{result.limit_note}"
                )
        for expected in (SAVE_OK_LINE, SAVE_REJECTED_LINE):
            if expected not in lines:
                saves = [line for line in lines if "wickdeep: save" in line]
                return (
                    f"wickdeep --check-save gave no line {expected!r}: "
                    f"{saves}{result.limit_note}"
                )
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        if self.failure is not None:
            return self.failure
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        return None
