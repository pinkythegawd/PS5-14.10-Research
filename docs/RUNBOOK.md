# Runbook — operational rules for this tree

## Untracked probe files — never `git clean` on this tree

`probes/phase17_new_vectors/434a_sys637_store_analysis.lua` and
`probes/phase17_new_vectors/435a_umtx_op_matrix.lua` are in-progress phase17
analysis and are intentionally untracked. `git clean -xdf` (or `-x`/`-X`)
deletes them. Do not run any `git clean` variant here while they exist —
commit or move them deliberately first.

## Prolapse payload exit codes (`payloads/prolapse/`)

| rc | meaning |
|---:|---|
| 0 | success |
| 1 | synthetic abort: `both_zero` row in `--synthetic` (host only) |
| 2 | `preflight_failed` (real console preflight failure), `serialized_control_failed`, `cap_control_failed`, or config/drain failure |
| 3 | `preflight_unavailable_in_build` (host build: preflight 4 cannot run; AIO is ENOSYS by design) |
| 4 | `generation_control_failed`: the bumped-generation delete did not reproduce `0x80020003` |

Every failing path prints a one-line `verdict=... rc=N` on stderr. Codes 0-3
are frozen; rc=4 is reserved for the generation control only.

### P1 control order and handover command

Default `--control all` order is **serialized → generation → cap**, after
preflights 1-4. The handover command for the console slice:

```sh
./prolapse_p1p2.elf --stage p1 \
  --control-rounds 500 \
  --max-threads 64 \
  --jsonl /data/prolapse/p1.jsonl \
  --seed 1
```

`--control-rounds` (or `race.control_rounds`) sets the serialized and
generation attempt count (default 500). `--rounds` (`race.rounds`) is the
strategy budget and is inert in this slice — do not use it to size controls.
Unknown flags are rejected at parse time with a hard failure.

### Cap control

`--control cap` assigns `max_threads + 2` no-op jobs; the last two must trip
the parked/active cap (`cap_hits` advances by >=2). It issues no AIO syscalls,
fully drains before returning (every assigned worker finished and idle,
`parked_live` == 0), skips preflight 4, and is host-verifiable — run it on
the host build and on the console with `--max-threads 2` before the full P1
run.
