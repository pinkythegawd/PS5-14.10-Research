#!/usr/bin/env python3
import json
import sys
from pathlib import Path

rows = [json.loads(line) for line in
        Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
        if line.strip()]
attempts = [r for r in rows if r.get("record") == "attempt"]
controls = [r for r in rows if r.get("record") == "control"]

assert len(attempts) == 42, f"expected 42 attempt rows, got {len(attempts)}"
assert all(r.get("synthetic") is True for r in attempts), \
    "every synthetic row must carry synthetic:true"
assert all(r.get("clock_src") == "libc" for r in attempts), \
    "host build must stamp clock_src=libc on every attempt"
assert all(r.get("sys624_unverified_on_console") is True for r in attempts), \
    "sys624_unverified_on_console:true missing on attempts"
assert [r["seq"] for r in attempts] == list(range(1, 43)), \
    "seq column must round-trip 1..42"

counts = {}
for r in attempts:
    counts[r["outcome"]] = counts.get(r["outcome"], 0) + 1
assert counts == {"both_zero": 3, "both_esrch": 2, "esrch_main": 10,
                  "esrch_racer": 10, "other": 15, "timeout": 2}, counts

by_seq = {r["seq"]: r for r in attempts}
derived = by_seq[39]
assert derived["outcome"] == "timeout", derived
assert derived["window_us"] == 6000, derived
assert derived["main_err"] == 0 and derived["racer_err"] == 0, derived
assert derived["main_ret"] == 0 and derived["racer_ret"] == 0, derived
overridden = by_seq[40]
assert overridden["outcome"] == "timeout", overridden
assert overridden["window_us"] == 0 and overridden["main_err"] == 22, \
    overridden

notes = [r["note"] for r in attempts if r.get("note")]
assert len(notes) == 2, f"expected 2 note rows, got {notes}"
assert 'say "hello" \\ backslash' in notes, \
    "quote/backslash note must round-trip through the escaper"
assert 'note with, comma and "quotes"' in notes, \
    "commas inside a note must survive the CSV split"

assert len(controls) == 1, f"expected 1 control record, got {len(controls)}"
c = controls[0]
assert c["control"] == "synthetic", c
assert c["attempts"] == 42 and c["both_zero"] == 3, c
assert c["pass"] is False, c
assert c["clock_src"] == "libc", c

print("check_synthetic: ok")
