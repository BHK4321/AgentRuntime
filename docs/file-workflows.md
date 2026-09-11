# Durable file workflows

AgentOS includes two file-processing task types that can be recovered after a
runtime restart:

- `text_transform` reads a text file and writes an uppercase or lowercase copy.
- `word_count` reads a file and writes JSON with byte, line, and word counts.

Both handlers resolve relative paths beneath `AGENTOS_WORK_DIR`. Absolute paths,
drive-qualified paths, `..` traversal, and symlink paths that resolve outside
the work directory are rejected. Output is written to a temporary file and
renamed, so retrying a task produces the same final artifact.

## Run a workflow

Set the work directory before starting `agentos_server`:

```powershell
$env:AGENTOS_WORK_DIR = "E:\OS\AgentOS\work"
New-Item -ItemType Directory -Force "$env:AGENTOS_WORK_DIR\input" | Out-Null
Set-Content "$env:AGENTOS_WORK_DIR\input\sample.txt" `
  "Hello durable world`nThis workflow survived a process boundary."
```

Submit this body to `POST /tasks`:

```json
{
  "tasks": [
    {
      "id": "file-transform-1",
      "type": "text_transform",
      "payload": {
        "input_path": "input/sample.txt",
        "output_path": "output/normalized.txt",
        "operation": "uppercase"
      },
      "dependencies": []
    },
    {
      "id": "file-count-1",
      "type": "word_count",
      "payload": {
        "input_path": "output/normalized.txt",
        "output_path": "output/stats.json"
      },
      "dependencies": ["file-transform-1"]
    }
  ]
}
```

The dependency ensures `output/normalized.txt` exists before `word_count`
starts. Results appear beneath `AGENTOS_WORK_DIR/output`.

## Reproduce the durable benchmark

With PostgreSQL, `agentos_server`, and FastAPI running, execute from the AgentOS
repository root:

```powershell
powershell -File scripts\run-durable-benchmark.ps1
```

The defaults run five trials of 100 durable zero-second tasks. The script prints
each trial, calculates median throughput and median-trial p95 ready-to-start
latency, and writes `benchmarks/results/durable_benchmark_latest.csv`.

The benchmark measures the complete HTTP-to-PostgreSQL path. It does not replace
the scheduler-only benchmark in `benchmarks/scheduler_benchmark.cpp`.
