#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A backend that reads a firmware spool id must reconcile the lane binding.

AmsBackend::reconcile_lane_binding() is what notices a lane re-bound or ejected
behind the app's back. It only works where the firmware states a spool id, so it
is wired per backend, beside the parser that reads the id. A backend whose
firmware starts reporting ids gains a parser with no reason to notice that the
call belongs next to it, and its bindings then go stale and paint forever
(prestonbrown/helixscreen#1645).

A backend (include/ams_backend_<name>.h + src/printer/ams_backend_<name>.cpp)
must call reconcile_lane_binding() when either:
  - its source reads a JSON key naming a spool id ("spool_id", "gate_spool_id",
    "spoolman_id", ...), or
  - it overrides printer_reports_spool_ids() to return true.

Usage: check_lane_binding_reconcile.py [--root DIR]
"""
import re
import sys
from pathlib import Path

COMMENT = re.compile(r"//.*$", re.MULTILINE)
SPOOL_ID_KEY = re.compile(r'"[A-Za-z_]*spool[A-Za-z_]*id[A-Za-z_]*"', re.IGNORECASE)
REPORTS_IDS = re.compile(r"printer_reports_spool_ids\s*\(\s*\)[^{;]*\{\s*return\s+true\b")
RECONCILE = re.compile(r"\breconcile_lane_binding\s*\(")


def scan(root: Path) -> list[str]:
    hits = []
    for cpp in sorted((root / "src" / "printer").glob("ams_backend_*.cpp")):
        name = cpp.stem[len("ams_backend_"):]
        files = [cpp, root / "include" / f"ams_backend_{name}.h"]
        code = "\n".join(COMMENT.sub("", f.read_text()) for f in files if f.exists())
        reasons = []
        key = SPOOL_ID_KEY.search(code)
        if key:
            reasons.append(f"reads {key.group(0)}")
        if REPORTS_IDS.search(code):
            reasons.append("printer_reports_spool_ids() returns true")
        if reasons and not RECONCILE.search(code):
            hits.append(f"{cpp.relative_to(root)}: {', '.join(reasons)}")
    return hits


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    if len(argv) == 3 and argv[1] == "--root":
        root = Path(argv[2]).resolve()
    elif len(argv) != 1:
        print(__doc__)
        return 2

    hits = scan(root)
    if hits:
        print(f"AMS backends that read spool ids without reconcile_lane_binding(): {len(hits)}")
        for h in hits:
            print("  " + h)
        print()
        print("  Call reconcile_lane_binding(slot, firmware_spool_id) where the id is parsed,")
        print("  as ams_backend_afc.cpp, ams_backend_cfs.cpp and ams_backend_happy_hare.cpp do.")
        return 1

    print("✅ every AMS backend that reads a spool id reconciles its lane binding")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
