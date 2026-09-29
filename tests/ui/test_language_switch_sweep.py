# SPDX-License-Identifier: GPL-3.0-or-later

"""Every screen re-translates its labels on a live language switch.

XML re-translates what it bound through translation_tag; text C++ set on a
label (or a tag C++ then overwrote) stays in the language it was written in.
Panels and most overlays are built once and kept, so a label that misses the
switch still reads English after it. This walks every screen the screenshot
recipes reach, in English, switches to Russian the way a user does, walks them
again, and reports each label whose English text is a translation key but
still reads English.
"""

from __future__ import annotations

import re
import shlex
from pathlib import Path

import pytest
import yaml

from helix.app import HelixCtlError

REPO_ROOT = Path(__file__).resolve().parents[2]
RECIPES = REPO_ROOT / "scripts" / "screenshot-recipes.sh"
RUSSIAN = REPO_ROOT / "translations" / "ru.yml"

# Index of Русский in SystemSettingsManager's language dropdown.
RU_INDEX = 4

# Edit-mode callouts drive raw pointer gestures, not screens of their own.
SKIP = {"callouts-2x2", "callouts-4x2", "callouts-untagged"}

DURATION = re.compile(r"^\d+(h|h \d+m| min)$")


def _recipes() -> list[tuple[str, list[list[str]]]]:
    text = RECIPES.read_text()
    body = text.split('SCREENSHOT_RECIPES="', 1)[1].split('"', 1)[0]
    recipes = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        token, _, steps = line.partition(" ")
        if token in SKIP:
            continue
        recipes.append((token, [shlex.split(s) for s in steps.split(";") if s.strip()]))
    return recipes


def _run(app, steps: list[list[str]]) -> bool:
    app.reset()
    app.wait_idle()
    for step in steps:
        try:
            app.ctl(*step)
        except HelixCtlError:
            return False  # not reachable on this mock; nothing to compare
        app.wait_idle()
    return True


def _labels(app) -> dict[str, str]:
    out = {}
    for widget in app.ls().get("widgets", []):
        if widget.get("type") != "label":
            continue
        try:
            out[widget["path"]] = app.text(widget["path"])
        except HelixCtlError:
            continue
    return out


def test_every_screen_retranslates_on_a_language_switch(fresh_helix_app):
    app = fresh_helix_app
    ru = yaml.safe_load(RUSSIAN.read_text())["translations"]

    before = {}
    unreachable = []
    for token, steps in _recipes():
        if _run(app, steps):
            before[token] = _labels(app)
        else:
            unreachable.append(token)

    app.reset()
    app.navigate("settings")
    app.wait_idle()
    app.click("row_language_time")
    app.wait_idle()
    app.ctl("set_value", "row_language", RU_INDEX)
    app.wait_idle()

    stale = []
    checked = 0
    for token, steps in _recipes():
        if token not in before or not _run(app, steps):
            continue
        after = _labels(app)
        for path, en in before[token].items():
            want = ru.get(en)
            if not en.strip() or not want or want == en:
                continue
            checked += 1
            now = after.get(path)
            if now is None:
                continue  # not on screen after the switch: rebuilt, nothing stale
            # Still exactly the English text is the stale signature; a different
            # rendering of the right translation (case, a count beside it) is not.
            # Durations are formatted without translation (format::duration_*),
            # so one that happens to equal a key ("5 min") is data, not stale.
            if now == en and not DURATION.match(en):
                stale.append(f"{token}: {path}: {en!r} still reads {now!r}, want {want!r}")

    report = "\n".join(stale)
    print(f"\n{checked} translatable labels checked, {len(stale)} stale; "
          f"unreachable on this mock: {', '.join(unreachable) or 'none'}\n{report}")
    assert checked > 0
    assert not stale, f"{len(stale)} stale labels:\n{report}"
