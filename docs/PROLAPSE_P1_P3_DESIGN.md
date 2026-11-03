# Prolapse P1-P3 design — native AIO delete/delete instrumentation

Date: 2026-11-03
Branch: `feat/prolapse`
Status: **design gate — no code written, built, or run**
Target: 14.00 V59 native path for development and validation; 14.10 is the
delivery profile. No WebKit, no `lane_VA`, no JS-side primitive, no KRW.

This design implements P1-P3 only. P4/P5 and any kernel read/write or payload
stage remain out of scope. The run stops with numbers and, if P2 wins, a held
overlapping fd pair from P3.

## 1. Scope and invariants

1. One standalone PS5 ELF, `prolapse_p1p3.elf`, loaded through the existing
   native/ELF loader path. It does not depend on the browser, the Guide, or
   WebKit.
2. The ELF is a measurement payload, not a jailbreak. It may issue AIO, socket,
   timing, affinity, and close syscalls. It does not perform an arbitrary free,
   arbitrary read/write, KASLR defeat, or credential patch.
3. P1 and P2 are safe to run as soon as the syscall ABI is verified. P3 refuses
   to start until the active profile has a measured `aio.req2_size` and
   `aio.req2_zone`; 14.10 has `0x58 -> 128`, 14.00 V59 is port-pending.
4. Every tunable resolves through `docs/PROLAPSE_OFFSETS.md`. Source files do
   not carry a second copy of a default.
5. A candidate win stops the run before any cleanup delete. Cleanup arrays and
   winner arrays are separate.

## 2. Preconditions and controls

### 2.1 Terminal state before the race

A delete only exercises the free branch after the request has reached a terminal
state. Each round:

1. Submit `race.batch` read requests through 669 on idle transports.
2. Call 666 (`aio_multi_cancel`) on the batch.
3. Call 664 (`aio_multi_poll`) until every state is `4` (aborted) or the
   `race.terminal_timeout_us` deadline expires.
4. Drop any round that did not reach a terminal state; count it as
   `terminal_timeout`, not as a race loss.

The reference's `aio_cancel`/`aio_poll` pair (gist lines 527-528) is a required
precondition, not an optimization.

### 2.2 Linger parking

The free path must be held inside `_fdrop` with the AIO lock dropped. The design
uses the reference's `SO_LINGER {1,1}` (gist lines 226-229, 297-298) on the
transport whose last reference the delete will drop:

1. Create the transport and set `linger.onoff=1`, `linger.seconds=1`.
2. Submit the AIO read while the user fd is open.
3. Close the user copy after submission, leaving the AIO request as the last
   reference.
4. The delete's free path performs the last `_fdrop`; the parking measurement
   records how long that call remains inside the kernel.

The default transport is connected loopback TCP because it has a real linger
path. `unix_socketpair` remains a control (`race.transport`). A run-level
parking preflight runs before the race budget: if it does not observe a delay,
P1 aborts with `parking_precondition_failed`; it does not spend the budget on
rounds that cannot exercise the window. A clean round that still reports
`parking_not_observed` is not evidence against the race. This is the
`SO_LINGER` precondition the earlier 416a/420a schedules did not combine with
terminal state.

### 2.3 Two native threads and serial controls

- Main thread and one persistent native racer, pinned according to the
  strategy. The racer is created once per run, not per round.
- Both threads receive the same `ids[]` and separate result arrays.
- Before the race budget, run three controls:
  1. **Single delete:** exactly one success and one `ESRCH`/`0x80020003`
     equivalent on a serial repeat.
  2. **Split IDs:** each thread deletes a disjoint half; both may succeed, but
     no slot has two zero results.
  3. **Duplicate in one call:** `[id, id]` in one 662 call must be
     `0, 0x80020003`, reproducing the control condition.
- If a control fails, P1 aborts with `control_failed`; it does not reinterpret
  the failure as a win.

## 3. P1 — instrumented delete/delete race

### 3.1 Round lifecycle

For each round under a fixed strategy:

1. Build a new batch of `race.batch` IDs and transports.
2. Force terminal state (section 2.1) and verify states.
3. Arm both threads on an atomic gate. No thread calls 662 before the gate.
4. Both threads call 662 once on the same `ids[]`, each writing only its own
   `ret`, `err[]`, entry timestamp, exit timestamp, and 624 samples.
5. Copy both result arrays and metadata into the round record **before any
   other delete**. There is no mop-up delete in the measurement path.
6. Classify each ID:
   - `both_zero`: both calls returned `0` for that slot. Candidate win.
   - `one_zero`: exactly one zero; the other is the expected invalid-ID error.
   - `both_error`: both nonzero.
   - `unexpected`: any other return or error code.
   `one_zero` contributes one `main_zero`/`racer_zero` to the successful side and
   one `main_loss`/`racer_loss` to the invalid-ID side.
7. If `both_zero` exists, stop the run immediately and hand the frozen round to
   P2. Do not close outstanding fds, do not clean up, do not reuse the IDs.
8. Otherwise, perform cleanup with a separate `errs_cleanup[]` array, close the
   transports, and continue.

The reference's `errs_main` is passed to both the race delete and the mop-up
delete (gist lines 542/545), which destroys the measurement. This design forbids
that: `errs_main`, `errs_racer`, and `errs_cleanup` are distinct allocations.

### 3.2 Three scheduling strategies

| Name | CPUs | Release | Purpose |
|---|---|---|---|
| `split_barrier` | main on cpu A, racer on cpu B | both released by one atomic gate | True parallel entry; primary candidate |
| `same_barrier` | both on cpu A | both released by one atomic gate | Scheduler-preemption control; no true parallel execution |
| `split_stagger` | main on cpu A, racer on cpu B | main leads by `race.stagger_us`, racer follows | Lands the second delete inside the linger-parked lock-drop window |

`race.stagger_us` is swept only in `split_stagger`; default 0. The sweep values
are fixed before the run and recorded. Strategies are selected with
`--set race.strategy=...`; `all` runs them in the order above and stops at the
first candidate win.

A strategy that completes `race.rounds` with zero `both_zero` results is a loser
and is appended to `docs/DEAD_ENDS.md` with the run ID, rounds, terminal
timeouts, one-zero count, both-error count, unexpected count, and the window
statistics. A clean run is not a refutation; it is a bounded negative result for
that schedule.

### 3.3 Timing and the 624 counter

Each delete records:

- `sys624_in` and `sys624_out`, read immediately around 662;
- `t_in_ns` and `t_out_ns` from `clock_gettime(232, CLOCK_MONOTONIC)`;
- `delete_us = (t_out_ns - t_in_ns) / 1000`;
- `overlap_us = max(0, min(t_out_main, t_out_racer) - max(t_in_main, t_in_racer))`;
- `park_us` for the linger close path, where measurable;
- `delta624 = sys624_out - sys624_in`;
- `window_us_624 = delta624 * ns_per_624_tick / 1000` when a calibration is
  valid, otherwise `unset`.

The repo measures 624 as a VM-operation counter, not a clock
(`docs/THREADING_ALIVE_14_10.md` lines 117-119). The design therefore reports
the raw `delta624`, the optional `window_us_624`, and a calibration-valid flag.
The calibration times a repeatable operation that advances 624 (for example the
documented `mmap` `+5`) with sys 232; if the factor is not stable, the flag is
false. The primary µs columns come from sys 232. If `delta624` is zero, the row
says `624_flat=1`; it does not discard the timing sample.

### 3.4 P1 outputs

Human-readable summary plus JSONL per round. Summary fields:

`run_id`, `profile`, `strategy`, `rounds_requested`, `rounds_completed`,
`terminal_ok`, `terminal_timeout`, `candidate_wins`, `id_wins`, `main_zero`,
`racer_zero`, `main_loss`, `racer_loss`, `one_zero`, `both_error`,
`unexpected`, `parking_not_observed`, `overlap_us_min`,
`overlap_us_median`, `overlap_us_p95`, `overlap_us_max`, `delta624_min`,
`delta624_median`, `delta624_max`, `window_us_624_valid`.

P1 acceptance: either a frozen candidate round handed to P2, or a bounded
negative with counts for every strategy. No P3 spray is performed in a negative
run.

### 3.5 Optional 0xFF probe mode (`--probe-ff`)

The reference snapshots' 0xFF spray thread is a binary fault detector: it
reclaims freed 64-zone chunks with `0xFFFF...` so a re-lookup of a freed state
faults at a controlled address. It produces no per-round record and its only
signal is "fault or no fault". P1's per-attempt JSONL line and `outcome` enum
supersede it as the success signal. Keep the probe as an optional
`--probe-ff` mode that can still produce the reachability fault, but a
`--probe-ff` fault is evidence, not a win: a P1 run never reports success off
it, and it never replaces the `both_zero` frozen-candidate path.

## 4. P2 — exact winner detection and per-ID evidence

P2 does not run a new race. It consumes the frozen P1 candidate.

### 4.1 Candidate record

For each ID, P2 records:

- `id`, `main_ret`, `racer_ret`;
- `main_err`, `racer_err`;
- `main_state_pre`, `racer_state_pre`;
- `main_seq`, `racer_seq` from a single atomic return sequence;
- `first_zero_tid`, `second_zero_tid`;
- `classification` (`both_zero`, `one_zero`, `both_error`, `unexpected`);
- `array_sentinel_ok` for both result arrays.

`both_zero` is true only when both `err` entries are exactly `0`. The lower
sequence number is the first caller; the higher sequence number is the second
caller and the duplicate-success winner. That is an ordering claim, not a claim
about which caller freed first; P3 decides whether the slot was actually
double-freed.

### 4.2 Per-ID logging

P2 prints one line per ID in the candidate round, not only the winner, for
example:

`p2 id=0x... main_ret=0 main_err=0x0 racer_ret=0 racer_err=0x0 seq=17/18 class=both_zero`

For non-candidate rounds, verbosity is controlled by `--verbose`; any
`unexpected` or `both_error` row is always printed. This makes the difference
between `ESRCH`/`0x80020003` and `0` visible without inference.

### 4.3 Integrity checks

- Sentinel words before and after each result array are checked after the join.
- The arrays must not overlap; a shared page is not used for the two result
  arrays.
- If a result array was clobbered, the round is `array_invalid`, not a win.
- P2 re-runs the duplicate-control on the same ID batch only if it can do so
  without touching the freed slot; otherwise it reports the control from the
  pre-race run. It does not perform a post-win poll of the dangling ID.

P2 acceptance: exactly one of `p2_two_zero=1` with a complete per-ID log, or
`p2_no_candidate` with the P1 negative record. A candidate is handed to P3 with
all fds held.

## 5. P3 — tag/readback oracle and held overlap pair

P3 runs only after P2 reports `p2_two_zero=1`. If P2 is negative, P3 prints
`p3_skipped=no_candidate` and returns.

### 5.1 Two-pass tag allocation

For each zone in `oracle.zones`:

1. Resolve `oracle.zone_optlen` for the zone and verify the IPv6 option is
   accepted.
2. Allocate `oracle.fanout` sockets in two passes. Each pass tags every chunk
   with a per-run cookie and an index: `tag = (spray.tag_base | index)` plus a
   64-bit nonce at a second offset.
3. Read every chunk back with `getsockopt` and classify:
   - `own`: the tag and nonce match the index that owns that fd.
   - `overlap`: fd `i` reads a tag belonging to a different fd `j`; record the
     ordered pair `(i, j)`, zone, and both tags.
   - `garbage`: short read, failed read, or wrong cookie/nonce.
4. Print per-zone counts:
   `zone=<n> optlen=<n> sprayed=<n> own=<n> garbage=<n> overlap=<n>`.

A cross-pass overlap is the only accepted positive. Two allocations that
merely reuse one normally freed chunk cannot produce a cross-pass tag swap; a
double-freed chunk can.

### 5.2 Holding the pair

`reclaim_zone()` returns an `oracle_result`:

`found`, `zone`, `optlen`, `fd_a`, `fd_b`, `tag_a`, `tag_b`, `own`, `garbage`,
`overlap`.

On a positive:

- close every non-overlapping fd in that zone;
- keep `fd_a` and `fd_b` open and do not issue another allocation in that zone;
- return the pair to `reclaim_and_confirm()`;
- print the pair and stop. No P4/P5 work is attempted.

On a negative: close all fds in the zone and continue to the next zone. The
reference's `reclaim_zone()` closes all oracle fds at lines 463-464, including
the pair it just found; this design closes only the non-overlap set.

`reclaim_and_confirm()` is called from the P3 entry path after a P2 candidate
(gist lines 473-483 define it but the reference never calls it). It iterates the
zones, returns the first positive, and prints a final `p3_overlap_pair` line. It
is also callable by `--phase p3-control` in a fresh process with no race; the
control must report zero overlaps on a healthy allocator.

### 5.3 Why RTHDR is not selectable

`IPV6_RTHDR` is present in the reference candidates (gist line 408) but is
documented as `EINVAL` on PS5 (gist line 100). The ledger permits only
`spray.options=dst,hop`; RTHDR is a preflight negative control, never a spray
primitive. If a future target accepts RTHDR, it must be re-enabled by an
explicit ledger change and a measured result, not by falling through a candidate
list.

### 5.4 P3 acceptance

`p3_overlap_pair` with two live fds and per-zone own/garbage/overlap counts, or
`p3_no_overlap` with the counts. The run ends there.

## 6. Reference defects and their fixes

| Reference defect | Gist lines | Design fix |
|---|---|---|
| `detect_double_free()` defined but unused | 322-328 | P1/P2 use a single `classify_two_zero()` path; P2 calls it on the frozen candidate and logs every ID |
| `errs_main` overwritten by the mop-up delete | 542, 545 | `errs_main`, `errs_racer`, `errs_cleanup` are separate; copy before any cleanup; no mop-up in the measurement path |
| `reclaim_and_confirm()` never called | 473-483 | Called by the P3 entry path only after `p2_two_zero=1`; also used by the P3 control |
| `reclaim_zone()` closes the pair it should hold | 463-464 | Close only non-overlap fds; return `oracle_result` with both pair fds open |
| `IPV6_RTHDR` still selectable though EINVAL | 100, 408 | Ledger allows `dst,hop` only; RTHDR is a preflight negative control |
| `[TUNE]` values scattered in source | 143, 148, 153, 185, 231-232, 297, 351 | All resolve through `docs/PROLAPSE_OFFSETS.md`; `--set` override; P4-only key rejected by P1-P3 |

### 6.1 Shared reference defects (both snapshots)

`probe_reqs2_zone()` does not exist in either snapshot. The only zone-probing
mechanism in either file is the five hardcoded optlens (56/120/248/264/520)
inside `reclaim_and_confirm()`, and that function is uncalled in both. The A1
sweep is kept — it replaces an uncalled hardcoded list, not a probing routine.

The two files are functionally identical apart from comments (comment-stripped
diff is empty); the kuzibelit repo adds the README and nothing functional. The
gist is canonical. Corroboration only — see `docs/EVIDENCE_REVIEW.md` §6.
Line numbers: gist (555 lines, vendored byte-identical) and kuzibelit at commit
`3624ff2` (vendored copy offset +9 by its provenance header).

| Reference defect (shared) | Lines (gist / kuzibelit) | Design fix |
|---|---|---|
| `spray_0x80`, `build_fake_entry`, `spray_free_all`, `arb_dec_and_maybe_free`, `FAKE_NOTIFY_TARGET` are present in both snapshots and unreachable from `main()`; only the 0xFF probe loop runs | 160-188, 351-378 / 173-213, 373-377 | P1-P3 dispatch keeps every stage reachable from `main()`; unreachable code is rejected at review. The 0xFF probe is retained only as optional `--probe-ff` (see §3.5) |
| `build_fake_entry` writes an RTHDR-shaped header while `select_opt()` may pick DSTOPTS; valid only because zero bytes are legal Pad1 and `+0x40` survives by accident | 179-188, 405-420 / 203-213, 427-438 | The fake-entry template must be shaped for the option `select_opt()` actually chose (DSTOPTS/HOPOPTS), not a hardcoded RTHDR layout; `+0x40` is written as an explicit field, never as a by-product of zero-fill |

## 7. File layout and build

```
exploit/prolapse/
  Makefile
  README.md
  include/
    prolapse_config.h      # generated from docs/PROLAPSE_OFFSETS.md
    ps5_abi.h
    aio.h
  src/
    main.c                 # phase dispatch, --set, --list-config
    config.c               # ledger/profile/override resolution
    syscalls.c             # 669/662/664/666/232/624 + socket/close/affinity
    transport.c            # TCP/socketpair, terminal state, linger parking
    race.c                 # P1 threads, gate, round record, strategy
    p2.c                   # exact winner and per-ID log
    p3.c                   # tag/readback oracle, held pair
    report.c               # human summary + JSONL
  tests/
    test_config.py         # host-side parser/range tests
    test_classify.py       # host-side both-zero/one-zero classifier tests
```

- Build with the PS5 payload SDK; `make` produces `prolapse_p1p3.elf` and a
  SHA-256 receipt. `make check` runs host tests and a generator-drift check; it
  does not require a console.
- `tools/gen_prolapse_config.py` generates the header from the ledger. A
  checked-in header that differs from the generator output fails `make check`.
- Deploy with the existing ELF loader on the 14.00 test console:
  `nc <console> 9021 < prolapse_p1p3.elf` (or the available elfldr sender).
- Capture stdout verbatim. The report is also emitted as JSONL for the result
  archive.

## 8. Safety and stop conditions

- No P4/P5, no arbitrary free, no KRW, no credential patch, no WebKit, no
  `lane_VA`.
- P1 stops on the first `both_zero`; P2/P3 then run on the frozen state. No
  cleanup delete touches a candidate slot.
- P3 holds the pair and exits; it does not allocate again in the winning zone.
- Every round is bounded by `race.terminal_timeout_us` and
  `linger.park_timeout_us`; the run has a hard `race.rounds` cap.
- Progress is printed before each risky syscall and flushed, so a console death
  leaves the last completed phase and round in the capture.
- A `parking_not_observed`, `terminal_timeout`, `control_failed`,
  `array_invalid`, or `unexpected` result is reported as such, not converted
  into a win.

## 9. Approval gate

After approval, implementation order is:

1. Config generator + ledger + host tests.
2. Syscall/transport layer with terminal-state and parking preflight.
3. P1 strategies and telemetry.
4. P2 exact winner logging.
5. P3 oracle with held pair.

No code will be written before this design is approved. The first run will be
P1 only on 14.00 V59. P2/P3 run only from a P1 candidate in the same process.
P4/P5/KRW remain out of scope for this round.

## 10. Binding amendments (approved 2026-11-03)

### A1 — the size class is unproven

`0x58` is 88 bytes. With 16-byte class steps that is at least 96; the reference
`SPRAY_OPTLEN 120` lands in 128; the session arithmetic
(`0x58 + 0x10` = 104) lands in 112. The three numbers disagree, and none is a
confirmed 0x58 request. None may be hardcoded in P1/P2. No function named
`probe_reqs2_zone` exists in either reference snapshot — the only zone-probing
mechanism there is the five hardcoded optlens in uncalled
`reclaim_and_confirm()` (see §6.1).

The ledger records all three with provenance `ASSUMED` and the arithmetic.
The P3 optlen sweep is `88, 96, 104, 112, 120, 128, 136, 152, 168, 184`.
P1/P2 read `aio.req2_zone`/`oracle.zone_optlen` if they ever need them; they do
not need a zone this round. A flat sweep is evidence that `0x58` is the wrong
size, not merely that the bucket is wrong.

### A2 — P3 remains blocked

P3 needs the req2 zone from IDA on `kernel_1400.elf` or from the A1 sweep. It is
not implemented this round. The README states this explicitly. P1 and P2 do not
need a zone.

### A3 — parked threads do not leak

- Bounded parked-thread pool.
- `SO_LINGER {0,0}` releases a parked thread on demand.
- `--max-threads` default 64; exceeding the cap fails the run loudly.
- The cap trip is validated in-build by the cap control (`--control cap`):
  `max_threads + 2` no-op assignments, the last two must trip; no AIO syscall
  is issued by the no-op jobs; host-verifiable (cap-only mode skips preflight
  4).
- Drain to zero at teardown, asserted before P2's frozen pass and before exit.
- Every round line prints `parked_live`.

### A4 — preflights assert, do not assume

1. `clock_gettime(232)` returns 0 and writes a nonzero `tv_nsec` progression
   across two calls. Failure aborts. 624 is never used as a clock fallback.
2. `cpuset_getaffinity(3,1,-1,...)` returns at least two distinct CPUs. Failure
   at one CPU aborts; main and racer are never silently co-scheduled.
3. `rlimit` is raised to cover every swept optlen at `SPRAY_FANOUT` plus
   headroom. A refused raise aborts.
4. `SO_LINGER {1,1}` preflight parks one thread, confirms it is still parked
   after 5 ms, releases it with `{0,0}`, and confirms it returns. Failure aborts
   before the race.

### A5 — stop on first win

`--stop-on-first-win` defaults on: one sample per boot. Reliability runs use
`--no-stop-on-first-win` to produce a denominator.

### A6 — dead ends are written on loss in the same run

When a strategy crosses its losing threshold, the run appends the entry to
`docs/DEAD_ENDS.md` as a side effect: strategy, attempts, wins, p99 window, cap
hits. It is not reconstructed from logs later.

### A7 — exit-code mapping (approved 2026-11-03)

| rc | meaning |
|---:|---|
| 0 | success |
| 1 | synthetic abort: `both_zero` row in `--synthetic` (host only) |
| 2 | `preflight_failed` (real console preflight failure), `serialized_control_failed`, `cap_control_failed`, or config/drain failure |
| 3 | `preflight_unavailable_in_build` (host build: preflight 4 cannot run; AIO is ENOSYS by design) |
| 4 | `generation_control_failed`: the bumped-generation delete did not reproduce `0x80020003` |

Every failing path prints a one-line `verdict=... rc=N` on stderr. Codes 0-3
are frozen as of this amendment; rc=4 is reserved for the generation control
only.

## 11. P1/P2 implementation scope (this round)

P1 and P2 only. P3 is not implemented. The P1 handoff is JSONL; P2 parses frozen
candidates only and never races. The 14.00 V59 profile is the only active
profile; 14.10 offsets are not ported this round. The implementation path is
`payloads/prolapse/`, superseding the `exploit/prolapse/` layout in §7.

