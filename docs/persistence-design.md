# PostgreSQL persistence design

## Problem

`PostgresRuntimeStore` originally protected every database operation with one
mutex and constructed a new `pqxx::connection` inside that critical section.
Task events, state transitions, execution attempts, and worker heartbeats all
queued behind connection setup and transaction commit. A burst of 100 durable
zero-second tasks consequently achieved a median 4.804 tasks/s and a median
trial p95 ready-to-start latency of 19.748 seconds.

## Implemented design

`PostgresConnectionPool` owns a fixed set of persistent connections. Callers
wait on a condition variable when every connection is busy. An acquired
connection is represented by a move-only RAII lease, so normal returns and
exceptions both return the slot to the pool.

The gRPC runtime has four scheduler workers and uses four pooled connections.
`PostgresRuntimeStore` and `PostgresEventStore` no longer hold a store-wide
database mutex. Each transaction exclusively owns its borrowed connection, so
independent workers can persist concurrently without sharing a `pqxx`
connection.

The pool deliberately does not make persistence asynchronous. `TaskStarted`,
`TaskCompleted`, failure, cancellation, and reclamation operations still commit
before `append` returns. This preserves the recovery contract and avoids a
window where memory reports a state transition that PostgreSQL has not recorded.

## Measured result

With four workers and four pooled connections, the same five-trial durable
benchmark reached:

- 678.481 tasks/s median throughput, up from 4.804 tasks/s
- 140.942 ms median-trial p95 ready-to-start latency, down from 19,747.946 ms
- 10/10 successful forced-crash recoveries
- 325.822 ms median process-launch-to-resume recovery time

The full trial data and methodology are in
`benchmarks/results/runtime_benchmark_2026-09-11.md`.

## Correctness boundaries

- A connection belongs to only one transaction at a time.
- Pool leases are move-only and return connections during exception unwinding.
- Recovery still marks prior `RUNNING` executions abandoned before loading work.
- Reclamation remains fenced by task and worker identity.
- State-changing event transactions remain synchronous.

## Remaining work

Connection pooling removes the measured dominant bottleneck without weakening
durability. Further optimization should be driven by profiling. Likely targets
are batching worker heartbeats, replacing per-task renewal threads with one
renewal loop, and batching informational events. Any asynchronous writer needs
a bounded queue, backpressure, deterministic shutdown flushing, and explicit
rules for which transitions must remain synchronous.
