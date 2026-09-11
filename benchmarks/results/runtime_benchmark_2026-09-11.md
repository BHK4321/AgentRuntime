# AgentOS runtime benchmark — 2026-09-11

## Environment

- Windows, Release build
- Local PostgreSQL 17
- FastAPI -> gRPC -> C++ runtime -> PostgreSQL
- Runtime worker count: 4 (current server configuration)

## Durable end-to-end burst

Each trial submitted 100 independent `sleep` tasks with `seconds: 0` in one
HTTP request. Throughput spans the earliest persisted task creation time to the
latest `TaskCompleted` event. Ready-to-start latency spans persisted task
creation to its first `TaskStarted` event.

| Trial | Tasks/s | p95 ready-to-start (ms) | Elapsed (s) |
|---:|---:|---:|---:|
| 1 | 4.821 | 19,747.946 | 20.744 |
| 2 | 4.693 | 20,393.901 | 21.307 |
| 3 | 4.804 | 19,531.099 | 20.814 |
| 4 | 4.842 | 19,594.659 | 20.652 |
| 5 | 4.589 | 20,639.526 | 21.790 |
| **Median** | **4.804** | **19,747.946** | **20.814** |

These numbers include HTTP, gRPC, task/event persistence, execution, and the
current synchronous PostgreSQL event path. They are not scheduler-only numbers.

## Observed persistence bottleneck

The scheduler-only result and the durable result differ by more than two orders
of magnitude: 1,027.98 tasks/s in process versus a 4.804 tasks/s median through
the durable service path. Under a burst of 100 immediately ready tasks, median
trial p95 ready-to-start latency reached 19.748 seconds.

The current implementation serializes every PostgreSQL operation through one
`PostgresRuntimeStore` mutex. Each event opens a new database connection and
commits its own transaction. `TaskStarted` and `TaskCompleted` therefore contend
with worker heartbeat writes, while every running task also owns a lease-renewal
thread that emits synchronous heartbeats. These implementation details explain
the observed queueing behavior, although a profiler should be used to attribute
the exact share of time to connection setup, transaction commits, mutex waiting,
and thread management.

The next design should preserve the existing durability guarantees while
reducing work on scheduler threads. Candidate changes to evaluate are a bounded
asynchronous event writer, batched event and heartbeat transactions, persistent
database connections, and one lease-renewal loop for all active assignments.
The updated design must define flush and shutdown behavior, backpressure, event
ordering, and which state transitions must commit before an API call succeeds.

## Connection-pool redesign results

The first redesign replaces per-operation connection creation and the global
store mutex with a pool of persistent PostgreSQL connections. The gRPC runtime
uses four pooled connections, matching its four scheduler workers. Each caller
exclusively borrows one connection for the lifetime of its transaction, then an
RAII lease returns it to the pool. Task state transitions remain synchronous and
retain their existing transaction boundaries.

The same 100-task durable burst was repeated five times after the change:

| Trial | Tasks/s | p95 ready-to-start (ms) | Elapsed (s) |
|---:|---:|---:|---:|
| 1 | 581.348 | 163.533 | 0.172 |
| 2 | 678.481 | 140.942 | 0.147 |
| 3 | 737.719 | 128.925 | 0.136 |
| 4 | 561.681 | 171.263 | 0.178 |
| 5 | 702.627 | 135.803 | 0.142 |
| **Median** | **678.481** | **140.942** | **0.147** |

Compared with the original durable median, connection pooling improved
throughput by **141.2x** and reduced p95 ready-to-start latency by **140.1x**.

Ten additional forced-crash trials all recovered successfully. Their recovery
times were 352.687, 332.582, 332.219, 315.864, 439.256, 416.819, 306.087,
318.882, 318.885, and 319.424 ms. Median process-launch-to-resume time was
**325.822 ms**, and every trial contained one abandoned original execution and
one completed replacement execution.

All 25 registered CTest cases passed after the redesign, including gRPC submit,
cancel, recovery, lease stealing, and PostgreSQL recovery tests.

## Scheduler-only recorded result

The existing `scheduler_benchmark.csv` records 5,000 simulated 2 ms tasks at
1,027.98 tasks/s with 16 workers. This result is from the repository's previous
benchmark run and was not repeated as part of the five durable trials above.

## Crash recovery

Each trial submitted a durable two-second sleep task, waited for `Running`,
force-terminated `agentos_server`, launched a new process, and measured from
process launch to the second persisted `TaskStarted` event. A trial passed only
when PostgreSQL contained an `ABANDONED` original execution and a `COMPLETED`
replacement execution.

| Trial | Recovery time (ms) | Abandoned | Completed | Result |
|---:|---:|---:|---:|---|
| 1 | 451.501 | 1 | 1 | Pass |
| 2 | 338.322 | 1 | 1 | Pass |
| 3 | 315.974 | 1 | 1 | Pass |
| 4 | 370.163 | 1 | 1 | Pass |
| 5 | 332.617 | 1 | 1 | Pass |
| 6 | 320.670 | 1 | 1 | Pass |
| 7 | 306.664 | 1 | 1 | Pass |
| 8 | 348.078 | 1 | 1 | Pass |
| 9 | 329.357 | 1 | 1 | Pass |
| 10 | 371.234 | 1 | 1 | Pass |

- Success rate: **10/10 (100%)**
- Median process-launch-to-resume time: **335.470 ms**
- Slowest observed recovery: **451.501 ms**

## Correctness issue found during measurement

The gRPC server explicitly passed `{}` for `lease_timeout`, which constructed a
zero-millisecond timeout instead of using the scheduler's five-second default.
The initial recovery run therefore entered rapid task reclamation. The server
configuration was corrected to pass `std::chrono::seconds(5)`, rebuilt, and all
reported recovery and durable burst trials were run after that correction.
