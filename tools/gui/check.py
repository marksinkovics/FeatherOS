#!/usr/bin/env python3
"""Checks the design tokens (CTest gui.tokens): the generated C++ is
current, every value in the prototype's theme blocks, accent table and :root
metrics equals tokens.json, and all themes and accents pass 4.5:1 for the
pairs of docs/design/GUI.md section 2.

The script reports a failure and never changes a colour."""

from __future__ import annotations

import re
import sys
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path

import tokens

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PROTOTYPE = ROOT / "docs" / "design" / "gui" / "prototype.html"
FONTS = ROOT / "initramfs" / "fonts"

MINIMUM_CONTRAST = 4.5
# (foreground, background), the 14 pairs of GUI.md section 2.
THEME_PAIRS = (
    [("text", background) for background in ("surface", "bar", "raised")]
    + [
        ("muted", background)
        for background in ("surface", "bar", "raised", "sunken")
    ]
    + [("on_accent", "accent"), ("accent", "bar"), ("accent", "surface")]
    + [(role, "surface") for role in ("ok", "warn", "bad", "info")]
)
# What an accent replaces, checked on every theme of the matching mode.
ACCENT_PAIRS = (
    ("on_accent", "accent"),
    ("accent", "bar"),
    ("accent", "surface"),
)

# The prototype's CSS property for each colour token.
PROTOTYPE_TOKEN_NAMES = {
    name: name.replace("_", "-") for name in tokens.COLOR_TOKENS
}


def number(value: str) -> int:
    return int(re.match(r"\d+", value).group())


def expand_hex(value: str) -> str:
    value = value.lower()
    if re.fullmatch(r"#[0-9a-f]{3}", value):
        return "#" + "".join(c * 2 for c in value[1:])
    if re.fullmatch(r"#[0-9a-f]{6}", value) or re.fullmatch(
        r"#[0-9a-f]{8}", value
    ):
        return value
    raise ValueError(f"not a colour: {value}")


def parse_rgba(value: str) -> tuple[str, int]:
    match = re.fullmatch(
        r"rgba\((\d+),(\d+),(\d+),([\d.]+)\)", value.replace(" ", "")
    )
    if not match:
        raise ValueError(f"not rgba(): {value}")
    red, green, blue, alpha = match.groups()
    rgb = f"#{int(red):02x}{int(green):02x}{int(blue):02x}"
    return rgb, int(
        (Decimal(alpha) * 255).quantize(Decimal(1), rounding=ROUND_HALF_UP)
    )


def parse_prototype(text: str) -> dict:
    themes = {}
    for match in re.finditer(
        r"\[data-fos=(\w+)\]\{color-scheme:(\w+);([^}]*)\}", text
    ):
        theme_id, scheme, body = match.groups()
        props = dict(re.findall(r"--([\w-]+):([^;]+)", body))
        themes[theme_id] = {"dark": scheme == "dark", "props": props}

    root = re.search(
        r":root\{color-scheme:dark;--s-bg[^}]*\}", text, re.S
    ).group(0)
    root_props = dict(re.findall(r"--([\w-]+):([^;}]+)", root))

    acc_match = re.search(r"const ACC = (\[.*?\]);\n", text)
    accents = []
    for entry in re.findall(
        r"\['(\w+)','([^']+)'(?:,\['(#\w+)','(#\w+)'\],\['(#\w+)','(#\w+)'\])?\]",
        acc_match.group(1),
    ):
        accents.append(entry)

    names = dict(
        re.findall(
            r"(\w+):\['([^']+)','(?:dark|light)'\]",
            re.search(r"const V = \{(.*?)\};", text).group(1),
        )
    )
    families = [
        re.findall(r"'([^']+)'", row)
        for row in re.findall(
            r"\[('[^\]]+)\]",
            re.search(r"const FAMS = \[(.*?)\];", text).group(1),
        )
    ]

    apps = {}
    apps_block = re.search(r"const APPS = \{(.*?)\n\};", text, re.S).group(1)
    for app_id, color in re.findall(
        r"^\s*(\w+):\{name:'[^']*'.*?color:'(#\w+)'", apps_block, re.M
    ):
        apps[app_id] = color

    shadows = {
        (int(y), int(blur))
        for y, blur in re.findall(
            r"box-shadow:0 (\d+)px (\d+)px var\(--sh\)", text
        )
    }

    return {
        "themes": themes,
        "root": root_props,
        "accents": accents,
        "names": names,
        "families": families,
        "apps": apps,
        "shadows": shadows,
        "button_height": re.search(r"\n\.btn\{height:(\d+)px", text).group(1),
        "title_bar_height": re.search(r"\n\.tb\{height:(\d+)px", text).group(
            1
        ),
        "menu_row_height": re.search(
            r"\n\.mi\{[^}]*?height:(\d+)px", text
        ).group(1),
        "contrast_border": re.search(
            r"\[data-fos=hcd\] \.win[^{]*\{border-width:(\d+)px\}", text
        ).group(1),
    }


def compare_prototype(data: dict, proto: dict) -> list[str]:
    problems = []

    def expect(what: str, have: object, want: object) -> None:
        if have != want:
            problems.append(
                f"{what}: tokens.json has {have!r}, the prototype has {want!r}"
            )

    themes = {t["id"]: t for t in data["themes"]}
    expect("theme ids", sorted(themes), sorted(proto["themes"]))
    for theme_id, theme in themes.items():
        block = proto["themes"].get(theme_id)
        if block is None:
            continue
        props = block["props"]
        expect(f"{theme_id} dark", theme["dark"], block["dark"])
        expect(f"{theme_id} name", theme["name"], proto["names"].get(theme_id))
        for token in tokens.COLOR_TOKENS:
            want = expand_hex(props[PROTOTYPE_TOKEN_NAMES[token]])
            expect(f"{theme_id} {token}", theme["colors"][token], want)
        for key in ("w1", "w2", "w3"):
            expect(
                f"{theme_id} {key}",
                theme["wallpaper"][key],
                expand_hex(props[key]),
            )
        feather = expand_hex(props["wf"])
        expect(f"{theme_id} wf", theme["wallpaper"]["wf"], feather[:7])
        expect(
            f"{theme_id} wf alpha",
            theme["wallpaper"]["wf_alpha"],
            int(feather[7:], 16),
        )
        rgb, alpha = parse_rgba(props["sh"])
        expect(f"{theme_id} shadow", theme["shadow"]["rgb"], rgb)
        expect(f"{theme_id} shadow alpha", theme["shadow"]["alpha"], alpha)

    families = [
        [f["id"], f["name"], f["dark"], f["light"]] for f in data["families"]
    ]
    expect("families", families, proto["families"])
    for theme in data["themes"]:
        family = next(
            f for f in data["families"] if f["id"] == theme["family"]
        )
        if theme["id"] not in (family["dark"], family["light"]):
            problems.append(
                f"{theme['id']} is not in its family {family['id']}"
            )

    accents = data["accents"]
    expect(
        "accent ids",
        [a["id"] for a in accents],
        [a[0] for a in proto["accents"]],
    )
    for accent, (_, name, d_accent, d_on, l_accent, l_on) in zip(
        accents, proto["accents"]
    ):
        expect(f"accent {accent['id']} name", accent["name"], name)
        if d_accent:
            expect(
                f"accent {accent['id']} dark",
                [accent["dark"]["accent"], accent["dark"]["on_accent"]],
                [d_accent, d_on],
            )
            expect(
                f"accent {accent['id']} light",
                [accent["light"]["accent"], accent["light"]["on_accent"]],
                [l_accent, l_on],
            )

    root = proto["root"]
    metrics = data["metrics"]
    expect("bar_height", metrics["bar_height"], number(root["bar-h"]))
    for key, name in (
        ("radius_small", "r-s"),
        ("radius_medium", "r-m"),
        ("radius_large", "r-l"),
    ):
        expect(key, metrics[key], number(root[name]))
    expect(
        "title_bar_height",
        metrics["title_bar_height"],
        int(proto["title_bar_height"]),
    )
    expect(
        "control_height",
        metrics["control_height"],
        int(proto["button_height"]),
    )
    expect(
        "menu_row_height",
        metrics["menu_row_height"],
        int(proto["menu_row_height"]),
    )
    expect(
        "contrast_border_width",
        metrics["contrast_border_width"],
        int(proto["contrast_border"]),
    )
    for key, name in (("fast", "fast"), ("base", "base"), ("slow", "slow")):
        expect(
            f"duration {key}", data["durations_ms"][key], number(root[name])
        )
    easing = re.fullmatch(r"cubic-bezier\(([^)]*)\)", root["ease"])
    values = [
        int((Decimal(v) * 1000).to_integral_value())
        for v in easing.group(1).split(",")
    ]
    expect("easing", data["easing_milli"], values)

    expect(
        "app tile colours",
        {a["id"]: a["color"] for a in data["app_tiles"]},
        {k: expand_hex(v) for k, v in proto["apps"].items()},
    )
    expect(
        "shadow sizes",
        sorted((s["offset_y"], s["blur"]) for s in data["shadows"]),
        sorted(proto["shadows"]),
    )
    return problems


def linear(channel: int) -> float:
    value = channel / 255
    return (
        value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    )


def luminance(rgb: str) -> float:
    value = tokens.parse_hex(rgb)
    red, green, blue = (value >> 16) & 255, (value >> 8) & 255, value & 255
    return (
        0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue)
    )


def contrast(first: str, second: str) -> float:
    high, low = sorted((luminance(first), luminance(second)), reverse=True)
    return (high + 0.05) / (low + 0.05)


def check_contrast(data: dict) -> list[str]:
    problems = []

    def test(label: str, foreground: str, background: str) -> None:
        ratio = contrast(foreground, background)
        if ratio < MINIMUM_CONTRAST:
            problems.append(
                f"contrast {ratio:.2f}:1 below {MINIMUM_CONTRAST}: {label}"
            )

    for theme in data["themes"]:
        colors = theme["colors"]
        for foreground, background in THEME_PAIRS:
            test(
                f"{theme['id']} {foreground} on {background}",
                colors[foreground],
                colors[background],
            )
        shade_key = "dark" if theme["dark"] else "light"
        for accent in data["accents"][1:]:
            shade = accent[shade_key]
            replaced = {
                **colors,
                "accent": shade["accent"],
                "on_accent": shade["on_accent"],
            }
            for foreground, background in ACCENT_PAIRS:
                test(
                    f"{theme['id']} with accent {accent['id']}: "
                    f"{foreground} on {background}",
                    replaced[foreground],
                    replaced[background],
                )
    return problems


def check_generated() -> list[str]:
    problems = []
    for path, text in tokens.outputs().items():
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            problems.append(
                f"{path.relative_to(ROOT)} is not what tokens.py generates"
            )
    return problems


def check_fonts(data: dict) -> list[str]:
    if not FONTS.is_dir():
        return []
    return [
        f"type role {role}: no {font}.ffnt in initramfs/fonts"
        for role, font in data["type_roles"].items()
        if not (FONTS / f"{font}.ffnt").exists()
    ]


def main() -> int:
    data = tokens.load()
    problems = check_generated()
    problems += compare_prototype(
        data, parse_prototype(PROTOTYPE.read_text(encoding="utf-8"))
    )
    problems += check_contrast(data)
    problems += check_fonts(data)
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(
        f"tokens ok: {len(data['themes'])} themes, "
        f"{len(data['accents']) - 1} accents, {len(THEME_PAIRS)} theme pairs"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
