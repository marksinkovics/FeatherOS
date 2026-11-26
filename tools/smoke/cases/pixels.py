import json
import re
import subprocess
import tempfile
from pathlib import Path

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case, SkipCase
from smoke.cases.screen import Difference, ReadPpm, ScreenCase
from smoke.script import WaitUntil
from smoke.terminal import SHELL_PROMPT

PIXELS_TIMEOUT_SECONDS = 300
ROOT = Path(__file__).resolve().parents[3]
UNIT_BUILD = ROOT / "build" / "unit-tests-ninja"
GUIRENDER = UNIT_BUILD / "guirender"
SYSTEM = ROOT / "initramfs"
PAGES = (1, 2, 3, 4)
THEMES = ("dusk", "hcl")
SIZE = "1024x768"
DRAWN = re.compile(rb"^uicard: page (\d+) drawn in (\d+) ms\r?$", re.MULTILINE)
RUNS = tuple((page, theme) for theme in THEMES for page in PAGES)


def Command(page, theme):
    return f"uicard --page {page} --theme {theme}\n"


class Pixels(ScreenCase, Case):
    """Runs uicard for each page of the test card in dusk and in the light
    high-contrast theme, takes a screendump while it owns the screen and
    compares it byte for byte with what guirender, the host build of the same
    drawing code, writes for that page and theme, then presses Esc and expects
    the prompt back."""

    serial_input = True
    timeout = PIXELS_TIMEOUT_SECONDS
    passed = (
        "uicard's four test card pages in dusk and the light high-contrast "
        "theme matched the host render byte for byte, and Esc returned the "
        "prompt each time"
    )

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.references = tempfile.TemporaryDirectory(prefix="fr-")
        self.step = 0
        self.waiting_for_prompt = False
        self.prompt_from = 0
        self.finished = False
        self.opened = False
        self.expected = {}

    def Start(self):
        if not (UNIT_BUILD / "build.ninja").exists():
            raise RuntimeError(
                "guirender is not built: run ./build.sh unit-test first"
            )
        built = subprocess.run(
            ["cmake", "--build", str(UNIT_BUILD), "--target", "guirender"],
            capture_output=True,
            text=True,
            check=False,
        )
        if built.returncode != 0 or not GUIRENDER.exists():
            raise SkipCase(
                "guirender did not build: "
                + built.stdout[-400:]
                + built.stderr
            )
        for page, theme in RUNS:
            path = Path(self.references.name) / f"{theme}-{page}.ppm"
            subprocess.run(
                [
                    str(GUIRENDER),
                    "card",
                    "--size",
                    SIZE,
                    "--page",
                    str(page),
                    "--theme",
                    theme,
                    "--accent",
                    "default",
                    "--system",
                    str(SYSTEM),
                    "--out",
                    str(path),
                ],
                check=True,
            )
            self.expected[(page, theme)] = ReadPpm(path)

    def Script(self, session):
        script = []
        for index, (page, theme) in enumerate(RUNS):
            script.append(Command(page, theme).encode())
            script.append(
                WaitUntil(
                    lambda index=index: self.step > index or self.failure
                )
            )
        return script

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.finished

    def OnOutput(self, session, clean_output):
        if self.finished:
            return
        if not self.opened:
            try:
                self.OpenScreen()
            except OSError as e:
                self.failure = f"cannot open QMP: {e}"
                self.finished = True
                return
            self.opened = True
        if self.waiting_for_prompt:
            if SHELL_PROMPT.search(clean_output, self.prompt_from) is None:
                return
            self.waiting_for_prompt = False
            self.step += 1
            self.finished = self.step == len(RUNS)
            return
        drawn = list(DRAWN.finditer(clean_output))
        if len(drawn) <= self.step:
            return
        page, theme = RUNS[self.step]
        report = drawn[self.step]
        try:
            if int(report.group(1)) != page:
                self.failure = (
                    f"uicard reported page {report.group(1).decode()}, "
                    f"expected {page}"
                )
            else:
                self.Step(
                    f"page {page} in {theme}",
                    lambda pixels: Difference(
                        pixels, self.expected[(page, theme)]
                    ),
                )
            self.Tap("esc")
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as e:
            self.failure = f"screendump failed: {e}"
        if self.failure is not None:
            self.finished = True
            return
        self.waiting_for_prompt = True
        self.prompt_from = report.end()

    def Stop(self):
        self.CloseScreen()
        self.references.cleanup()

    def Check(self, result):
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        if self.failure is not None:
            return self.failure
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        return None
