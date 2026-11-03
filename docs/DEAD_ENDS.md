# Dead ends — Prolapse P1-P3

This file receives measured negative schedules. It is not a place for guesses.

## Entry format

Each P1 strategy that completes its round budget with zero `both_zero` results
gets one entry with: run ID, firmware/profile, strategy, batch, rounds,
terminal timeouts, one-zero count, both-error count, unexpected count, window
statistics, and the build SHA-256. A strategy is not listed until the run
exists.

## Entries

Entries are appended by a P1 run when a strategy reaches
`race.loss_threshold` with zero wins, and by a run that trips the thread cap.
No entry in this section is reconstructed by hand. The design is at
[`PROLAPSE_P1_P3_DESIGN.md`](PROLAPSE_P1_P3_DESIGN.md).
