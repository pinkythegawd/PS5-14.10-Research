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
| 5 | `cap_create_shortfall`: the cap create batch fell short of `max_threads+2` outcomes — a worker creation failed before the cap could be exercised |

Every failing path prints a one-line `verdict=... rc=N` on stderr. Codes 0-3
are frozen; rc=4 is reserved for the generation control only; rc=5 is reserved
for the cap create shortfall only.

### P1 control order and handover command

Default `--control all` order is **serialized → generation → cap**, after
preflights 1-4. Every control writes a `record:"control"` line with
`phase:"start"` before its body runs and a closing `record:"control"` line
after it, both flushed — the JSONL line order is therefore evidence of the
dispatch order on the console.

**both_zero abort propagation (rule):** any `both_zero` in the serialized
control is run-terminating. The serialized closing control line is written
first (`pass:false`), then the run exits with `verdict=serialized_control_failed
rc=2`; the generation control and the cap control do **not** run, the pool
drains, and nothing is written to JSONL after the serialized closing line.
The stop is unconditional — there is no flag that disables it.

The handover command for the console slice:

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

`--control cap` runs a create batch of `max_threads + 2` assignments. Every
outcome is either a job created on a worker or a cap trip (pool full);
`creates_ok` counts both. Two asserts, in order:

1. `creates_ok == max_threads + 2` — its own verdict
   `verdict=cap_create_shortfall` rc=5 if it falls short (a worker creation
   failed before the cap could be exercised).
2. `cap_hits` advanced by `>= 2` — the cap-logic assert
   (`verdict=cap_control_failed` rc=2).

Both numbers (`creates_ok`, `cap_hits_delta`) are reported in the closing
control line and on stdout. The no-op jobs issue no AIO syscalls, the control
fully drains before returning (every assigned worker finished and idle,
`parked_live` == 0), it skips preflight 4, and is host-verifiable — run it on
the host build and on the console with `--max-threads 2` before the full P1
run. The rc=5 shortfall path needs a failing `pthread_create` and is verified
by code review, not on the host.
