# Pipelined commit: encode the WAL outside the commit serializer (experimental)

Pull-request text. The implementation sits on top of `feat/adaptive-commit-lock-scheduling` (memgraph#4777), which
is not merged; the base branch is that head merged into `master`.

## What this is, in plain terms

Today a Memgraph commit does three things while holding one lock (`commit_mutex_`): it hands out the commit
timestamp, it turns every change the transaction made into bytes and writes them to the write-ahead log (and sends
the same bytes to replicas), and it makes the transaction visible. Because the lock is held for all three, two
writers can never encode at the same time; the second one waits for the first to finish writing its log. For a
1,000-row upsert most of the time under that lock is spent producing the log bytes, so adding writers only adds
queueing.

Postgres solved the same problem long ago: a backend builds its WAL record in its own memory first, then takes a
short lock only to reserve a position in the log and copy the finished bytes in, and commit visibility is a
separate, ordered step. The log stays in order because positions are handed out in order, not because one
transaction at a time is allowed to do all of its work.

This change borrows that shape. With `--experimental-enabled=pipelined-commit`, a commit now:

1. takes the lock only long enough to get its timestamp and a ticket in the commit queue;
2. releases the lock and encodes its changes into a private buffer, in parallel with other committers;
3. waits for its ticket to come up, and only then checks unique constraints, copies the finished buffer into the
   log in one write, replicates, and becomes visible.

Nothing about what ends up in the log changes: the buffer is byte-for-byte what the old inline path would have
written, checksum included, and a harness proves the flag-off path still writes identical files. Replication,
two-phase commit and constraint checking are the existing code, just executed in ticket order. Transactions that
do not fit the fast path (index changes, storages without a log, commits that need two-phase commit, commits that
would exceed a memory budget) take the same ticket and run the old code after their turn comes, so every commit is
still ordered by one mechanism.

The flag is off by default and requires `lockfree-read-snapshot`, whose three-phase commit this builds on. The
constraint optimization in memgraph#4769 (skip unique-constraint bookkeeping for properties no constraint covers)
is independent and is the "after constraint optimization" column in the tables below.

## How big a change is it

About 1,800 lines of production code across 28 files, 4,100 lines of tests, and 640 lines of docs. Roughly half
the production code is new, self-contained pieces (a buffer encoder, a commit-order gate and ticket, a memory
budget); the other half is the commit path in `inmemory/storage.cpp` and the replication object, where the
existing durability continuation was moved under a ticket rather than rewritten. It touches the most sensitive
path in the storage engine, which is why the failure protocol (what happens when something throws between "log
bytes written" and "visible") takes up most of the design and most of the tests. It changes no file format, no
wire format and no query surface beyond the flag, the budget flag and five counters.

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

Local (16-core box, WAL on, `--storage-delta-on-identical-property-update=false`, the writer sweep from
`tests/manual/unique_constraint_property_update_bench.py`: 100,000 Node/Value pairs, 48 batches of 1,000 rows per
trial, medians of 3 trials; two launches per cell). Baseline is v3.12.0; "after constraint optimization" is
v3.12.0 plus memgraph#4769 (skip unique-constraint verification for unrelated property writes); "after WAL
optimization" is this branch with `lockfree-read-snapshot,pipelined-commit` on top of #4769. Milliseconds per
transaction throughout.

| Writers | Tx p50 baseline | after constraint optimization | after WAL optimization | CPU per tx baseline | after constraint optimization | after WAL optimization |
|---|---|---|---|---|---|---|
| 1 | 13.3 / 13.3 | 8.6 / 8.3 | 7.0 / 7.6 | 14.0 / 13.5 | 9.2 / 8.5 | 7.3 / 7.9 |
| 12 | 56.5 / 53.9 | 17.5 / 13.1 | 10.2 / 8.8 | 61.5 / 60.0 | 17.7 / 13.3 | 9.4 / 9.2 |

The same binary with the flags off measures 7.0 / 7.2 ms (1 writer) and 12.1 / 12.9 ms (12 writers) p50, so the
12-writer gain is the pipeline's: 12.5 to 9.5 ms p50, 14.3 to 9.3 ms CPU per transaction, 635 to 830 tx/s, p99
26 to 16 ms. With one writer the flag changes nothing, as designed. Counters: every measured transaction was
encoded outside the serializer, 0 budget fallbacks, 0 two-phase fallbacks.

Production instance (5-minute in-pod windows, upsert batches of up to 1,000 rows from 2 and 12 writer pods; the
same three builds as above, #4769 being the constraint optimization):

| Writer pods | MAIN cores baseline | after constraint optimization | after WAL optimization | Tx p50 baseline | after constraint optimization | after WAL optimization | Tx p90 baseline | after constraint optimization | after WAL optimization |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 2.40 | 1.48 | 1.03 | 28 | 11 | 12 | 63 | 21 | 22 |
| 12 | 5.18 | 1.97 | 1.30 | 81 | 24 | 16 | 277 | 140 | 53 |

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
