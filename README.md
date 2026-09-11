# AgentOS

AgentOS is a local C++ execution runtime for dependency-aware, concurrent task
workflows. PostgreSQL stores task definitions, dependencies, execution attempts,
worker heartbeats, leases, and runtime events. A FastAPI service is the HTTP
front door and submits work to the C++ runtime over gRPC.

## Architecture

```text
FastAPI (api/main.py)
        | gRPC SubmitWorkflow / CancelTask
        v
C++ scheduler + worker pool     ----->  PostgreSQL
(runtime/scheduler.hpp)                 tasks, deps, executions,
                                        workers, runtime_events
```

The C++ scheduler supports dependency graphs, FIFO ready-task scheduling,
concurrent workers, retries, deadlines, cancellation, idempotency metadata,
heartbeats, leases, and durable task types. The built-in durable handler is
`sleep`. `cpp_callback` lambdas are process-local and cannot be recovered.

## Requirements

- Windows PowerShell
- CMake 3.20 or newer
- Visual Studio 2022 C++ Build Tools (`cl.exe`)
- PostgreSQL 17 on `localhost:5432`
- vcpkg at `E:\vcpkg`
- Python 3.11
- `curl.exe` (included with current Windows)

All commands below assume the workspace root:

```powershell
cd E:\OS
```

## Repository Layout

```text
AgentOS/
├── CMakeLists.txt
├── README.md
├── database/
│   └── schema.sql
├── runtime/
│   ├── main.cpp                      # demo binary (hardcoded A→E graph)
│   ├── grpc_runtime_server.cpp       # gRPC service used by the API
│   ├── scheduler.hpp
│   ├── task.hpp
│   ├── task_graph.hpp
│   ├── task_handler_registry.hpp
│   ├── postgres_runtime_store.hpp
│   └── proto/
│       └── agentos.proto
├── api/
│   ├── main.py
│   ├── grpc_runtime_client.py
│   └── requirements.txt
├── tests/
├── scripts/
│   └── run-all-tests.ps1             # env + build-grpc + ctest
└── benchmarks/
```

## Environment variables

| Variable | Default | Used by |
|---|---|---|
| `AGENTOS_DATABASE_URL` | `postgresql://agentos:agentos@localhost:5432/agentos` | C++ binaries and FastAPI |
| `AGENTOS_RUNTIME_ADDRESS` | `127.0.0.1:50051` | FastAPI gRPC client |
| `AGENTOS_WORK_DIR` | `<current directory>/AgentOS/work` | Durable file-task sandbox |
| `PATH` | must include vcpkg and PostgreSQL `bin` | C++ processes loading `libpq` / vcpkg DLLs |

Local connection string:

```text
postgresql://agentos:agentos@localhost:5432/agentos
```

---

## Build and run (full stack)

Do these steps in order. First-time `grpc:x64-windows` compilation from source
often takes **1–2 hours**. Later AgentOS rebuilds are minutes.

### 1. Start PostgreSQL

```powershell
Start-Service postgresql-x64-17
Get-Service postgresql-x64-17
```

The service must be `Running` before anything else.

### 2. Create the database (once)

As the PostgreSQL administrator:

```powershell
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" `
  -h localhost -U postgres -d postgres -W
```

Inside `psql`:

```sql
CREATE ROLE agentos LOGIN PASSWORD 'agentos';
CREATE DATABASE agentos OWNER agentos;
\c agentos
\i E:/OS/AgentOS/database/schema.sql
GRANT USAGE ON SCHEMA public TO agentos;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO agentos;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO agentos;
```

If the role already exists:

```sql
ALTER ROLE agentos WITH PASSWORD 'agentos';
```

Verify:

```powershell
$env:PGPASSWORD = "agentos"
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" -w `
  -h localhost -U agentos -d agentos `
  -c "SELECT current_user, current_database();"
```

### 3. Install C++ dependencies with vcpkg (once)

```powershell
E:\vcpkg\vcpkg.exe install libpqxx:x64-windows
E:\vcpkg\vcpkg.exe install grpc:x64-windows
```

`grpc:x64-windows` builds debug and release static libraries. Do not start a
second install while one is already running.

Confirm headers exist:

```powershell
Test-Path E:\vcpkg\installed\x64-windows\include\pqxx\pqxx
Test-Path E:\vcpkg\installed\x64-windows\include\grpcpp\grpcpp.h
```

Both must return `True` before CMake.

### 4. Create the Python environment and generate bindings

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r AgentOS/api/requirements.txt
```

Generate Python gRPC stubs from the shared contract (do this after every
`agentos.proto` change):

```powershell
New-Item -ItemType Directory -Force AgentOS/api/generated | Out-Null
New-Item -ItemType File -Force AgentOS/api/generated/__init__.py | Out-Null
python -m grpc_tools.protoc `
  -I AgentOS/runtime/proto `
  --python_out=AgentOS/api/generated `
  --grpc_python_out=AgentOS/api/generated `
  AgentOS/runtime/proto/agentos.proto
```

Confirm:

```powershell
python -c "from api.generated import agentos_pb2_grpc; print('PROTO_OK')"
```

If that import fails, run it from `E:\OS` or set
`$env:PYTHONPATH = "E:\OS\AgentOS"`.

### 5. Configure and build C++ targets

Use **one** CMake tree, `AgentOS/build-grpc`, for the server, in-process
tests, Postgres recovery, and gRPC tests.

```powershell
cmake -S AgentOS -B AgentOS/build-grpc `
  -DAGENTOS_ENABLE_POSTGRES=ON `
  -DAGENTOS_ENABLE_GRPC=ON `
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=E:/vcpkg/installed/x64-windows
cmake --build AgentOS/build-grpc --config Release
```

That produces:

```text
AgentOS\build-grpc\Release\agentos_server.exe
AgentOS\build-grpc\Release\agentos_tests.exe
AgentOS\build-grpc\Release\agentos_grpc_tests.exe
AgentOS\build-grpc\Release\agentos.exe
AgentOS\build-grpc\Release\agentos_benchmark.exe
```

Build a single target if you only need the API runtime:

```powershell
cmake --build AgentOS/build-grpc --config Release --target agentos_server
```

Optional second tree for the Postgres demo only (`A → B/C → D → E`), if you
do not want gRPC in that binary’s build:

```powershell
cmake -S AgentOS -B AgentOS/build-postgres `
  -DAGENTOS_ENABLE_POSTGRES=ON `
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=E:/vcpkg/installed/x64-windows
cmake --build AgentOS/build-postgres --config Release --target agentos
```

### 6. Run all tests (single ctest path)

PostgreSQL must be running. `postgres_recovery` and the `grpc_*` tests share
the `agentos` database; CTest serializes those tests with a resource lock.
Stop `agentos_server` first so recovery tests do not abandon live rows.

From `E:\OS`, this is the only command you need after a successful configure:

```powershell
powershell -File AgentOS\scripts\run-all-tests.ps1
```

The script sets `AGENTOS_DATABASE_URL` and `PATH`, builds `AgentOS/build-grpc`
(Release), then runs:

```powershell
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
ctest --test-dir AgentOS/build-grpc -C Release --output-on-failure
```

That `ctest` line is the single test entry point. With
`AGENTOS_ENABLE_POSTGRES=ON` and `AGENTOS_ENABLE_GRPC=ON` it registers:

| Group | Names |
|---|---|
| In-process scheduler | `dependency_order`, `concurrent_tasks`, `dynamic_submission`, `mixed_submission`, `completed_dependency`, `validation`, `atomic_rejection`, `incremental_validation`, `failure_propagation`, `retry_success`, `retry_exhausted`, `events`, `deadline`, `cancellation`, `lease_expiry`, `lease_renewal`, `cancel_state`, `idempotency`, `task_context`, `durable_events` |
| Postgres | `postgres_recovery` |
| gRPC + Postgres | `grpc_submit`, `grpc_cancel`, `grpc_recovery`, `grpc_lease_steal` |

Equivalent CMake target (same directory, same env vars as above):

```powershell
cmake --build AgentOS/build-grpc --config Release --target test-all
```

Filter one test:

```powershell
ctest --test-dir AgentOS/build-grpc -C Release --output-on-failure -R lease_expiry
```

Optional in-process-only tree (no gRPC, no Postgres required except
`postgres_recovery` which is not registered here):

```powershell
cmake -S AgentOS -B AgentOS/build
cmake --build AgentOS/build --config Debug --target agentos_tests
ctest --test-dir AgentOS/build -C Debug --output-on-failure
```

Optional benchmark (from either tree after a Release build):

```powershell
cmake --build AgentOS/build-grpc --config Release --target agentos_benchmark
.\AgentOS\build-grpc\Release\agentos_benchmark.exe
```

When the binaries exist, start the processes in **Start each service**.

---

## Start each service

Do this every time you want to use the stack. Skip the vcpkg/CMake steps
unless you changed C++ or proto sources. Use **four terminals** and leave
them running. Issue `curl.exe` from a fifth window (or a new tab).

### Terminal 1 — PostgreSQL

```powershell
Start-Service postgresql-x64-17
Get-Service postgresql-x64-17
```

Status must be `Running`. This is the durable store. Nothing else starts
usefully without it.

### Terminal 2 — C++ gRPC runtime (`agentos_server`)

This process owns the scheduler and worker pool. FastAPI cannot submit work
without it. Keep it in the foreground.

```powershell
cd E:\OS
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
.\AgentOS\build-grpc\Release\agentos_server.exe
```

Expected output (leave this window open):

```text
AgentOS gRPC runtime listening on 0.0.0.0:50051
```

If this process is not running, a TCP connect to `127.0.0.1:50051` fails with
connection refused (`WinError 10061`). If bind fails with WSA 10048, another
`agentos_server` is already listening.

### Terminal 3 — FastAPI HTTP front door

Activate the same venv created during the first build.

```powershell
cd E:\OS
.\.venv\Scripts\Activate.ps1
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
$env:AGENTOS_RUNTIME_ADDRESS = "127.0.0.1:50051"
python -m uvicorn api.main:app --app-dir AgentOS --reload --host 127.0.0.1 --port 8000
```

Expected output:

```text
Uvicorn running on http://127.0.0.1:8000
```

Swagger UI: http://127.0.0.1:8000/docs

Endpoints:

```text
GET  /health
POST /tasks
GET  /tasks/{id}
GET  /tasks/{id}/events
POST /tasks/{id}/cancel
```

`GET /health` checks PostgreSQL only, not the gRPC runtime.

Durable task types include `sleep`, `text_transform`, and `word_count`. See
[`docs/file-workflows.md`](docs/file-workflows.md) for a real dependency
workflow and [`scripts/run-durable-benchmark.ps1`](scripts/run-durable-benchmark.ps1)
for the reproducible end-to-end benchmark.

### Terminal 4 — Postgres monitor (`psql`)

Open this **before** you hit the API so you can watch rows appear.

```powershell
$env:PGPASSWORD = "agentos"
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" -w `
  -h localhost -U agentos -d agentos
```

Inside `psql`, start a 1-second refresh on the tables the runtime writes:

```sql
SELECT id, type, state, attempts, updated_at
FROM tasks
ORDER BY updated_at DESC
LIMIT 10;
\watch 1
```

Open a second `psql` window if you also want live events:

```sql
SELECT sequence, event_type, task_id, worker_id, created_at
FROM runtime_events
ORDER BY sequence DESC
LIMIT 15;
\watch 1
```

Useful one-shot queries (run `\watch` stop with Ctrl+C first):

```sql
SELECT id, type, payload_json, state, attempts FROM tasks ORDER BY updated_at DESC;
SELECT task_id, dependency_id FROM task_dependencies ORDER BY task_id;
SELECT task_id, worker_id, attempt, status, started_at, finished_at
FROM executions ORDER BY id DESC LIMIT 20;
SELECT id, status, last_heartbeat FROM workers ORDER BY id;
```

Live running leases:

```sql
SELECT id, task_id, worker_id, attempt, status, lease_expires_at
FROM executions WHERE status = 'RUNNING';
\watch 1
```

Completed tasks are retained. Cleanup is not automatic.

### Confirm the three listeners

From a fifth window, or after the services are up:

```powershell
curl.exe -s http://127.0.0.1:8000/health
Test-NetConnection 127.0.0.1 -Port 50051 | Select-Object TcpTestSucceeded
```

Health should print `{"status":"ok"}`. `TcpTestSucceeded` should be `True`.

---

## Hit the API with curl and watch Postgres

Task IDs must be unique in the `tasks` table. Change `demo-a` / `demo-b` if
you already submitted them.

Use `curl.exe` in PowerShell so Windows does not alias `curl` to
`Invoke-WebRequest`.

### 1. Health (Postgres only)

```powershell
curl.exe -s http://127.0.0.1:8000/health
```

```text
{"status":"ok"}
```

### 2. Submit a two-task sleep workflow

`demo-b` waits for `demo-a`. Each sleep is 2 seconds so `\watch 1` can show
`Waiting` → `Running` → `Completed`.

```powershell
curl.exe -s -X POST http://127.0.0.1:8000/tasks `
  -H "Content-Type: application/json" `
  -d "{\"tasks\":[{\"id\":\"demo-a\",\"type\":\"sleep\",\"payload\":{\"seconds\":2}},{\"id\":\"demo-b\",\"type\":\"sleep\",\"payload\":{\"seconds\":2},\"dependencies\":[\"demo-a\"]}]}"
```

Expected HTTP response:

```text
{"task_ids":["demo-a","demo-b"]}
```

In the `psql` `\watch` window you should see, in order:

1. `tasks`: `demo-a` and `demo-b` inserted (`Waiting`)
2. `task_dependencies`: `demo-b` → `demo-a`
3. `runtime_events`: `TaskSubmitted` for both
4. `executions` + `workers`: `demo-a` `RUNNING` on a `worker-*`
5. `runtime_events`: `TaskStarted` then `TaskCompleted` for `demo-a`
6. the same cycle for `demo-b` after `demo-a` completes
7. `tasks.state` ends as `Completed` for both

### 3. Read task state and events from the API

```powershell
curl.exe -s http://127.0.0.1:8000/tasks/demo-a
curl.exe -s http://127.0.0.1:8000/tasks/demo-b
curl.exe -s http://127.0.0.1:8000/tasks/demo-a/events
curl.exe -s http://127.0.0.1:8000/tasks/demo-b/events
```

Match those JSON `state` / `event_type` values against the `tasks` and
`runtime_events` rows in Postgres. They are the same store.

### 4. Cancel example

Submit a longer sleep, then cancel it before it finishes. Use a new ID.

```powershell
curl.exe -s -X POST http://127.0.0.1:8000/tasks `
  -H "Content-Type: application/json" `
  -d "{\"tasks\":[{\"id\":\"demo-cancel\",\"type\":\"sleep\",\"payload\":{\"seconds\":30}}]}"

curl.exe -s -X POST http://127.0.0.1:8000/tasks/demo-cancel/cancel
```

Watch `tasks.state` and `runtime_events` for the cancel. Waiting/Ready
tasks become `Blocked` immediately. A `Running` task stays `Running` until
the worker observes the cancellation flag, then becomes `Blocked`.

The API validates duplicate IDs, missing dependencies, cycles, supported types
(`sleep`, `cpp_callback`), and payloads before calling the runtime.

Durable sleep payload:

```text
sleep: {"seconds": 2}
```

---

## Optional: run the demo binary instead of the API

This does not listen on gRPC. It runs a hardcoded graph and exits.

```powershell
cd E:\OS
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
.\AgentOS\build-postgres\Release\agentos.exe
```

Expected output includes:

```text
PostgreSQL event storage enabled
All tasks completed
```

This binary calls the same recovery helper as `agentos_server`
(`recover_stale_executions` + `load_recoverable_tasks`). After a gRPC process
restart, unfinished registered handlers (currently `sleep`) are restored into
the in-memory graph and resumed. `cpp_callback` lambdas cannot be recovered.

---

## Production behavior and tradeoffs

This is a **single-process** runtime: gRPC, the scheduler, and the worker pool
share one address space. PostgreSQL is the durable log, not a second executor.

The gRPC runtime uses four persistent PostgreSQL connections, matching its four
scheduler workers. Each transaction exclusively borrows a pooled connection;
durable state transitions still commit synchronously. See
[`docs/persistence-design.md`](docs/persistence-design.md) for the design,
correctness boundaries, and before/after measurements.

**Why gRPC instead of in-process calls from FastAPI**

The Python API and the C++ scheduler have different lifetimes. gRPC is a
process boundary so you can restart Uvicorn without dropping in-flight workers,
and so the runtime can be talked to by any language. The cost is an extra hop
and a protobuf contract. An in-process embedding would be faster and simpler
for a single binary, but then the HTTP layer could not crash independently and
you could not recover API-submitted work in `agentos_server` without linking
Python into C++.

**Why leases**

A worker that dies mid-task would otherwise hold `Running` forever. Each
assignment has a lease. Heartbeats renew it while the worker is alive,
including during user work. If heartbeats stop, the monitor **reclaims** the
task: the execution is marked `ABANDONED`, the task goes back to `Waiting`,
and a new worker may take it. A `run_token` fences the old attempt so a late
`complete()` from the dead assignment is ignored. That is steal-with-fencing,
not a consensus group.

False expiry is possible if you disable renewal (`set_renew_lease_while_running(false)`);
that path exists for tests. In normal operation, renewal is on.

**What happens on crash**

1. Process dies. Postgres still has `tasks`, `executions`, and `runtime_events`.
2. `agentos_server` starts, calls `recover_stale_executions()`: every `RUNNING`
   execution is `ABANDONED` (those workers are gone), matching tasks go back to
   `Waiting`, then `load_recoverable_tasks()` fills the in-memory graph, then
   Heartbeats also extend `lease_expires_at` while the process is alive so live
rows stay queryable. They are not inserted into `runtime_events` (too chatty);
worker liveness is `workers.last_heartbeat`.
3. Registered types (`sleep`) run again. `cpp_callback` is skipped.

There is no multi-node membership, so two `agentos_server` processes on one
database will both try to run the same ready tasks. Run one runtime.

**Why not Kafka**

Kafka is a durable *log between services*. This runtime needs a queryable
task graph (state, deps, attempts, leases) and transactions around submit.
Postgres already does that. Kafka would help if many independent consumers
needed to replay the event stream; it would not replace the scheduler or the
dependency table. Adding Kafka before there is more than one runtime process
is extra moving parts without a consumer.

**Tests**

See **6. Run all tests**. The one CTest tree is `AgentOS/build-grpc`. In-process
scheduler tests are `tests/scheduler_tests.cpp`. Postgres recovery and gRPC
(`grpc_submit`, `grpc_cancel`, `grpc_recovery`, `grpc_lease_steal`) are
registered in that same tree when CMake is configured with Postgres and gRPC.

```powershell
powershell -File AgentOS\scripts\run-all-tests.ps1
```

or:

```powershell
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
ctest --test-dir AgentOS/build-grpc -C Release --output-on-failure
```

**Issues this work actually hit** (useful in an interview; these are design
bugs, not compile errors):

1. **The resume path was on the wrong binary.** Recovery lived in the demo
   `agentos.exe`, not `agentos_server`. API-submitted work survived in Postgres
   and still vanished from the process that serves gRPC. Wiring
   `restore_runtime_graph()` into the server is what made crash restart real.

2. **Cancel lied.** `CancelTask` used to always return `Blocked`. Waiting work
   is blocked immediately; running work stays `Running` until the worker sees
   the flag. Returning the graph state is cooperative cancel, not preemption.

3. **Lease expiry was an event, not a steal.** Emitting `WorkerLeaseExpired`
   without `reclaim_running()` + `run_token` leaves `Running` stuck. Steal has
   to requeue and fence: a late `complete()` from the old attempt must no-op.

4. **Abandon-by-task-id is a lost update.** If a late lease event marks every
   `RUNNING` row for that task `ABANDONED`, a replacement attempt that already
   inserted a new row gets killed in Postgres. Abandon is fenced by
   `(task_id, worker_id)` on `TaskReclaimed`. If the start row is not committed
   yet, reclaim still inserts an `ABANDONED` execution so the table matches the
   event log.

5. **Turning off lease renewal live-locks steal tests.** Renewal is how a live
   worker proves it is alive. If you disable it to force expiry, the stolen
   attempt also looks dead and is reclaimed in a loop. The honest test: first
   attempt sleeps past the lease; the stolen attempt finishes immediately.
   The lease clock starts when the assignment is stamped, not at the last idle
   heartbeat. Windows `Sleep` is ~15.6ms, so a 15ms lease expires on the first
   monitor tick and `shutdown()` joins a worker that never sees `stopping_`.

6. **Crash recovery is process-scoped, not lease-scoped.** On
   `agentos_server` start, every `RUNNING` execution belonged to dead threads,
   even if `lease_expires_at` is still in the future. Only abandoning expired
   leases would skip work that crashed inside the lease window.

7. **Schema catches stale writers.** `executions.worker_id` references
   `workers`. Seeding a crash row without inserting the worker fails the FK.
   Heartbeats upsert workers first so start/lease rows can land.

8. **Idle lease expiry hid later work.** The monitor recorded a worker as
   expired while it was blocked in a slow heartbeat write, then ignored that
   worker forever. If it took a task afterwards, the assignment was never
   stolen. Running work must stay reclaimable even after an idle expiry.

9. **Shared Postgres is not a test fixture.** `recover_stale_executions()`
   is global. Parallel gRPC tests, or a test while a real server is running,
   will abandon each other's rows. Tests are serialized with a CTest resource
   lock; run one runtime on a database.

10. **gRPC vs in-process is a lifetime choice.** In-process FastAPI→scheduler
   is faster, but then HTTP crashes take workers with them. gRPC lets Uvicorn
   restart while C++ keeps the thread pool. Kafka would be a log between
   services; it does not give you a transactional DAG.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `WinError 10061` / connection refused on `:50051` | `agentos_server` is not running | Start step 6; confirm `listening on 0.0.0.0:50051` |
| `runtime unavailable` from FastAPI | gRPC client cannot reach the runtime | Set `AGENTOS_RUNTIME_ADDRESS=127.0.0.1:50051` and keep the server process alive |
| CMake cannot find gRPC / Protobuf | `grpc:x64-windows` not installed | Finish step 3; `grpcpp.h` must exist |
| CMake cannot find libpqxx | `libpqxx:x64-windows` not installed | Run the libpqxx install in step 3 |
| C++ binary starts then fails on `libpq` | PostgreSQL/vcpkg DLLs not on `PATH` | Prepend the `PATH` from step 6 |
| `GET /health` returns 503 | Postgres down or wrong URL | Start the service; check `AGENTOS_DATABASE_URL` |
| Duplicate task ID 409 | IDs already in `tasks` | Use new IDs |
| Python `ModuleNotFoundError: grpc` | venv missing packages | Activate `.venv` and reinstall `api/requirements.txt` |
| Missing `api/generated` | protobuf stubs not generated | Re-run the `grpc_tools.protoc` command in step 4 |
| `curl` returns unexpected HTML/objects | PowerShell aliased `curl` to `Invoke-WebRequest` | Call `curl.exe` explicitly |

---

## GitHub repository setup

From the AgentOS git root:

```powershell
cd E:\OS\AgentOS
git init
git add .
git status
git commit -m "Initial AgentOS runtime and API"
git branch -M main
```

Create an empty GitHub repository, then:

```powershell
git remote add origin https://github.com/YOUR_USERNAME/agentos.git
git push -u origin main
```

SSH:

```powershell
git remote add origin git@github.com:YOUR_USERNAME/agentos.git
git push -u origin main
```

Do not commit PostgreSQL passwords, `.env` files, build directories, venvs, or
generated protobuf bindings (`api/generated/`).
