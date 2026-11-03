#!/usr/bin/env python3
import json
import sys
from pathlib import Path

p = Path(sys.argv[1])
rows = [json.loads(line) for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]
ids = [r for r in rows if r.get("record") == "p2_id"]
wins = [r for r in rows if r.get("record") == "p2_winner"]
summary = [r for r in rows if r.get("record") == "p2_summary"]
assert len(ids) == 5, f"expected 5 p2_id rows, got {len(ids)}"
assert len(wins) == 3, f"expected 3 p2_winner rows, got {len(wins)}"
assert summary and summary[-1]["frozen_candidates"] == 3, summary
suspects = [r for r in ids if r["false_winner_suspect"]]
assert len(suspects) == 1, f"expected 1 suspect, got {len(suspects)}"
assert suspects[0]["id"] == 17158, suspects[0]
assert all(r["winner_role"] == "racer" for r in wins), wins
assert sorted(r["winner_id"] for r in wins) == [16901, 17157, 17413], wins
print("check_p2: ok")
