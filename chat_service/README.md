# Local chat service

This is a separate local service on port 8080. It serves a browser chat UI and
calls Ollama Cloud from Python. No local Ollama install or model download is
required. The browser never receives the cloud API key. FastAPI and the C++
runtime remain the execution services; this service does not connect to PostgreSQL.

## Run

From the repository root, in PowerShell:

```powershell
python -m venv chat_service/.venv
./chat_service/.venv/Scripts/python.exe -m pip install -r chat_service/requirements.txt
# A local .env is provided. If using a fresh clone, create it:
# Copy-Item chat_service/.env.example chat_service/.env
```

Edit `chat_service/.env`:

```dotenv
AGENTOS_INTERFACE_URL=http://127.0.0.1:8000
AGENTOS_INTERFACE_TOKEN=
OLLAMA_BASE_URL=https://ollama.com
OLLAMA_API_KEY=your-ollama-cloud-api-key
OLLAMA_MODEL=gpt-oss:20b
```

Use your Render HTTPS API URL for `AGENTOS_INTERFACE_URL` when deployed. This
must be the HTTP interface, not port 50051. `AGENTOS_INTERFACE_TOKEN` optionally
supplies a bearer token to an authenticated gateway; the current AgentOS API
does not itself implement authentication. The local UI binds to loopback and
rejects cross-origin writes. It is a single-user development service, not a
public multi-user app.

Get a key from <https://ollama.com/settings/keys>. Select an available cloud
model with tool support using its API name (not necessarily its CLI `:cloud`
alias). See [cloud setup](https://docs.ollama.com/cloud) and
[tool calling](https://docs.ollama.com/capabilities/tool-calling).
Environment variables already set in the shell take precedence over `.env`.
Restart this service after configuration changes.

```powershell
./chat_service/.venv/Scripts/python.exe -m uvicorn chat_service.main:app --host 127.0.0.1 --port 8080
```

Open <http://127.0.0.1:8080>. The existing API, PostgreSQL and runtime must be
running for execution or uploads; ordinary chat only needs Ollama Cloud.

## Documents and the database

Apply the updated `database/schema.sql` to your existing database (it uses
`CREATE TABLE IF NOT EXISTS`), then restart the AgentOS API:

```powershell
& "C:/Program Files/PostgreSQL/17/bin/psql.exe" -h localhost -U agentos -d agentos -W -f database/schema.sql
```

For a hosted database use its connection details instead. Apply the schema as
the table owner/API role, or grant that role access to the new `documents` table.

Set **the same absolute `AGENTOS_WORK_DIR`** for the API and C++ runtime before
starting them. For example, in both local terminals:

```powershell
$env:AGENTOS_WORK_DIR = "E:/OS/AgentOS/work"
```

Upload flow:

1. Browser reads a supported file (up to 10 MiB) and base64-encodes it for upload.
2. Local chat forwards `{filename, content_base64}` to the API's `POST /documents`.
3. API preserves the original in `AGENTOS_WORK_DIR/uploads/` and stores extracted
   UTF-8 text beside it. It inserts
   `id`, `filename`, `storage_path`, `size_bytes`, `created_at` into `documents`.
4. The model references the document ID. The adapter translates it into the
   stored relative input path and generates unique output paths and task IDs.
5. The C++ handler reads the file, writes its output, and persists execution
   status in PostgreSQL as before. `GET /tasks/{id}/result` returns completed
   file output through the API (at most 1 MiB).

**PostgreSQL stores metadata, not document contents.** Task payloads reference
paths, not document foreign keys. There is no automatic document cleanup or
object-storage integration. A failed metadata insert removes the newly written
file; an abrupt process crash between file write and insert can leave an orphan.

On Render, the API and runtime need a shared filesystem (the proposed combined
container provides this). Separate hosts cannot use each other's local paths.
Ephemeral storage loses uploads and results on restart/redeploy even when
database metadata remains. Persistent file storage is needed to retain artifacts.

## What to try

- “Run a two-second test task and check its status.”
- “Submit one sleep DAG: start for 1 second, then left and right for 2 seconds in parallel, then join for 1 second after both. Submit all four nodes in one workflow.”
- Upload a supported document, then: “Count the words, lines, and bytes in my document.”
- “Convert my document to uppercase, then count words in the converted file.”
- “Check the result of that task.”

The adapter supports `sleep`, `text_transform`, `word_count`, and `python_script`.
Uploads support TXT, Markdown, CSV, JSON, XML, HTML, PDF, DOCX, XLSX, and PPTX.
Text is extracted for model previews and runtime tasks; output is plain text, so
the original layout and formatting are not rewritten. Scanned PDFs and images
need OCR, and legacy binary Office formats are not supported.
For custom CSV/text/JSON operations, the runtime worker launches a Python
subprocess. See [script setup](../script_worker/README.md). The model can inspect
a bounded document preview, generate standard-library Python, and correct a
failed script up to twice per turn.

The runtime activity panel polls task status and provides cancel/result buttons.
Pending tasks continue running if the browser closes. The model does not wait
indefinitely: each chat turn allows at most six model calls with ten tools each.
If still pending, ask for the result later. Failed/uncertain submissions keep
their generated IDs visible; requests are not automatically retried.

Chat history and document/task associations are in memory, scoped to a browser
cookie. Reloading/new conversation or restarting the service resets that chat;
it does not delete runtime tasks or uploaded files. The limit is 20 uploads,
100 tasks per chat, and 100 local sessions per process. Run one Uvicorn worker.
Uploaded bytes are not included in model prompts automatically. Filenames,
task metadata, requested document previews (up to 12,000 bytes), script logs,
and up to 12,000 characters of a retrieved task result are sent
to Ollama Cloud. Built-in text conversion follows the runtime's byte-oriented casing,
not full Unicode case conversion.

## Tests

```powershell
python -m unittest discover -s tests -p "test_chat*.py" -v
```

Tests use simulated Ollama/AgentOS HTTP responses and a temporary filesystem;
they do not consume cloud credits or modify the running task database.
