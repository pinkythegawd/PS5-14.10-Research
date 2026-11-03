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
writer, the config/`--set` contract, and the P2 parser on a frozen fixture.

## P1 (first slice: preflights + serialized control)

```sh
./prolapse_p1p2.elf --stage p1 \
  --max-threads 64 \
  --jsonl /data/prolapse/p1.jsonl \
  --seed 1
```

The run is preflights, then the serialized control, then the generation
control, then teardown. No strategy runs. `race.control_rounds` defaults to
500; override with `--set race.control_rounds=1000`.

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

### Controls (run before any race)

- Serialized: one caller, two sequential deletes, same id. Expect `~0`
  `both_zero`; any `both_zero` aborts the run — P1 would then be counting
  something other than the race. Every attempt emits one JSONL attempt line
  (`strategy:"serialized_control"`, `control:true`) and the run ends with the
  outcome histogram on stdout.
- Generation: bump generation bits and delete the bumped id. Expect
  `0x80020003` on the loser.

Both controls also emit one `record:"control"` summary line and must pass.

### Parked-thread cap

`--max-threads` defaults to 64 and is a hard cap on active plus parked workers.
Exceeding it is a loud failure, not degradation. The round line prints
`parked_live`; teardown drains to zero before exit and prints
`parked_live=0 drain=ok`. `--max-threads 2` is the acceptance cap test.

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

## Acceptance status

Host checks are green. Console acceptance for the P1 slice requires the SDK, a
14.00 V59 console, and capture of the four preflights, the serialized control
at >=500 attempts with its histogram and zero `both_zero`, the generation
control, and the `--max-threads 2` cap trip. Strategy acceptance (1000 attempts
per strategy, one in-run dead-end append, three frozen `both_zero` events
through P2) is deferred with the strategies. Those numbers are not fabricated
here.
