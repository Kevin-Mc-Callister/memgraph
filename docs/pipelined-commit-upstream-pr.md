# Pipelined commit: encode the WAL outside the commit serializer (experimental)

Draft pull-request text for memgraph/memgraph. Not opened; kept here until the base branch question is settled (the
implementation sits on top of `feat/adaptive-commit-lock-scheduling`, memgraph#4777, which is not merged).

## Problem

With `lockfree-read-snapshot` a main-side commit holds the commit serializer (`commit_mutex_`) from the mint through
durability, replication and publication. Unique-constraint validation and the read watermark both need commits to
become visible in mint order, so the serializer cannot simply be released earlier. The serial section therefore
contains work that is not order-sensitive at all: encoding every delta of the transaction into WAL bytes. For a
1,000-row batch on a production instance roughly 26 of 41 ms are serialized and the largest single piece is the
encoding of about 8,000 deltas. Adding writers adds queueing, not throughput.

## Design

Flag `--experimental-enabled=pipelined-commit`, startup-only, off by default, valid only together with
`lockfree-read-snapshot` (validated on the combined command-line plus environment mask, and again by the storage
constructor). Budget `--storage-pipelined-commit-max-bytes` (default 256 MiB).

An eligible main-side commit (in-memory transactional storage with a WAL, data deltas only, no metadata deltas) runs
in three stages:

| Stage | Holds | Work |
|---|---|---|
| S1 mint | `commit_mutex_`; `engine_lock_` briefly | mint the commit timestamp, register a `CommitTicket` with the `CommitOrderGate`, release both locks |
| S2 encode | accessor and transaction only | `MaterializeTxnCommands` (the existing traversal, extracted whole), `EncodeTxnCommandsTo` into a private, CRC-complete `TxnWalBuffer` through a budget-charging allocator |
| S3 ordered | gate ticket; `engine_lock_` only inside publish | `gate.Enter(ts)`, unique validation, `InitializeWalFile`, replication streams open, `AppendEncodedTransaction` (verbatim), `FinalizeWalFile`, ship, `FinalizeCommitPhase`, retire the ticket |

Only the WAL encoding leaves the serial section. Replication, publication and two-phase commit are the existing code
run in ticket order. Commits that are not eligible (metadata transactions, WAL-less storages, commits that turn out to
need 2PC because a STRICT_SYNC replica is registered, over-budget commits) take the same ticket and run the existing
durability code after entering the gate (`OrderedLegacyCommit`), so every main-side minted commit belongs to one
ordering domain. Replica-side writes never take a ticket: the replication server applies them on one thread in
main's order already.

Invariants kept:

- INV-ORDER: WAL frames in each file are in strictly increasing commit-timestamp order; replicas receive them in that
  order (streams open in S3, one transaction per client at a time).
- INV-PUBLISH: commit timestamps are published in mint order, so `last_committed_mvcc_ts_` stays contiguous and the
  schema-info queue never receives an insertion below its processed bound.
- INV-UNIQUE: a commit validates unique constraints only after every earlier-minted commit has published or fully
  aborted.
- Failure semantics are unchanged: partial WAL writes and fsync failures stay fatal, a SYNC replica failure still
  reports the transaction as committed, aborts never advance the read watermark.
- The flag-off path is the same code; a deterministic harness (`tests/unit/storage_v2_off_identity.cpp`) run on the
  base and on this branch produces byte-identical WAL files.

Maintenance sites that used to take `commit_mutex_` to exclude an in-flight committer now call
`InMemoryStorage::QuiesceCommits()` (take `commit_mutex_`, then wait until every issued ticket has retired): epoch
change, recovery-step selection, current-WAL transfer, recovery completion, heartbeat reconciliation, and shutdown's
final WAL exclusion (after the async indexer and TTL workers have stopped).

## Ticket lifecycle

`CommitTicket` is a noncopyable owning object with states registered, entered, published or aborted, plus an
independent irreversible mark and a WAL record state (not started, incomplete, complete). `CommitWithTicket` is the
sole owner of abort cleanup and retirement: one exception boundary covers everything after registration; a failed
local abort terminates and is never retried (`AbortTicketOrTerminate`); an exception after the irreversible mark or
while a record is incomplete terminates; an exception after a complete `commit=false` prepare record runs the single
2PC abort continuation once (`AbortTwoPcOrTerminate`: WAL finalization, then the abort decisions, each step set done
only after it returned); an abortable exit before any WAL frame drains replica borrowers and discards the unprepared
streams (connection retired while the stream still owns the RPC lock, stream reset, client MAYBE_BEHIND), performed
only by the scope that owns the replication object. `TransactionReplication` gains a trailing `CommitTicket *`,
constructor rollback for already-opened streams, and, on ticketed execution only, the same retire-reset-MAYBE_BEHIND
sequence for the ASYNC arm of an abort decision.

## Budget

Every retained S2 allocation (materialized commands, tracking sets, the vertex cache, the WAL buffer) goes through
`BudgetAllocator`, which charges the per-database `PipelineBudget` before allocating and releases in `deallocate`.
Refusal never blocks: it converts the commit into the ordered legacy path after destroying every charged allocation.

## Deferred

- Gate parking: a committer waiting at the gate blocks its worker thread. The interpreter's commit-lock parking is
  unchanged and does not cover gate waits; the end-to-end liveness script shows readers are still admitted during
  saturation because commit-lock parking frees workers, but there is no latency bound at the gate.
- A private replication transport: replica payloads are still encoded by the per-replica tasks in S3.

## Measurements

Box: 16 cores, WAL on, `--storage-delta-on-identical-property-update=false`, the writer sweep from
`tests/manual/unique_constraint_property_update_bench.py` (100,000 Node/Value pairs, batches of 1,000 rows, medians
of 3 trials). Latencies in milliseconds per transaction. See `docs/pipelined-commit-borgdev-report.md` for the full
tables (48- and 96-batch trials) and the production measurement.

Quiet round, 96 batches per trial (latencies and CPU in milliseconds per transaction):

| Launch | Writers | tx/s | CPU ms/tx | p50 ms | p99 ms |
|---|---|---|---|---|---|
| base, lockfree-read-snapshot | 1 | 64.2 | 15.52 | 15.2 | 25.7 |
| | 4 | 118.6 | 24.58 | 32.6 | 48.5 |
| | 8 | 99.4 | 30.10 | 62.3 | 174.1 |
| | 12 | 94.6 | 32.29 | 96.5 | 253.6 |
| this branch, lockfree-read-snapshot | 1 | 76.3 | 13.23 | 12.5 | 22.5 |
| | 4 | 122.9 | 23.02 | 29.5 | 48.4 |
| | 8 | 112.7 | 27.29 | 57.7 | 131.7 |
| | 12 | 101.0 | 30.21 | 89.4 | 288.4 |
| this branch, lockfree-read-snapshot + pipelined-commit | 1 | 86.2 | 11.56 | 11.1 | 20.4 |
| | 4 | 159.6 | 13.33 | 23.8 | 32.7 |
| | 8 | 131.9 | 14.90 | 56.7 | 78.7 |
| | 12 | 115.9 | 16.56 | 98.1 | 115.6 |

Shorter 48-batch trials on the same box gave the same shape (4 writers: 157.0 tx/s lock-free vs 177.5 pipelined;
8 writers: 136.2 vs 161.5; 12 writers: 135.6 vs 148.8; server CPU per transaction 18.5 to 24.6 ms vs 12.7 to
15.0 ms; p99 42 to 218 ms vs 34 to 87 ms). The box is shared, so single-writer numbers varied by up to 30% between
rounds; the multi-writer gap and the CPU-per-transaction drop held in every round. With the flag off the branch
matches the base within noise.

Counters after the pipelined launch: `pipelined_commit_budget_fallbacks` 0, `pipelined_commit_two_pc_fallbacks` 0.

## Tests

- `storage_v2_buffer_encoder`: byte and CRC identity with the file encoder, budget charging before growth and
  release on destruction, refusal injection, rebound containers.
- `storage_v2_commit_order_gate`: ordered entry under shuffled arrival, WaitIdle, retirement rules, destructor
  termination on an unretired ticket.
- `storage_v2_wal_file`: encoded append byte-identical to the direct append, mixed direct and buffered transactions
  summarize and recover, commit-flag patch at relocated offsets.
- `storage_v2_pipelined_commit`: flag dependency, quiescence (synthetic ticket and in-flight pipelined commit),
  serializer exclusion for legacy writers, INV-UNIQUE for concurrent duplicate creates and updates with and without
  the budget fallback (both refusal sites), legacy commits not overtaking pipelined tickets, INV-ORDER with unequal
  encode sizes, rotation, the legacy-forced rotation with one finalization per transaction, WAL-less storages, the
  lifecycle faults (after_mint, after_ticket, materialization fault, before_publish) and the death cases
  (after_append, after_finalize_wal, after_publish, WAL-disabled after_publish, failed abort), the first eligible
  commit on an empty WAL and rotation through the real `InitializeWalFile`.
- `storage_v2_replication`: STRICT_SYNC fallback through 2PC, a refused prepare vote, an exception after a complete
  prepare record, refused abort decisions for both continuation callers, live-borrower aborts on the direct SYNC
  pipeline, the locally owned legacy fallback and the borrowed STRICT_SYNC fallback, all-unscheduled aborts before
  any frame in all three scopes, partial replication-object construction, ticket-gated probe exclusion, replica
  ordering and counts under eight concurrent writers, and, with each replica in its own process (the replica
  handlers' 2PC cache is static), the abort-step death cases (WAL finalization, decision scheduling) and the mixed
  STRICT_SYNC + ASYNC complete-prepare abort with both forced interleavings.
- `storage_v2_durability_inmemory`: a pipelined `WalDeathResilience` variant (six writers killed at a random point).
- `utils_priority_thread_pool`: sixteen commit-shaped tasks on four workers with a slow head.
- `tests/manual/pipelined_commit_worker_liveness.py`: sixteen Bolt writers and a reader against a real server with
  four Bolt workers while the head is parked in its encode stage; PERIODIC COMMIT and a before-commit trigger.
- The full storage sweep passes with no new skips; the assertion-enabled build of the new suites passes.

## Scope

Storage/v2 and replication only. No new query surface beyond the flag, the budget flag, and the counters in
`SHOW STORAGE INFO ON CURRENT DATABASE` (`pipelined_commit_s2_encodes`, `pipelined_commit_budget_fallbacks`,
`pipelined_commit_two_pc_fallbacks`, `pipelined_commit_gate_wait_ns`, `pipelined_commit_s3_ns`).
