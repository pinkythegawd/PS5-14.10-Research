# Prompt vs docs — reconciled and re-scoped

Date: 2026-11-03
Task: PROLAPSE native P1-P3 AIO delete-path instrumentation
Branch: `feat/prolapse`, created from `master`
Status: **resolved, re-scoped** — the docs stand; the prompt's native scope is
narrowed to P1-P3 and the earlier "blocked" conclusion is withdrawn.

## Rule applied

The task says docs override the prompt on conflict. The earlier record treated
the AIO generation check, the duplicate-delete control, and the clean 416a/420a
races as contradictions. They are not contradictions once the mechanisms and
preconditions are separated. The reconciliation is in
[`EVIDENCE_REVIEW.md`](EVIDENCE_REVIEW.md); no measured result is rewritten.

No `exploit/` code, no build output, and no offsets header have been created.
The P1-P3 design is in [`PROLAPSE_P1_P3_DESIGN.md`](PROLAPSE_P1_P3_DESIGN.md)
and is waiting for approval before implementation.

## 1. FontFace — deferred, not blocking

The measured result is 4/25 boots with a real leak; stage 2 never hit, so the
free trigger is missing. The earlier claim of a "100% reclaim recipe" remains
unsupported. Finding 10 stays closed and is not modified.

Prolapse is now a native payload. The FontFace -> `lane_VA` -> V88 forge chain is
not on its critical path, so the missing browser free trigger does not block
P1-P3. Stages 0-3 leave the Prolapse milestone.

## 2. AIO generation check — real on 14.10, revalidation added on 14.20

The two records describe different checks:

- `docs/AIO_BATCH_14_10.md` lines 109-132 measures the 14.10 generation field and
  the `0x80020003` rejection at lookup time.
- The gist (`aio_lapse_exploit.c` lines 17-18) says 14.20 adds the
  `*state != saved_generation_id` recheck. `ps5-1410/STATUS.md` lines 349-368
  records the same diff and the first-lookup/post-lock-re-lookup window.

The 14.10 pre-drop check can pass, a racer can swap the slot, and the resumed
delete can free sub-objects of the wrong object. The 14.20 fix is the
revalidation, not the field. Both docs stand.

## 3. Duplicate delete — control condition, not refutation

`docs/PRIVATE_AIO_DUPLICATE_DELETE_2026-10-21.md` lines 7 and 19 records
`[id, id]` in one call: one success, one `0x80020003`, loader alive. That is a
single-thread control. A plain duplicate delete never swaps a slot, so it cannot
exercise the lock-drop window. The `556a` two-child near-miss
(`ps5-1410/STATUS.md` lines 356-362) shows the slot guard working; it is the
baseline, not a refutation.

## 4. 416a/420a — clean, but missing preconditions

`docs/CHAIN_14_10.md` lines 53-74 records the native AIO races as clean. Those
results stand. They did not combine the two conditions the reference needs:

1. `aio_cancel` + `aio_poll` to force terminal state so delete takes the free
   branch (gist lines 527-528).
2. `SO_LINGER {1,1}` parking to hold the thread in `_fdrop` with the AIO lock
   dropped (gist lines 226-229, 297-298).

Without both, delete/delete serializes. The two-thread race is **untested
rather than refuted**; the window is between the re-lookup and the
`[W+0x18]`/`[W+0x20]` guard.

## Decision

The docs are not overridden. The reconciled position is:

1. AIO delete/delete is a narrow, preconditioned native test, not a claim that
   the docs missed a trivial bug.
2. P1-P3 are authorized in principle: P1 instrumented race, P2 exact winner
   detection, P3 tag/readback oracle holding the overlapping pair.
3. P1-P3 implementation is gated on approval of
   [`PROLAPSE_P1_P3_DESIGN.md`](PROLAPSE_P1_P3_DESIGN.md).
4. P4/P5, lane_VA, KRW, and kernel payload work remain out of scope.
5. No result is claimed until a native run produces numbers; losing scheduling
   strategies go to [`DEAD_ENDS.md`](DEAD_ENDS.md) with counts.

The branch still has only this record and the supporting docs. The two
pre-existing untracked probes under `probes/phase17_new_vectors/` were left
untouched.

## Evidence anchors

| Topic | Anchor |
|---|---|
| Reconciliation | `docs/EVIDENCE_REVIEW.md` |
| P1-P3 design | `docs/PROLAPSE_P1_P3_DESIGN.md` |
| Configuration ledger | `docs/PROLAPSE_OFFSETS.md` |
| FontFace result | `docs/WEBKIT_FINDINGS.md` Finding 10, lines 378-433; update line 471 |
| AIO generation check | `docs/AIO_BATCH_14_10.md` lines 109-132 |
| Duplicate delete | `docs/PRIVATE_AIO_DUPLICATE_DELETE_2026-10-21.md` lines 7, 19 |
| Native AIO races | `docs/CHAIN_14_10.md` lines 53-74 |
| 14.20 diff and guard | `ps5-1410/STATUS.md` lines 349-368 |
