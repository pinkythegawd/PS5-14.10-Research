# Runbook — operational rules for this tree

## Untracked probe files — never `git clean` on this tree

`probes/phase17_new_vectors/434a_sys637_store_analysis.lua` and
`probes/phase17_new_vectors/435a_umtx_op_matrix.lua` are in-progress phase17
analysis and are intentionally untracked. `git clean -xdf` (or `-x`/`-X`)
deletes them. Do not run any `git clean` variant here while they exist —
commit or move them deliberately first.
