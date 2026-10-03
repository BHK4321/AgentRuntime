"""Document uploads and task artifacts on the API/runtime shared filesystem."""
import base64
import binascii
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
from uuid import uuid4

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from .document_text import DocumentFormatError, extract_document

MAX_DOCUMENT_BYTES = 10 * 1024 * 1024


class DocumentUpload(BaseModel):
    filename: str = Field(min_length=1, max_length=200)
    content: str | None = Field(default=None, max_length=MAX_DOCUMENT_BYTES)
    content_base64: str | None = Field(default=None, max_length=14 * 1024 * 1024)


def work_root() -> Path:
    return Path(os.getenv("AGENTOS_WORK_DIR", str(Path.cwd() / "AgentOS" / "work"))).resolve()


def confined_path(relative: str) -> Path:
    normalized = relative.replace("\\", "/")
    if not normalized or normalized.startswith("/") or ":" in normalized or ".." in normalized.split("/"):
        raise HTTPException(422, "file path must stay inside AGENTOS_WORK_DIR")
    root = work_root()
    path = (root / normalized).resolve()
    if not path.is_relative_to(root):
        raise HTTPException(422, "file path escapes AGENTOS_WORK_DIR")
    return path


def create_document_router(database) -> APIRouter:
    router = APIRouter()

    @router.post("/documents", status_code=201)
    def upload_document(document: DocumentUpload):
        if (document.content is None) == (document.content_base64 is None):
            raise HTTPException(422, "provide exactly one of content or content_base64")
        try:
            content = (document.content.encode("utf-8") if document.content is not None
                       else base64.b64decode(document.content_base64, validate=True))
        except (UnicodeEncodeError, binascii.Error) as error:
            raise HTTPException(422, "file content is not valid text or base64") from error
        if len(content) > MAX_DOCUMENT_BYTES:
            raise HTTPException(413, "documents are limited to 10 MiB")
        try:
            extracted = extract_document(document.filename, content)
        except DocumentFormatError as error:
            raise HTTPException(422, str(error)) from error
        document_id = uuid4().hex
        extension = PurePosixPath(document.filename).suffix.lower()
        relative = f"uploads/{document_id}.source{extension}"
        text_relative = f"uploads/{document_id}.txt"
        path, text_path = confined_path(relative), confined_path(text_relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        text_path.write_text(extracted, encoding="utf-8")
        try:
            with database() as connection:
                connection.execute(
                    "INSERT INTO documents (id, filename, storage_path, size_bytes) VALUES (%s, %s, %s, %s)",
                    (document_id, document.filename, relative, len(content)),
                )
        except Exception:
            path.unlink(missing_ok=True)
            text_path.unlink(missing_ok=True)
            raise
        return {"id": document_id, "filename": document.filename, "storage_path": relative,
                "text_path": text_relative, "size_bytes": len(content)}

    @router.get("/tasks/{task_id}/result")
    def task_result(task_id: str):
        with database() as connection:
            row = connection.execute("SELECT type, payload_json, state FROM tasks WHERE id = %s", (task_id,)).fetchone()
        if row is None:
            raise HTTPException(404, "task not found")
        script = row[0] == "python_script"
        if row[2] != "Completed" and not (script and row[2] == "Failed"):
            raise HTTPException(409, "task has not completed")
        if row[0] not in {"text_transform", "word_count", "python_script"}:
            return {"task_id": task_id, "content": None}
        output_path = json.loads(row[1])["output_path"]
        path = confined_path(output_path)
        report = None
        if script:
            report_path = confined_path(output_path + ".report.json")
            try:
                with report_path.open("rb") as stream:
                    report = json.loads(stream.read(128000))
            except FileNotFoundError:
                report = {"ok": False, "error": "No execution report was written. Check the runtime logs and Python executable configuration."}
            if row[2] == "Failed":
                return {"task_id": task_id, "content": None, "report": report}
        try:
            with path.open("rb") as artifact:
                data = artifact.read(MAX_DOCUMENT_BYTES + 1)
        except FileNotFoundError as error:
            raise HTTPException(404, "artifact missing; check the API/runtime shared work directory") from error
        if len(data) > MAX_DOCUMENT_BYTES:
            raise HTTPException(413, "artifact exceeds 10 MiB result limit")
        return {"task_id": task_id, "content": data.decode("utf-8", errors="replace"), **({"report": report} if script else {})}

    @router.get("/documents/{document_id}/preview")
    def preview_document(document_id: str):
        with database() as connection:
            row = connection.execute("SELECT storage_path FROM documents WHERE id = %s", (document_id,)).fetchone()
        if row is None:
            raise HTTPException(404, "document not found")
        try:
            text_path = f"uploads/{document_id}.txt"
            with confined_path(text_path).open("rb") as stream:
                data = stream.read(12001)
        except FileNotFoundError as error:
            raise HTTPException(404, "document file missing") from error
        return {"document_id": document_id, "content": data[:12000].decode("utf-8", errors="replace"), "truncated": len(data) > 12000}

    return router
