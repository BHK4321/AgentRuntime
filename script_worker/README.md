# Python document tasks

The C++ runtime worker launches Python directly for generated document scripts.
No Docker installation or separate script worker is needed. Install Python 3 on
the machine running the runtime. On Render, include Python in the runtime image.

The script reads its selected document from `input/document` in a temporary
working folder and writes UTF-8 output to `output/result.txt`. The runtime copies
the selected document into that folder and publishes the result under
`AGENTOS_WORK_DIR`. Scripts get 30 seconds and the result is limited to 1 MiB.
The C++ runtime stops the Python process group on cancellation or after 45
seconds. `AGENTOS_PYTHON_EXECUTABLE` can override the default (`python` on
Windows and `python3` on Linux). `AGENTOS_SCRIPT_RUNNER` can point to
`script_worker/direct_runner.py` if the service's working directory is not the
repository root.

## Start and deploy

Stop the C++ runtime with Ctrl+C and rebuild:

```powershell
cd E:\OS\AgentOS
cmake --build build-grpc --config Release --target agentos_server
```

Restart the C++ runtime as usual. No separate worker terminal is needed. Restart
FastAPI and the chat service so they load the script task and tool definitions.
The API, C++ runtime, and uploaded documents must share the same work directory.

For Render, install `python3` in the runtime Dockerfile and set:

```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
```

The C++ binary gets its default `AGENTOS_SCRIPT_RUNNER` path from CMake. Set that
variable explicitly if the source tree is installed at a different path in the
runtime image. Set `AGENTOS_PYTHON_EXECUTABLE=python3` if `python3` is not on
the service's `PATH`.

## Try it

Upload a CSV and ask:

> Inspect this CSV, group sales by region, and write the totals as CSV.

Or upload a text file and ask:

> Remove duplicate lines, preserving their original order, and return the cleaned document.

The model previews the file, writes standard-library Python, submits it as a
`python_script` task, then checks the result. CSV, JSON, regular expressions,
and text operations work. PDF/DOCX extraction and third-party Python packages
are not included.

## Execution permissions

The script subprocess runs under the same operating-system account as the C++
runtime. It has that account's filesystem and network permissions; Python
subprocess execution is not a security sandbox. For a local, single-user test
this keeps setup simple. Do not deploy untrusted, multi-user script execution
with sensitive files or credentials available to the runtime account. Script
source is stored in the task payload in PostgreSQL.
