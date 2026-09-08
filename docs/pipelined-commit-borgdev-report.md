# Pipelined commit: implementation, verification and borgdev report

Written 2026-09-08. Implements `docs/superpowers/plans/2026-09-07-memgraph-pipelined-commit.md` (tgraph-api, revision
13) per the handoff `docs/superpowers/plans/2026-09-08-memgraph-pipelined-commit-handoff.md`.

## Branches and images

| Item | Value |
|---|---|
| Implementation branch | `Kevin-Mc-Callister/memgraph` `pipelined-commit`, head `HEAD_SHA` |
| Base | `master` at `be32bab6bebef32c8d1eafb4420fe8f6221dd5ba`, merged with memgraph#4777 head `cd7a8f8814617eab2357da203cd81a28a090908b` (clean merge, no conflicts) |
| Integration branch | `borgdev-integration` = `pipelined-commit` + memgraph#4769 (`skip-unique-verification-for-unrelated-properties`, `c8ff27f1aaba`), head `INTEGRATION_SHA` |
| Image | `docker.io/transparapull/memgraph-dev:2`, digest `IMAGE_DIGEST` |
| Binary version | `memgraph version 3.13.0+…` from the master-based tree; durability format `kVersion = 37` (v3.12.0 writes 36) |
| Build container | `mgpatch2`, tree `/home/mg/memgraph-pipeline` (toolchain v8 installed at `/opt/toolchain-v8`) |
| No GitHub pull request was opened | PR text drafted in `docs/pipelined-commit-upstream-pr.md` |

Commits on `pipelined-commit` (oldest first):

COMMIT_LIST

## What was implemented

Tasks 0 through 8 of the plan, in full; Task 9 as a document (`docs/pipelined-commit-upstream-pr.md`). See the
plan for the design; `docs/pipelined-commit-base-notes.md` maps the plan's citations to the merged tree.

## Local verification

### Unit suites (Release build, this branch)

| Suite | Result |
|---|---|
| storage_v2_buffer_encoder (new) | 5 passed |
| storage_v2_commit_order_gate (new) | 6 passed |
| storage_v2_wal_file | 87 passed, 5 skipped (81 + 6 new; skips unchanged from the base) |
| storage_v2_pipelined_commit (new) | 27 passed |
| storage_v2_replication | 41 passed (23 + 18 new, including the process-isolated two-replica cases) |
| storage_v2_durability_inmemory | 161 passed (158 + 3 new crash-resilience instances) |
| storage_v2_lockfree_read_snapshot | 27 passed |
| storage_v2_constraints | 87 passed, 29 skipped (unchanged) |
| utils_priority_thread_pool | 28 passed (27 + 1 new) |

Assertion-enabled build (`build-asserts`, memgraph's own code compiled without `NDEBUG` on the Release conan
dependencies; a full Debug dependency set did not fit the 21 GB of free disk): buffer encoder 5, gate 6, wal_file
87, pipelined_commit 27, replication 41, all passed. The Release build covers the plan's NDEBUG requirement for
test 10 and the rotation test.

FULL_SWEEP

### OFF identity

`tests/unit/storage_v2_off_identity` built at the Task 0 commit (`f642896be`) and at the branch head, run with the
flags off: one WAL file each, 22,120 bytes, transactions 1 to 386, identical SHA-256
(`d76dfab621290487c4d6bef76cafbab9b90a3b80906b1f5a4eea64c63c8c4e86`).

### End-to-end liveness (`tests/manual/pipelined_commit_worker_liveness.py`)

Server with 4 Bolt workers and both flags, 16 writer processes and one reader, the head parked in its encode stage
for 2 seconds by the out-of-band file: every writer finished within 12 ms of the release, the reader was admitted
during saturation (828 ms), PERIODIC COMMIT and a before-commit trigger completed, 23 pipelined encodes, 0 budget
fallbacks.

### Task 8 A/B (this 16-core box)

`tests/manual/unique_constraint_property_update_bench.py`, 100,000 Node/Value pairs, batches of 1,000 rows, WAL on,
`--storage-delta-on-identical-property-update=false`, medians of 3 trials, latencies in milliseconds per transaction,
server CPU in milliseconds per transaction.

Run 1 (48 batches per trial):

| Launch | Writers | tx/s | rows/s | CPU ms/tx | p50 ms | p99 ms |
|---|---|---|---|---|---|---|
| Task 0 binary, no experiments | 1 | 81.5 | 81,481 | 12.29 | 11.5 | 22.0 |
| | 4 | 153.3 | 153,275 | 23.96 | 22.1 | 47.5 |
| | 8 | 134.2 | 134,233 | 49.37 | 41.1 | 151.5 |
| | 12 | 127.1 | 127,122 | 69.17 | 51.7 | 206.6 |
| Task 0 binary, lockfree-read-snapshot | 1 | 82.5 | 82,492 | 12.29 | 11.7 | 18.3 |
| | 4 | 165.2 | 165,195 | 18.12 | 21.6 | 38.1 |
| | 8 | 146.8 | 146,815 | 21.67 | 41.5 | 99.4 |
| | 12 | 133.6 | 133,632 | 24.58 | 69.5 | 175.9 |
| Final binary, no experiments | 1 | 84.6 | 84,639 | 11.88 | 11.4 | 22.0 |
| | 4 | 143.3 | 143,278 | 25.83 | 24.2 | 47.3 |
| | 8 | 147.3 | 147,330 | 43.75 | 36.7 | 126.4 |
| | 12 | 138.3 | 138,276 | 66.04 | 52.3 | 262.7 |
| Final binary, lockfree-read-snapshot | 1 | 86.4 | 86,392 | 11.67 | 11.2 | 18.0 |
| | 4 | 157.0 | 156,951 | 18.54 | 22.7 | 42.2 |
| | 8 | 136.2 | 136,217 | 21.87 | 42.4 | 146.1 |
| | 12 | 135.6 | 135,588 | 24.58 | 64.0 | 218.4 |
| Final binary, lockfree-read-snapshot + pipelined-commit | 1 | 85.5 | 85,471 | 11.67 | 11.1 | 19.0 |
| | 4 | 177.5 | 177,485 | 12.71 | 19.5 | 33.9 |
| | 8 | 161.5 | 161,520 | 13.96 | 46.0 | 59.3 |
| | 12 | 148.8 | 148,792 | 15.00 | 75.7 | 87.2 |

Pipelined launch counters: `s2_encodes` 788 (every measured transaction), `budget_fallbacks` 0, `two_pc_fallbacks` 0,
`gate_wait_ns` 18.3 s total, `s3_ns` 4.4 s total. Abort rate: 0 (the benchmark asserts no errors).

Run 2 (96 batches per trial; the box is shared and the last two launches of the first 96-batch pass were hit by
other load, so the three launches that matter were repeated twice more in the same order; the quiet second round
is shown, the noisy rounds are in `/home/mg/logs/bench_all_96.log` and `bench_key.log` in the container):

| Launch | Writers | tx/s | rows/s | CPU ms/tx | p50 ms | p99 ms |
|---|---|---|---|---|---|---|
| Task 0 binary, lockfree-read-snapshot | 1 | 64.2 | 64,247 | 15.52 | 15.2 | 25.7 |
| | 4 | 118.6 | 118,622 | 24.58 | 32.6 | 48.5 |
| | 8 | 99.4 | 99,427 | 30.10 | 62.3 | 174.1 |
| | 12 | 94.6 | 94,573 | 32.29 | 96.5 | 253.6 |
| Final binary, lockfree-read-snapshot | 1 | 76.3 | 76,296 | 13.23 | 12.5 | 22.5 |
| | 4 | 122.9 | 122,911 | 23.02 | 29.5 | 48.4 |
| | 8 | 112.7 | 112,736 | 27.29 | 57.7 | 131.7 |
| | 12 | 101.0 | 101,011 | 30.21 | 89.4 | 288.4 |
| Final binary, lockfree-read-snapshot + pipelined-commit | 1 | 86.2 | 86,172 | 11.56 | 11.1 | 20.4 |
| | 4 | 159.6 | 159,629 | 13.33 | 23.8 | 32.7 |
| | 8 | 131.9 | 131,930 | 14.90 | 56.7 | 78.7 |
| | 12 | 115.9 | 115,904 | 16.56 | 98.1 | 115.6 |

Single-writer throughput on this shared box varied between 57 and 86 tx/s for the same configuration across
rounds, which bounds what these numbers can show; the pattern that held in every round is the one above.

Reading: single-writer p50 is within noise across all five launches; with 4 to 12 writers the pipelined launch
commits more transactions per second than the lock-free baseline at a fraction of the server CPU per transaction and
with a much lower p99; the flag-off launch of the final binary matches the Task 0 binary within noise. Counters after
the pipelined launch: `budget_fallbacks` 0, `two_pc_fallbacks` 0; every eligible commit was encoded in S2.

## Review protocol

Opus reviewers ran per task (spec compliance, then quality) and over the whole diff. Findings and dispositions:

### Tasks 1-3, spec compliance (Opus): compliant with notes

- SHOULD-FIX, accepted: `CommitOrderGate::HeadTicket()` and `CommitTicket::published()` were unused; removed.
- SHOULD-FIX, rejected: passing the durable timestamp into `MaterializeTxnCommands`. The plan's interface is
  `MaterializeTxnCommands(TxnAllocPolicy, std::function<void()> const &)` with no timestamp; the traversal's
  callback ignores its timestamp argument and the plan says the extraction hands the collector what the inline
  path did. Kept as specified.
- Notes recorded, no change: no `pipeline_budget.cpp` (header-only), `BudgetVector` lives in `pipeline_budget.hpp`,
  three extra buffer-encoder tests, the 64-byte minimum allocation.

### Tasks 1-3, quality (Opus): no blocking findings

- SHOULD-FIX, rejected: sharing the wire format between `Encoder<FileType>` and `BufferEncoder` through a
  sink-parameterized base. The plan prescribes `BufferEncoder final : public BaseEncoder` with its own writers and
  states that `WriteString` cannot be copied literally; refactoring the upstream file encoder is outside the plan's
  scope, and the byte-identity test guards divergence. Kept.
- SHOULD-FIX, rejected: `progress` as a template parameter instead of `std::function const &`. The plan's interface
  is the `std::function` form; one indirect call per delta was not measurable in the flag-off benchmark.
- SHOULD-FIX, accepted: `AppendEncodedTransaction` now asserts a filled buffer (timestamp and frame count).
- SHOULD-FIX, accepted: the budget's peak accounting (up to three times a buffer's final size during growth) is
  documented on `PipelineBudget`.
- SHOULD-FIX, accepted in spirit: the gate death tests now match the `terminate called` line; the assertion text
  itself goes to the logger, not stderr, so it cannot be matched.
- Notes accepted: the stale "only writer" comment on `Encoder::Write`. Notes recorded, no change: `mutable` gate
  members (the plan's interface), the `frame_count` loop in `CompleteTransactionBookkeeping` (the plan's text), the
  test-only `TxnWalBuffer(PipelineBudget &)` constructor, the fixed temp directory in the buffer-encoder test.

### Tasks 4-6, spec compliance (Opus): approve, two test gaps

- Items 1 through 10 of the plan's contract were found fully compliant (flags, hooks and their firing sites, the
  five quiescence conversions and shutdown ordering, the dispatch point, `CommitWithTicket`'s ownership,
  `OrderedLegacyCommit`'s three outcome branches, `PipelinedCommit`, `TransactionReplication`'s rollback and the
  ticketed ASYNC arm, the record brackets, the irreversible mark).
- SHOULD-FIX, accepted: the `between_frames` incomplete-record death test (plan item 9) was missing; added as
  `PipelinedBetweenFramesDeathTest` with a process-isolated STRICT_SYNC replica.
- SHOULD-FIX, accepted: the two-replica `throw_before_enqueue_for` live-borrower variant (plan item 3b) was
  missing; added as `PipelinedPartialSchedulingTest` for the direct pipeline and the legacy scope.
- Notes recorded, no change: Task 7's replica-ordering test landed in the same commit; the unconditional
  test-counter increments (`finalize_wal_calls`, `decision_calls`, `abort_rpc_client_calls_`) and the
  `dynamic_cast` in `FinalizeTransaction`; `AbortTwoPcOrTerminate` guards `FinalizeWalFile` with `if (wal_file_)`
  like the base's own 2PC arm.

### Tasks 4-7, quality (Opus): four blocking test-robustness findings, all fixed

- BLOCKING, fixed: assertions returning over a live committer thread in the mixed STRICT_SYNC + ASYNC tests
  (`JoinedThread` releases the park and joins on every exit).
- BLOCKING, fixed: the pool liveness test skipped shutdown on a failed assertion (it now always shuts the pool
  down, then asserts; the poll bound outlasts the asserted bound).
- BLOCKING, fixed: `std::latch::count_down` on a satisfied latch when the successor commit fired the same hook
  (one-shot guards).
- BLOCKING, fixed: hooks and probes leaked on early return (`HookGuard` clears them on every exit; the death
  children are exempt).
- SHOULD-FIX, fixed: `replication_test_hooks_` is now `std::atomic`; the `commit_num_committed_txns_` comment no
  longer claims an engine-lock hold; the forked crash-resilience child ends with `_exit` and its recovery check now
  asserts prefix consistency (the sum over writers of `p + 1` equals the number of complete non-seed transactions);
  the process fixture ignores SIGPIPE, retries on EINTR, uses close-on-exec pipes, closes descriptors on spawn
  failure, reports a replica that crashed or exited nonzero, and moves the replica's stdout off the IPC channel;
  the liveness script terminates stuck processes, verifies that no writer finished before the release, and starts
  the reader's clock before the connection; a `QuiesceCommits` comment records the retirement-wait invariant;
  death children lower `RLIMIT_CORE`.
- SHOULD-FIX, rejected: the `decision_schedule_throws_once` injection "orphans" a decision task. The plan
  prescribes this fault site; `AbortTwoPcOrTerminate` terminates in its catch before any destructor of the
  replication object runs, so the orphaned task never outlives its owner.
- SHOULD-FIX, noted, no change: `FinalizeCommitHandler` reads the replica hooks after its early returns (a
  one-shot refusal armed for a transaction that never reaches the abort stays armed; test-only); fixed ports and
  temp names in the replication suite are the suite's existing convention.
- Notes recorded: the `bad_alloc` fallback does not count as a budget fallback (the plan increments only on
  refusal); `on_commands_released` fires for both scopes on a fallback (tests filter by scope).

The fixture's new exit-status check also surfaced a real teardown defect: a replica quitting with a prepared
transaction still cached in the static 2PC slot crashed at static destruction; the replica role now aborts it
before tearing down.


## borgdev

BORGDEV_SECTION

## Deviations from the plan and the handoff

DEVIATIONS

## Left for Kevin

- Merge tgraph-api PR #918 (plan docs) if not yet merged; decide on the upstream PR (`docs/pipelined-commit-upstream-pr.md`).
- The tgraph-event replica count for the comparison (measured at the count found on the cluster; see above).
- Restore `tgraph-controller` to 1 replica and re-register replicas when the experiment is over; the StatefulSet
  image is set by hand and a tinstaller reinstall reverts it.
- Whether to keep the `master`-based image on borgdev: its durability format (37) cannot be read by `memgraph-dev:1`
  (3.12.0, format 36), so rolling back after it has written requires restoring the backup.
