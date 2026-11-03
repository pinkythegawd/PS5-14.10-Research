# Prolapse P1-P3 configuration and offset ledger

Status: design-time ledger. Every `[TUNE]` value in the reference
(`aio_lapse_exploit.c`, gist `80dff057`) must resolve through this table. No
value is edited in a source file to retune a run; use `--set key=value`.

The 14.00 V59 column is deliberately not filled with guessed 14.10 values.
Entries marked `port pending` must be measured on the target and supplied with
`--set` (or a checked-in profile) before the dependent phase is allowed to run.
The one exception is the `sys.*` ABI numbers: the 14.00 column says
`ASSUMED (14.10 ABI; verify on target)`, and P1 prints the fallback it used.
No P1/P2 value is `CONFIRMED_ON_HW` yet.

Provenance vocabulary used below: `CONFIRMED_IN_IDA`, `CONFIRMED_ON_HW`, and
`ASSUMED`. A value is never promoted without a matching artifact.

## Toolchain pin (2026-11-03)

Cross-run comparison (windows, park times, codegen-sensitive race behavior) is
only valid within one toolchain. Any change to any row below invalidates every
P1/P2/P3 timing comparison made before it; update this block and re-baseline
before the next run. A toolchain change is a ledger change, not a silent
rebuild. **An SDK version change invalidates cross-run comparison exactly like
a compiler change** — the SDK supplies the compiler wrapper, crt objects, libc
and stubs that every console ELF links against.

| Component | Pinned value |
|---|---|
| WSL distro | Kali GNU/Linux Rolling (Debian base) |
| WSL host kernel | 6.18.33.2-microsoft-standard-WSL2 |
| `clang --version` (console ELF) | clang 18.1.8, prebuilt `clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04` release tarball (llvm-project `llvmorg-18.1.8`), user-local at `~/ps5-payload-dev/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04`; `ld.lld` 18.1.8 |
| PS5_PAYLOAD_SDK | v0.45 (tag date 2026-10-20), source https://github.com/ps5-payload-dev/sdk, installed 2026-11-03 to `~/ps5-payload-dev/prefix` (`make install`, user-local, no sudo); libtinfo5 (Debian bullseye deb) extracted user-locally for the wrapper |
| `gcc --version` (make check) | gcc (Debian 16.2.0-1) 16.2.0 |
| `python3 --version` (generator/checks) | Python 3.14.7 |

## Runtime keys

| Key | Default | Range | Phase | 14.10 status | Source |
|---|---:|---|---|---|---|
| `target.profile` | `14.00_v59` | `14.00_v59`, `14.10` | all | selectable | task scope |
| `race.batch` | 4 | 1-32 | P1 | measured shape | gist 231-232 |
| `race.rounds` | 50000 | 1-5000000 | P1 | budget | gist 524; design budget |
| `race.strategy` | `all` | `all`, `split_barrier`, `same_barrier`, `split_stagger` | P1 | design | task default; cheapest-first when `all` |
| `race.stagger_us` | 0 | 0-500 | P1 | design | this document |
| `race.seed` | 0 | 0-18446744073709551615 | P1 | design | 0 = fixed schedule |
| `race.max_threads` | 64 | 2-256 | P1 | design | hard parked/active cap |
| `race.control_rounds` | 500 | 8-100000 | P1 | design | serialized and generation controls; slice requires >=500 attempts |
| `race.loss_threshold` | 1000 | 1-5000000 | P1 | design | A6 dead-end append threshold |
| `race.stop_on_first_win` | 1 | 0-1 | P1 | design | A5 default on |
| `race.spin_limit` | 800000000 | 1-4000000000 | P1 | gist | gist 543 |
| `race.transport` | `tcp_loopback` | `tcp_loopback`, `unix_socketpair` | P1 | design | gist 290-298 uses socketpair; TCP is the linger-capable default |
| `race.terminal_timeout_us` | 1000000 | 1000-10000000 | P1 | design | gist 527-528 |
| `linger.onoff` | 1 | 0-1 | P1 | gist | gist 297 |
| `linger.seconds` | 1 | 0-60 | P1 | gist | gist 297 |
| `linger.park_timeout_us` | 2000000 | 1000-60000000 | P1 | design | this document |
| `spray.optlen` | 120 | 8-1024, multiple of 8 | P3 | reference primary value; `oracle.zone_optlen` overrides per zone | gist 148-152 |
| `spray.fanout` | 1024 | 1-4096 | P3 | gist | gist 153 |
| `spray.options` | `dst,hop` | subset of `dst,hop` | P3 | `rthdr` disabled | gist 100, 408 |
| `spray.tag_base` | `0xAABB0000` | 32-bit | P3 | gist | gist 400, 453 |
| `spray.alternative` | `none` | `none`, `evf`, `pktopts` | P3 | untested | gist 143 |
| `spray.rthdr_type` | 0 | 0 | P3 | **disabled** | gist 185; RTHDR is EINVAL on PS5 (gist 100) |
| `oracle.fanout` | 256 | 2-4096 | P3 | gist | gist 430 |
| `oracle.passes` | 2 | 1-4 | P3 | gist | gist 432 |
| `oracle.zones` | `64,128,256,512,1024` | comma list | P3 | gist | gist 477-481 |
| `oracle.zone_optlen` | `64:56,128:120,256:248,512:264,1024:520` | zone:optlen map | P3 | gist | gist 477-481 |
| `oracle.keep_pair` | 1 | 0-1 | P3 | design | this document; 0 is control only |
| `p4.fake_notify_target` | 0 | 64-bit | **P4 only** | reserved | gist 351; rejected by P1-P3 CLI |

## Kernel/object values

| Key | 14.00 V59 | 14.10 | Phase | Source |
|---|---:|---:|---|---|
| `aio.req2_size` | port pending | `0x58` | P3 | gist 8-9; 14.10 IDA claim; **ASSUMED** (A1) |
| `aio.req2_zone` | derived, unset | `128` | P3 | gist 138-152; **ASSUMED** (A1) |
| `aio.entry_notify_off` | port pending | `0x40` | P4 only | gist 10-12 |
| `aio.state_size` | port pending | `0x160` | P4 only | gist 480 |
| `sys.aio_submit_cmd` | **ASSUMED** (14.10 ABI; verify on target) | `669` | P1 | gist 59 |
| `sys.aio_multi_delete` | **ASSUMED** (14.10 ABI; verify on target) | `662` | P1 | gist 60 |
| `sys.aio_multi_poll` | **ASSUMED** (14.10 ABI; verify on target) | `664` | P1 | gist 61 |
| `sys.aio_multi_wait` | **ASSUMED** (14.10 ABI; verify on target) | `663` | P1 | gist 62 |
| `sys.aio_multi_cancel` | **ASSUMED** (14.10 ABI; verify on target) | `666` | P1 | gist 63 |
| `sys.clock_gettime` | **ASSUMED** (14.10 ABI; verify on target) | `232` | P1/P2/P3 | `docs/SYSENT_PRESENCE_14_10.md` line 251 |
| `sys.vm_counter` | **ASSUMED** (14.10 ABI; verify on target) | `624` | P1/P2/P3 | `docs/THREADING_ALIVE_14_10.md` lines 117-119 |

`sys.vm_counter` is recorded as a raw counter only. The repo measures it as a
VM-operation counter: `gettimeofday` leaves it flat, and the `mmap` sanity check
moves it by `+5` (`docs/THREADING_ALIVE_14_10.md` lines 117-119). A zero delta is
therefore a valid observation, not a timer failure. Microsecond figures come
from `sys.clock_gettime`; the 624 delta and its calibration factor are reported
beside them.

## `--set` contract

- Syntax: `--set key=value`, repeatable. Example:
  `--set race.batch=8 --set race.strategy=split_stagger --set race.stagger_us=25`.
- `--list-config` prints every effective key, its value, its source
  (`ledger`, `profile`, `override`), and whether the phase is allowed.
- Unknown keys, out-of-range values, and phase-forbidden keys are hard errors
  before any syscall.
- `p4.fake_notify_target` is listed only so the reference `[TUNE]` is accounted
  for. Passing it to P1-P3 is an error, not a silently ignored option.
- `aio.req2_size=port pending` means P3 refuses to start. P1/P2 may run without
  it. This is the "ported, not assumed" rule.

## A1 size-class discrepancy (2026-11-03)

The request size is **not** a confirmed class. Provenance: **ASSUMED** for every
number below.

| Arithmetic | Result | Class if 16-byte steps | What it means |
|---|---:|---:|---|
| `0x58` | 88 | 96 | raw request size; not a class boundary |
| `0x58 + 0x10` (`probe_reqs2_zone`) | 104 | 112 | reference probe's zone estimate |
| `SPRAY_OPTLEN 120` | 120 | 128 | reference spray's zone estimate |

The three disagree and none is a confirmed 0x58 allocation. Do not hardcode
`0x58`, 96, 112, 120, or 128 in P1/P2. P3 must sweep:

`88, 96, 104, 112, 120, 128, 136, 152, 168, 184`

and treat a flat sweep as evidence that the `0x58` size assumption is wrong, not
just that the bucket choice is wrong.

P3 is blocked this round. P1/P2 read any zone/optlen from this ledger if they
need it and do not inline a class.

## Build-time generation

`tools/gen_prolapse_config.py` reads this file and emits
`payloads/prolapse/include/prolapse_config.h`; `make` runs the generator and
fails if the checked-in header differs. This keeps the ledger as the only place
where a default is written. The generated header is not hand-edited.

