# Prolapse P1/P2 (14.00 V59 dev path)

Standalone payload for the instrumented duplicate-delete race (P1) and the
frozen-candidate winner log (P2). It is **not** an exploit. It does not perform
KRW, does not use `lane_VA`, and does not touch WebKit.

## Scope and P3 status

- Active profile: `target.profile=14.00_v59`.
- P2 is implemented in this tree. P1 is implemented as its first slice only:
  the four A4 preflights, the JSONL writer, and the serialized control (single
  thread, two sequential deletes, no racer). The race strategies
  (`same_barrier`, `split_barrier`, `split_stagger`) are deferred until the
  serialized control validates on 14.00 V59; their spec is unchanged in
  `docs/PROLAPSE_P1_P3_DESIGN.md`.
- **P3 is not implemented and is blocked.** P3 needs the req2 zone from IDA on
  `kernel_1400.elf` or the A1 optlen sweep. The current `0x58` size class is
  unproven; see `docs/PROLAPSE_OFFSETS.md` A1. No P1/P2 code inlines a zone or
  optlen. A flat P3 sweep means the `0x58` size assumption is wrong, not just
  the bucket.
- 14.10 offsets are not ported here. The ledger's 14.00 column says
  `verify on target` for the AIO syscall numbers; the payload uses the 14.10
  number only as an explicitly-labelled fallback and prints it at startup.
  Supply `--set sys.aio_multi_delete=...` etc. to override.

## Build

PS5 payload SDK:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-dev/sdk
make                  # builds prolapse_p1p2.elf
```

Host checks (no SDK, no console):

```sh
make check            # generated ledger header + C core tests + P2 fixture
```

`make check` verifies the ledger header is not stale, the classifier and JSONL
writer, the config/`--set` contract, the P2 parser on a frozen fixture, and the
synthetic harness: the 42-row fixture exercises all six outcomes, the
`both_zero` abort, the histogram, and a note containing a quote and a backslash
so JSON escaping is proven.

## P1 (first slice: preflights + serialized control)

```sh
./prolapse_p1p2.elf --stage p1 \
  --max-threads 64 \
  --jsonl /data/prolapse/p1.jsonl \
  --seed 1
```

The run is preflights, then the serialized control, then the generation
control, then the cap control, then teardown. No strategy runs.
`race.control_rounds` defaults to 500; override with `--set
race.control_rounds=1000` or `--control-rounds 1000`.

Serialized control per round: submit `AIO_CMD_READ|AIO_CMD_MULTI` over a TCP
loopback pair with `SO_LINGER {1,1}` on the target; `aio_cancel`; `aio_poll`
until state 3 or 4; two sequential `aio_multi_delete` calls on the same id
from one worker, no racer; classify; one JSONL attempt line. The target user
fd is closed after submit; the peer is the live handle used to release a
parked worker with `SO_LINGER {0,0}`.

### Preflights (must print all four)

1. `clock_gettime(232)` returns 0 and advances in `tv_nsec`.
2. `cpuset_getaffinity(3,1,-1,...)` returns at least two allowed CPUs; the
   lowest two are selected.
3. `RLIMIT_NOFILE` is raised to cover `spray.fanout` at the configured batch and
   thread counts plus headroom.
4. `SO_LINGER {1,1}` parks one worker past 5 ms, then `{0,0}` releases it and it
   returns.

Any failure aborts before racing. A failed parking preflight is a stop
condition: later numbers would be meaningless.

Exit codes distinguish the failure kinds with a one-line verdict:

- `0` = success.
- `1` = synthetic abort: a `both_zero` row in `--synthetic` (host only).
- `2` = `verdict=preflight_failed` (a real failure on a console where the
  check ran but did not pass) or `verdict=serialized_control_failed` /
  `verdict=cap_control_failed`.
- `3` = `verdict=preflight_unavailable_in_build`: the check cannot run in this
  build. On the host build, preflight 4 (linger parking) always exits 3 because
  AIO syscalls are ENOSYS by design.
- `4` = `verdict=generation_control_failed`: the bumped-generation delete did
  not reproduce `0x80020003`. Kept distinct from 2 because the generation check
  is the one 14.10 ground truth the whole race depends on.

### Controls (run before any race)

`--control all|serialized|generation|cap` selects which controls run (default
`all` = serialized, then generation, then cap). A failed control aborts with
its own one-line verdict (`verdict=serialized_control_failed` rc=2 /
`verdict=generation_control_failed` rc=4 / `verdict=cap_control_failed` rc=2),
distinct from the preflight verdicts.

- Serialized: one caller, two sequential deletes, same id. Expect `~0`
  `both_zero`; any `both_zero` aborts the run — P1 would then be counting
  something other than the race. The stop is unconditional and cannot be
  disabled. Every attempt emits one JSONL attempt line
  (`strategy:"serialized_control"`, `control:true`) and the run ends with the
  outcome histogram on stdout.
- Generation: bump the generation bits of the id before the second delete and
  delete the bumped id; the loser must return `0x80020003`. If that error does
  not reproduce, the run aborts with `verdict=generation_control_failed` rc=4 —
  the generation check is the one 14.10 ground truth the whole race depends on.
- Cap: assign `max_threads + 2` no-op jobs from the worker pool. The first
  `max_threads` must succeed and the last two must trip the cap
  (`pool_assign` returns `-1`, `cap_hits` advances by >=2). No AIO syscall is
  issued by the no-op jobs. It fully drains before returning: every assigned
  worker must finish and clear `active`, and `parked_live` must be 0. As a
  standalone mode `--control cap` skips preflight 4 (it does not need linger)
  so the cap trip is host-verifiable too.

All controls also emit one `record:"control"` summary line and must pass.

### Defaults (exact, as committed)

- `--control`: default `all` = serialized, generation, cap, in that order.
- `--rounds`: maps to `race.rounds`, default 50000 — the strategy budget. It is
  inert in this slice (no strategies run) and does not affect control
  attempts.
- Control attempts: `--control-rounds` / `race.control_rounds`, default 500,
  range 8-100000. The serialized and generation controls each run this many
  attempts.
- `--stop-on-first-win` / `--no-stop-on-first-win` is the strategy-stage
  first-win stop (default on) and is unrelated to `both_zero`. Unknown flags
  are rejected at parse time with a hard failure.

### Parked-thread cap

`--max-threads` defaults to 64 and is a hard cap on active plus parked workers.
Exceeding it is a loud failure, not degradation. The round line prints
`parked_live`; teardown drains to zero before exit and prints
`parked_live=0 drain=ok`. The cap trip is validated in-build by the cap control
(`--control cap`); `--max-threads 2` is the acceptance invocation.

### P1 JSONL

One `record:"attempt"` line per control round, flushed after every line.
Required fields:

`seq, strategy, id, t_start_us, t_end_us, window_us, parked_live, main_ret,
main_err, racer_ret, racer_err, outcome`

`outcome` is one of `both_zero`, `esrch_main`, `esrch_racer`, `both_esrch`,
`timeout`, `other`. For control lines `main_*` is the first sequential delete
and `racer_*` is the second. Time is `clock_gettime(232)`. Raw 624 is present
in `sys624_*` fields and labelled `vm_operation_counter_ordering_token`; it is
not used as a timer. Extra fields include `round_id`, `slot`, `batch_size`,
`main_seq`, `racer_seq`, `frozen`, `candidate`, and `control`.

Every record carries `clock_src`: `"232"` on console (raw `clock_gettime(232)`),
`"libc"` on the host build (libc `clock_gettime`). Synthetic rows additionally
carry `synthetic:true`. The `sys624_*` fields are kept but flagged
`sys624_unverified_on_console:true` on every attempt record; they are removed
only after the first console run confirms they move.

The `frozen:true`/`frozen_round` machinery exists for P2 but stays dormant
this round: the slice produces no races, so nothing is frozen.

### Dead ends

Dormant this round: the dead-end append (A6) is tied to strategy loss
thresholds and thread-cap trips, and no strategy runs. The `--dead-ends` flag
(default `docs/DEAD_ENDS.md`) is retained for when strategies return.

## P2

P2 never races and never calls AIO. It reads only frozen candidates:

```sh
./prolapse_p1p2.elf --stage p2 \
  --input /data/prolapse/p1.jsonl \
  --out /data/prolapse/p2.jsonl
```

Every id in a frozen round is logged with `id`, `ret`, `err`, `seq`, `slot`,
and both callers' errors/sequences. `both_zero` is narrow: exactly two
`err == 0` on one slot. The second zero (higher `main_seq`/`racer_seq`) is
emitted as `winner_id` with its slot. A non-zero, non-ESRCH error on the same
slot sets `false_winner_suspect:true` and records `suspect_err`; P3 will need
these marked.

`--jsonl` is also accepted as the P2 output path when `--out` is absent.

## Synthetic mode (host build only)

`--synthetic <file>` feeds a CSV table through `prolapse_classify` and the
JSONL writer with no AIO or race syscalls, so the classifier, writer, escaping,
histogram and `both_zero` abort are exercised without a console:

```sh
./build/prolapse_host --synthetic tests/fixtures/synthetic_40.csv \
  --synthetic-no-abort --jsonl /tmp/synthetic.jsonl
```

CSV columns: `seq,main_ret,main_err,racer_ret,racer_err[,window_us][,outcome][,note]`.
`window_us` feeds the windowed classifier: a row with both rets 0, both errs 0
and `window_us >= race.timeout_window_us` (default 5000) derives `timeout`
with no override. The optional `outcome` column still overrides the classifier
outright (for rows the derivation cannot express); the `note` column is free
text — everything after its comma is kept verbatim, commas included, and goes
through the JSON escaper. Blank lines, `#` comments, and a `seq,...` header
line are skipped. Every row is emitted with `synthetic:true`,
`strategy:"synthetic"`, and the CSV window in `window_us` (zeroed timers
otherwise), plus a closing `control` summary.

The windowed derivation is synthetic-only: the console P1 path keeps
caller-set timeouts (timed flags), because reclassifying a slow `both_zero` as
`timeout` would mask the serialized control's stop condition.

Any `both_zero` row aborts the run with exit 1 and
`synthetic: BOTH_ZERO ABORT` on stderr, mirroring the P1 control stop rule;
`--synthetic-no-abort` disables the abort so a fixture containing `both_zero`
rows can be classified end to end. `make check` runs both modes against
`tests/fixtures/synthetic_40.csv` (42 rows: 3 `both_zero`, 2 `both_esrch`, all
six outcomes — one `timeout` derived via `window_us`, one via override — and a
note containing a quote and a backslash).

## Acceptance status

Host checks are green, including the synthetic harness (all six outcomes,
`both_zero` abort, histogram, escaping), the exit-code split (host preflight
4 prints `verdict=preflight_unavailable_in_build` and exits 3), and the cap
control (`--control cap` passes on the host build: assignments 1..N succeed,
the last two trip the cap, all workers drained). Console acceptance for the
P1 slice requires the SDK, a 14.00 V59 console, and capture of the four
preflights, the serialized control at >=500 attempts with its histogram and
zero `both_zero`, the generation control, and the cap control. The exact
handover command is:

```sh
./prolapse_p1p2.elf --stage p1 \
  --control-rounds 500 \
  --max-threads 64 \
  --jsonl /data/prolapse/p1.jsonl \
  --seed 1
```

With the committed defaults (`--control all` = serialized → generation → cap,
`race.control_rounds` 500, `both_zero` stop unconditional) this runs:
preflights 1-4, serialized control (500 attempts), generation control (500
attempts), cap control, teardown. It does not run strategies — `--rounds`
(`race.rounds`, the strategy budget) is inert in this slice. Strategy
acceptance (1000 attempts per strategy, one in-run dead-end append, three
frozen `both_zero` events through P2) is deferred with the strategies. Those
numbers are not fabricated here.
