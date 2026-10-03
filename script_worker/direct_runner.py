"""Run one generated document script as a child of a C++ scheduler worker."""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

MAX_BYTES = 1024 * 1024
MAX_LOG_BYTES = 16 * 1024
OUTPUT_PATTERN = re.compile(r"^output/[a-zA-Z0-9_-]+\.txt$")


def atomic_write(path: Path, data: bytes):
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(data)
    temp.replace(path)


def execute(request_file: Path, job: Path):
    report = {"ok": False, "exit_code": None, "timed_out": False,
              "stdout": "", "stderr": "", "error": ""}
    result = b""
    try:
        payload = json.loads(request_file.read_text(encoding="utf-8"))
        if not isinstance(payload, dict) or set(payload) - {"input_path", "output_path", "code"}:
            raise ValueError("invalid task request fields")
        input_path = payload.get("input_path")
        output_path = payload.get("output_path")
        code = payload.get("code")
        if not isinstance(input_path, str) or not isinstance(output_path, str) or not isinstance(code, str):
            raise ValueError("input_path, output_path, and code are required")
        normalized = input_path.replace("\\", "/")
        if normalized.startswith("/") or ":" in normalized or ".." in normalized.split("/") or normalized.startswith(".script-jobs/"):
            raise ValueError("input path escapes AGENTOS_WORK_DIR")
        if not OUTPUT_PATTERN.fullmatch(output_path):
            raise ValueError("invalid task output path")
        if len(code.encode("utf-8")) > 32_000:
            raise ValueError("script exceeds 32 KiB")
        root = Path(os.environ["AGENTOS_WORK_DIR"]).resolve()
        source = (root / normalized).resolve()
        if not source.is_relative_to(root) or not source.is_file():
            raise ValueError("input must be a regular file within AGENTOS_WORK_DIR")
        with source.open("rb") as stream:
            document = stream.read(MAX_BYTES + 1)
        if len(document) > MAX_BYTES:
            raise ValueError("input exceeds 1 MiB")
        with tempfile.TemporaryDirectory(prefix="run-", dir=job) as temporary:
            scratch = Path(temporary)
            (scratch / "input").mkdir()
            (scratch / "output").mkdir()
            (scratch / "input" / "document").write_bytes(document)
            script = scratch / "main.py"
            script.write_text(code, encoding="utf-8")
            stdout_path = job / "stdout.log"
            stderr_path = job / "stderr.log"
            environment = {"PATH": os.environ.get("PATH", os.defpath), "PYTHONIOENCODING": "utf-8",
                           "PYTHONUNBUFFERED": "1", "LANG": "C.UTF-8"}
            if os.name == "nt":
                for key in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP", "PATHEXT"):
                    if key in os.environ:
                        environment[key] = os.environ[key]
            with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                options = {"start_new_session": os.name != "nt"}
                if os.name == "nt":
                    options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
                process = subprocess.Popen([sys.executable, "-I", str(script)], cwd=scratch,
                    stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, env=environment, **options)
                try:
                    report["exit_code"] = process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    report["timed_out"] = True
                    if os.name == "nt":
                        process.kill()
                    else:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.wait()
                report["ok"] = report["exit_code"] == 0 and not report["timed_out"]
            for key, path in (("stdout", stdout_path), ("stderr", stderr_path)):
                with path.open("rb") as stream:
                    data = stream.read(MAX_LOG_BYTES + 1)
                report[key] = data[:MAX_LOG_BYTES].decode("utf-8", errors="replace")
                report[key + "_truncated"] = len(data) > MAX_LOG_BYTES
            if report["ok"]:
                artifact = scratch / "output" / "result.txt"
                if not artifact.is_file() or artifact.is_symlink():
                    report["ok"] = False
                    report["error"] = "script must write a regular output/result.txt file"
                else:
                    with artifact.open("rb") as stream:
                        result = stream.read(MAX_BYTES + 1)
                    if len(result) > MAX_BYTES:
                        report["ok"] = False
                        report["error"] = "result exceeds 1 MiB"
                    else:
                        result.decode("utf-8")
    except Exception as error:
        report["ok"] = False
        report["error"] = str(error)[:2000]
    atomic_write(job / "report.json", json.dumps(report).encode("utf-8"))
    if report["ok"]:
        atomic_write(job / "result.txt", result)
        atomic_write(job / "status", b"ok")
        return 0
    atomic_write(job / "status", b"error")
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: direct_runner.py request.json job-directory")
    raise SystemExit(execute(Path(sys.argv[1]), Path(sys.argv[2])))
