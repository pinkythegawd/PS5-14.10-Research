# Evidence review — AIO lapse, duplicate delete, and Prolapse scope

Date: 2026-11-03
Branch: `feat/prolapse`
Status: **resolved, re-scoped**. The three apparent conflicts are compatible when
the generation check, the post-lock revalidation, the control condition, and the
missing race preconditions are kept separate. This document does not rewrite
any measured result in the cited records.

Reference source: zecoxao gist `80dff057233c1f203352ff8bf317294d`,
`aio_lapse_exploit.c`. Line numbers below are from the raw file downloaded on
2026-11-03, 555 lines, SHA-256
`087D354375D53FBC63C77DEBC58AC3C602B4A9BC32A222129401807ED0600ABB`.
Vendored at `exploit/prolapse/reference/zecoxao_aio_lapse_exploit.c`.

Second reference: kuzibelit `PS5-14.00-14.10-Jailbreak-PoC`, commit `3624ff2`
(2026-10-27), a reworked variant of the same file ("Publapse", 577 lines).
Vendored at `exploit/prolapse/reference/kuzibelit_aio_lapse_exploit.c` with a
provenance header; body is verbatim. Used here for corroboration only — none of
its code is copied into tracked sources.

## 1. The 14.10 generation check is real; the 14.20 fix is the revalidation after the lock drop

**Repo evidence.** `docs/AIO_BATCH_14_10.md` lines 109-130 measures a real
identifier generation field on 14.10:

- bits 13-15 cycle through eight values on one slot (lines 111-119);
- bumping the generation by one produces `0x80020003` for delete and poll, while
  the live request remains (lines 120-128);
- the doc calls the wrap "an aliasing window, not an unchecked index" (lines
  130-132).

That is a **submit-time / lookup-time check**. It rejects an identifier whose
generation does not match the slot at the point the kernel looks the request up.

**Gist evidence.** The gist's header says the 14.20 patch is the
`*state != saved_generation_id` **recheck** (lines 17-18), and that the
recycled-slot step is what dies on 14.20. The session status outside this
repository records the same 14.20 diff more explicitly
(`ps5-1410/STATUS.md` lines 349-368): the first lookup and the post-lock
re-lookup can see different slots, and the check rejects a reused slot before
freeing.

**Reconciliation.** These are two mechanisms, not two claims about the same
check:

1. 14.10 already validates the generation when the ID is resolved. That is why
   `AIO_BATCH_14_10.md` sees `0x80020003` on a bumped ID.
2. The 14.10 delete path can drop the AIO lock to re-acquire the object. A
   pre-drop check can pass, a racing thread can free and reallocate the slot,
   and the resumed path can free sub-objects of the **swapped** object.
3. 14.20 adds the revalidation after the lock drop. The field is not new; the
   second check is.

The `0x80020003` measurement and the race claim therefore coexist. The first
describes the ID check at lookup; the second describes what happens after the
lookup result is saved and the lock is released.

## 2. Duplicate delete is the control condition, not a refutation

**Repo evidence.** `docs/PRIVATE_AIO_DUPLICATE_DELETE_2026-10-21.md` lines 7 and
19 records `[id, id]` in one batch: per-entry results `0x00000000,
0x80020003`, a later poll returning `0x80020003`, and a surviving loader. The
doc interprets this as "ordinary sequential removal" and says not to repeat the
input without a new lifecycle hypothesis. That is exactly what it is: a
single-call, single-thread control.

**Why it cannot refute the race.** A duplicate ID in one call is processed by
one thread, one entry at a time. The second iteration either sees the first
entry already removed or sees its own saved state; it never swaps a slot with a
second thread. It is the baseline that shows the normal second-lookup rejection.
It does not exercise the lock-drop window.

**Near-miss control.** `ps5-1410/STATUS.md` lines 356-362 records the two-child
`556a` result: both children deleting the same 40 IDs report `0` on 25, but the
free is guarded by test-and-clear of `[W+0x18]`/`[W+0x20]`, and the losing
caller finds both sub-object pointers zeroed. 42 double-status results left the
allocator untouched, and 200 new requests during the race changed nothing. That
is a control showing the guard working; it is not evidence that the
pre-lock-drop window cannot be hit by a different schedule.

## 3. 416a/420a lacked the two race preconditions

**Repo evidence.** `docs/CHAIN_14_10.md` lines 53-74 records the native AIO
races:

- `416a`: 1200 submits vs 1200 deletes, clean;
- `420a`: 1200 submits vs 1200 polls vs 1200 deletes, clean;
- both sides native, with the object provably freed and reused elsewhere.

Those results stand. They are negative results for the schedules they ran, not
for the lock-drop window.

**Gist preconditions.** The reference uses two mechanisms that the 416a/420a
schedules did not combine:

1. `aio_cancel` followed by `aio_poll` forces the request to terminal/aborted
   state so the delete path takes the free branch (gist lines 527-528).
2. `SO_LINGER {1,1}` on the last file reference parks the thread inside
   `_fdrop` while the AIO lock is dropped (gist lines 226-229 and 297-298).

Without both, delete/delete serializes by construction: the first call removes
the entry and the second sees an invalid ID. `416a`/`420a` did not force the
terminal state and did not park a thread in `_fdrop`.

**Reconciliation.** The correct status of the two-thread delete race is
**untested rather than refuted**. The untested window is the interval between
the re-lookup and the slot guard. The status record puts it between `0x8ad6fe7c`
and `0x8ad6fec3` and notes the loader limits (`ps5-1410/STATUS.md` lines
363-368). P1-P3 is designed to test that window with both preconditions and to
record the losing schedules with numbers.

## 4. Deferred, not blocking

- FontFace: 4/25 boots reached stage 2 with stage 2 never hitting — leak real, free trigger missing; deferred, not blocking.

## 5. Scope consequences

- Prolapse is a native payload: RIP control -> userland ROP -> `mmap`/read ELF
  -> native kernel work. The FontFace -> `lane_VA` -> V88 forge is not on its
  critical path. Stages 0-3 leave the Prolapse milestone. Finding 10 remains
  closed and is not modified.
- The dev/validation target is the 14.00 V59 path where native code execution
  already exists; 14.10 is the delivery target. Payload offsets are ported, not
  assumed. No P1-P3 result is gated on 14.10 WebKit state.
- This round is P1-P3 only. P4/P5 and kernel R/W remain out of scope.

## 6. Kuzibelit Publapse reference — corroboration only

Not technical evidence; it is a second public snapshot of the same lineage
(gist "Prolapse", repo "Publapse"). Naming drift is noted once here; the ledger
and payload code keep `PROLAPSE`.

The two files are functionally identical apart from comments (the
comment-stripped diff is empty); the kuzibelit repo adds the README and nothing
functional. The gist is canonical; the repo is README provenance around the
same code.

- **Defined-but-uncalled corroboration.** The kuzibelit README (lines 107-108,
  150) independently lists `reclaim_and_confirm()` and `detect_double_free()`
  as "Defined, but not called by `main()`". This corroborates the defect list in
  `docs/PROLAPSE_P1_P3_DESIGN.md` §6. It is a second author's reading of the
  same code, not a measurement.
- **Zone probing.** `probe_reqs2_zone()` does not exist in either snapshot.
  The only zone-probing mechanism in either file is the five hardcoded optlens
  (56/120/248/264/520) inside `reclaim_and_confirm()`, and that function is
  uncalled in both. The A1 sweep is kept — it replaces an uncalled hardcoded
  list, not a probing routine.
- **Unreachable code — shared defect.** In both snapshots, `spray_0x80`,
  `build_fake_entry`, `spray_free_all`, `arb_dec_and_maybe_free`, and
  `FAKE_NOTIFY_TARGET` have no call site reachable from `main()`; only the 0xFF
  probe loop runs. Recorded in the fix list (§6.1), not evidence of exploit
  behavior.

## Evidence anchors

| Topic | Anchor |
|---|---|
| 14.10 generation field and `0x80020003` | `docs/AIO_BATCH_14_10.md` lines 109-132 |
| No boundary break through the AIO surface | `docs/AIO_BATCH_14_10.md` lines 149-153 |
| Duplicate-delete control | `docs/PRIVATE_AIO_DUPLICATE_DELETE_2026-10-21.md` lines 7, 19 |
| 416a/420a clean schedules | `docs/CHAIN_14_10.md` lines 53-74 |
| 14.20 diff, guard, and slot window | `ps5-1410/STATUS.md` lines 349-368 |
| 624 is a VM-operation counter, not a clock | `docs/THREADING_ALIVE_14_10.md` lines 117-119 |
| 14.20 recheck in the reference | gist lines 17-18 |
| Cancel + poll terminal state | gist lines 527-528 |
| Linger parking | gist lines 226-229, 297-298 |
| Reference P1-P3 defects | gist lines 100, 322-328, 408, 463-464, 473-483, 542/545 |
| Defined-but-uncalled corroboration | kuzibelit README lines 107-108, 150 |
| Shared unreachable spray/fake-entry code | gist lines 160-188, 351-378; kuzibelit lines 173-213, 373-377 |
| Shared RTHDR template vs DSTOPTS `select_opt` | gist lines 179-188, 405-420; kuzibelit lines 203-213, 427-438 |
| Five hardcoded optlens in uncalled `reclaim_and_confirm()` | gist lines 473-483; kuzibelit lines 495-503 |

Kuzibelit line numbers refer to the original repo file at commit `3624ff2`;
the vendored copy is offset +9 by its provenance header. Gist line numbers
refer to the 555-line raw gist (vendored byte-identical).
