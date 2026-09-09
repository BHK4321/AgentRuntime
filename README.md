# AgentOS

AgentOS is a local C++ execution runtime for dependency-aware, concurrent task
workflows. PostgreSQL stores task definitions, dependencies, execution attempts,
worker heartbeats, leases, and runtime events. A FastAPI service exposes local
workflow submission and monitoring endpoints.

## Architecture

```text
FastAPI API
    |
  | gRPC
    v
 C++ runtime / scheduler -----> PostgreSQL
                       |
                       v
                   worker pool
```

The C++ scheduler currently supports dependency graphs, FIFO ready-task scheduling,
concurrent workers, retries, deadlines, cancellation, idempotency metadata, heartbeats,
leases, durable task types, and startup recovery for registered task handlers.

## Requirements

- Windows PowerShell
- CMake 3.20 or newer
- Visual Studio C++ build tools
- PostgreSQL 17 running on `localhost:5432`
- vcpkg installed at `E:\vcpkg`
- Python 3.11 for the local API

## Repository Layout

```text
AgentOS/
├── CMakeLists.txt
├── README.md
├── database/
│   └── schema.sql
├── runtime/
│   ├── main.cpp
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
└── benchmarks/
```

## PostgreSQL Setup

Start the native PostgreSQL service:

```powershell
Start-Service postgresql-x64-17
```

Create the application role and database as the PostgreSQL administrator:

```powershell
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" `
  -h localhost -U postgres -d postgres -W
```

Run inside `psql`:

```sql
CREATE ROLE agentos LOGIN PASSWORD 'agentos';
CREATE DATABASE agentos OWNER agentos;
\c agentos
\i E:/OS/AgentOS/database/schema.sql
GRANT USAGE ON SCHEMA public TO agentos;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO agentos;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO agentos;
```

If the role or database already exists, use:

```sql
ALTER ROLE agentos WITH PASSWORD 'agentos';
```

The local connection string is:

```text
postgresql://agentos:agentos@localhost:5432/agentos
```

Verify the connection:

```powershell
$env:PGPASSWORD = "agentos"
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" -w `
  -h localhost -U agentos -d agentos `
  -c "SELECT current_user, current_database();"
```

## C++ Runtime

Install the PostgreSQL C++ client library once:

```powershell
E:\vcpkg\vcpkg.exe install libpqxx:x64-windows
```

Configure and build the PostgreSQL-enabled runtime:

```powershell
cd E:\OS
cmake -S AgentOS -B AgentOS/build-postgres `
  -DAGENTOS_ENABLE_POSTGRES=ON `
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=E:/vcpkg/installed/x64-windows
cmake --build AgentOS/build-postgres --config Release --target agentos
```

Run it:

```powershell
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
.\AgentOS\build-postgres\Release\agentos.exe
```

Expected output includes:

```text
PostgreSQL event storage enabled
All tasks completed
```

The demo uses the registered `sleep` handler and persists task type and payload. The
current built-in durable handler is:

```text
sleep: {"seconds": 1}
```

Legacy `cpp_callback` tasks run only while their C++ process is alive because lambdas
cannot be serialized. Registered task types can be loaded and resumed after restart.

## Inspect the Database

```powershell
$env:PGPASSWORD = "agentos"
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" -w `
  -h localhost -U agentos -d agentos
```

Useful queries:

```sql
SELECT id, type, payload_json, state, attempts FROM tasks ORDER BY id;
SELECT task_id, dependency_id FROM task_dependencies ORDER BY task_id;
SELECT task_id, worker_id, attempt, status, lease_expires_at FROM executions ORDER BY id;
SELECT id, status, last_heartbeat FROM workers ORDER BY id;
SELECT sequence, event_type, task_id, worker_id, created_at
FROM runtime_events ORDER BY sequence;
```

Live polling in `psql`:

```sql
SELECT id, task_id, worker_id, attempt, status, lease_expires_at
FROM executions WHERE status = 'RUNNING';
\watch 1
```

Completed tasks are retained. Cleanup is not automatic; the event history is intended
to support auditing and later replay/checkpoint features.

## Tests and Benchmarks

Build and run the test suite:

```powershell
cmake --build AgentOS/build --config Debug --target agentos_tests
ctest --test-dir AgentOS/build -C Debug --output-on-failure
```

Build and run the benchmark:

```powershell
cmake --build AgentOS/build --config Release --target agentos_benchmark
.\AgentOS\build\Release\agentos_benchmark.exe
```

## Local FastAPI API

Install the Python dependencies with the same interpreter you will use to run Uvicorn:

```powershell
C:\Users\Bhaskar\AppData\Local\Microsoft\WindowsApps\python3.11.exe `
  -m pip install -r AgentOS/api/requirements.txt
```

Generate the Python gRPC bindings from the shared contract:

```powershell
New-Item -ItemType Directory -Force AgentOS/api/generated | Out-Null
New-Item -ItemType File -Force AgentOS/api/generated/__init__.py | Out-Null
python -m grpc_tools.protoc `
  -I AgentOS/runtime/proto `
  --python_out=AgentOS/api/generated `
  --grpc_python_out=AgentOS/api/generated `
  AgentOS/runtime/proto/agentos.proto
```

Start the API after the C++ gRPC runtime service is running:

```powershell
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
python -m uvicorn api.main:app --app-dir AgentOS --reload
```

Open Swagger at http://127.0.0.1:8000/docs.

Available endpoints:

```text
GET  /health
POST /tasks
GET  /tasks/{id}
GET  /tasks/{id}/events
POST /tasks/{id}/cancel
```

Submit a workflow from PowerShell:

```powershell
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8000/tasks `
  -ContentType "application/json" -Body (@{
    tasks = @(
      @{ id = "api-a"; type = "sleep"; payload = @{ seconds = 1 } },
      @{ id = "api-b"; type = "sleep"; payload = @{ seconds = 1 }; dependencies = @("api-a") }
    )
  } | ConvertTo-Json -Depth 5)
```

The API validates duplicate IDs, missing dependencies, dependency cycles, supported
types, and task payloads before persisting a workflow transactionally.

## gRPC Status

The shared protobuf contract is at:

```text
AgentOS/runtime/proto/agentos.proto
```

The Python gRPC client and C++ service source are present, but the C++ gRPC server is
not considered ready until the vcpkg `grpc:x64-windows` installation completes and
`agentos_server` is built and tested. After installation:

```powershell
E:\vcpkg\vcpkg.exe install grpc:x64-windows
cmake -S AgentOS -B AgentOS/build-grpc `
  -DAGENTOS_ENABLE_POSTGRES=ON `
  -DAGENTOS_ENABLE_GRPC=ON `
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=E:/vcpkg/installed/x64-windows
cmake --build AgentOS/build-grpc --config Release --target agentos_server
```

Run the C++ gRPC service:

```powershell
$env:PATH = "E:\vcpkg\installed\x64-windows\bin;C:\Program Files\PostgreSQL\17\bin;$env:PATH"
$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
.\AgentOS\build-grpc\Release\agentos_server.exe
```

Then run FastAPI in another terminal with:

```powershell
$env:AGENTOS_RUNTIME_ADDRESS = "127.0.0.1:50051"
python -m uvicorn api.main:app --app-dir AgentOS --reload
```

## GitHub Repository Setup

From the workspace root:

```powershell
cd E:\OS
git init
git add .
git status
git commit -m "Initial AgentOS runtime and API"
git branch -M main
```

Create an empty repository on GitHub, then replace the placeholder below with its
actual URL:

```powershell
git remote add origin https://github.com/YOUR_USERNAME/agentos.git
git push -u origin main
```

For SSH instead:

```powershell
git remote add origin git@github.com:YOUR_USERNAME/agentos.git
git push -u origin main
```

Before pushing, verify that secrets and generated output are ignored:

```powershell
git status --short
git check-ignore -v AgentOS/build-postgres AgentOS/api/generated .env
```

Do not commit PostgreSQL passwords, `.env` files, build directories, or generated
protobuf bindings.
