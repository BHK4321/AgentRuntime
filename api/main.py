import json
import os
from collections import defaultdict, deque
from contextlib import contextmanager
from typing import Any

import psycopg
import grpc
from fastapi import FastAPI, HTTPException, Query, status
from pydantic import BaseModel, Field, field_validator

from .grpc_runtime_client import RuntimeClient
from .documents import create_document_router
from script_worker.contracts import ScriptPayload

DATABASE_URL = os.getenv(
    "AGENTOS_DATABASE_URL",
    "postgresql://agentos:agentos@localhost:5432/agentos",
)
RUNTIME_ADDRESS = os.getenv("AGENTOS_RUNTIME_ADDRESS", "127.0.0.1:50051")
SUPPORTED_TYPES = {"sleep", "text_transform", "word_count", "cpp_callback", "python_script"}
TERMINAL_STATES = {"Completed", "Failed", "Blocked"}

app = FastAPI(title="AgentOS API", version="0.1.0")


class TaskRequest(BaseModel):
    id: str = Field(min_length=1, max_length=200)
    type: str = Field(min_length=1, max_length=100)
    payload: dict[str, Any] = Field(default_factory=dict)
    dependencies: list[str] = Field(default_factory=list)
    max_attempts: int = Field(default=1, ge=1)
    retry_delay_ms: int = Field(default=0, ge=0)
    deadline_ms: int = Field(default=0, ge=0)
    idempotent: bool = False
    idempotency_key: str | None = None

    @field_validator("dependencies")
    @classmethod
    def unique_dependencies(cls, value: list[str]) -> list[str]:
        if len(value) != len(set(value)):
            raise ValueError("dependencies must be unique")
        return value


class WorkflowRequest(BaseModel):
    tasks: list[TaskRequest] = Field(min_length=1)


class CancelResponse(BaseModel):
    id: str
    state: str


@contextmanager
def database():
    connection = psycopg.connect(DATABASE_URL)
    try:
        yield connection
        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()


app.include_router(create_document_router(database))


def validate_workflow(tasks: list[TaskRequest], existing_ids: set[str]) -> None:
    ids = [task.id for task in tasks]
    incoming = set(ids)
    if len(ids) != len(incoming):
        raise HTTPException(status_code=409, detail="task IDs must be unique")
    duplicate_existing = incoming & existing_ids
    if duplicate_existing:
        raise HTTPException(
            status_code=409,
            detail=f"task IDs already exist: {sorted(duplicate_existing)}",
        )

    for task in tasks:
        if task.type == "python_script":
            try:
                ScriptPayload.model_validate(task.payload)
            except ValueError as error:
                raise HTTPException(422, "invalid python_script payload: " + str(error)) from error
            if task.max_attempts != 1:
                raise HTTPException(422, "script tasks use one execution attempt; submit corrected code as a new task")
        if task.type not in SUPPORTED_TYPES:
            raise HTTPException(
                status_code=422,
                detail=f"unsupported task type: {task.type}",
            )
        if task.id in task.dependencies:
            raise HTTPException(status_code=422, detail=f"task depends on itself: {task.id}")
        missing = set(task.dependencies) - incoming - existing_ids
        if missing:
            raise HTTPException(
                status_code=422,
                detail=f"task {task.id} has missing dependencies: {sorted(missing)}",
            )
        if task.idempotent and not task.idempotency_key:
            raise HTTPException(
                status_code=422,
                detail=f"idempotent task requires idempotency_key: {task.id}",
            )
        if task.type == "sleep":
            seconds = task.payload.get("seconds")
            if not isinstance(seconds, int) or isinstance(seconds, bool) or seconds < 0:
                raise HTTPException(
                    status_code=422,
                    detail=f"sleep task {task.id} requires non-negative integer payload.seconds",
                )
        if task.type in {"text_transform", "word_count"}:
            input_path = task.payload.get("input_path")
            output_path = task.payload.get("output_path")
            if not isinstance(input_path, str) or not input_path:
                raise HTTPException(
                    status_code=422,
                    detail=f"{task.type} task {task.id} requires payload.input_path",
                )
            if not isinstance(output_path, str) or not output_path:
                raise HTTPException(
                    status_code=422,
                    detail=f"{task.type} task {task.id} requires payload.output_path",
                )
            for path in (input_path, output_path):
                normalized = path.replace("\\", "/")
                if normalized.startswith("/") or ":" in normalized or ".." in normalized.split("/"):
                    raise HTTPException(
                        status_code=422,
                        detail=f"{task.type} task {task.id} paths must stay inside AGENTOS_WORK_DIR",
                    )
        if task.type == "text_transform" and task.payload.get("operation") not in {
            "uppercase",
            "lowercase",
        }:
            raise HTTPException(
                status_code=422,
                detail=f"text_transform task {task.id} requires uppercase or lowercase operation",
            )

    edges = {task.id: [dependency for dependency in task.dependencies if dependency in incoming] for task in tasks}
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(task_id: str) -> None:
        if task_id in visiting:
            raise HTTPException(status_code=422, detail="workflow contains a dependency cycle")
        if task_id in visited:
            return
        visiting.add(task_id)
        for dependency in edges[task_id]:
            visit(dependency)
        visiting.remove(task_id)
        visited.add(task_id)

    for task_id in incoming:
        visit(task_id)


def task_row(row: tuple[Any, ...]) -> dict[str, Any]:
    return {
        "id": row[0],
        "type": row[1],
        "payload": json.loads(row[2]),
        "state": row[3],
        "max_attempts": row[4],
        "attempts": row[5],
        "retry_delay_ms": row[6],
        "deadline_ms": row[7],
        "idempotent": row[8],
        "idempotency_key": row[9],
        "created_at": row[10],
        "updated_at": row[11],
    }


@app.get("/health")
def health() -> dict[str, str]:
    try:
        with database() as connection:
            connection.execute("SELECT 1")
        return {"status": "ok"}
    except psycopg.Error as error:
        raise HTTPException(status_code=503, detail="database unavailable") from error


@app.post("/tasks", status_code=status.HTTP_201_CREATED)
def submit_workflow(workflow: WorkflowRequest) -> dict[str, Any]:
    with database() as connection:
        existing = set(
            row[0]
            for row in connection.execute(
                "SELECT id FROM tasks WHERE id = ANY(%s)",
                ([task.id for task in workflow.tasks],),
            )
        )
        validate_workflow(workflow.tasks, existing)
    try:
        task_ids = RuntimeClient(RUNTIME_ADDRESS).submit_workflow(
            [
                {
                    "id": task.id,
                    "type": task.type,
                    "payload_json": json.dumps(task.payload, separators=(",", ":")),
                    "dependencies": task.dependencies,
                    "max_attempts": task.max_attempts,
                    "retry_delay_ms": task.retry_delay_ms,
                    "deadline_ms": task.deadline_ms,
                    "idempotent": task.idempotent,
                    "idempotency_key": task.idempotency_key,
                }
                for task in workflow.tasks
            ]
        )
    except grpc.RpcError as error:
        raise HTTPException(status_code=503, detail=f"runtime unavailable: {error.code().name}") from error
    return {"task_ids": task_ids}


@app.get("/tasks")
def list_tasks(limit: int = Query(50, ge=1, le=100), offset: int = Query(0, ge=0)) -> dict[str, Any]:
    with database() as connection:
        total = connection.execute("SELECT COUNT(*) FROM tasks").fetchone()[0]
        rows = connection.execute(
            "SELECT id, type, state, attempts, max_attempts, created_at, updated_at "
            "FROM tasks ORDER BY created_at DESC, id DESC LIMIT %s OFFSET %s",
            (limit, offset),
        ).fetchall()
        ids = [row[0] for row in rows]
        dependencies = defaultdict(list)
        if ids:
            for task_id, dependency_id in connection.execute(
                "SELECT task_id, dependency_id FROM task_dependencies "
                "WHERE task_id = ANY(%s) ORDER BY task_id, dependency_id",
                (ids,),
            ):
                dependencies[task_id].append(dependency_id)
    return {
        "total": total,
        "limit": limit,
        "offset": offset,
        "tasks": [
            {"id": row[0], "type": row[1], "state": row[2], "attempts": row[3],
             "max_attempts": row[4], "created_at": row[5], "updated_at": row[6],
             "dependencies": dependencies[row[0]]}
            for row in rows
        ],
    }


@app.get("/tasks/{task_id}")
def get_task(task_id: str) -> dict[str, Any]:
    with database() as connection:
        row = connection.execute(
            "SELECT id, type, payload_json, state, max_attempts, attempts, retry_delay_ms, "
            "deadline_ms, idempotent, idempotency_key, created_at, updated_at "
            "FROM tasks WHERE id = %s",
            (task_id,),
        ).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="task not found")
        dependencies = [
            dependency[0]
            for dependency in connection.execute(
                "SELECT dependency_id FROM task_dependencies WHERE task_id = %s ORDER BY dependency_id",
                (task_id,),
            )
        ]
    result = task_row(row)
    result["dependencies"] = dependencies
    return result


@app.get("/tasks/{task_id}/events")
def get_task_events(task_id: str) -> dict[str, Any]:
    with database() as connection:
        exists = connection.execute("SELECT 1 FROM tasks WHERE id = %s", (task_id,)).fetchone()
        if exists is None:
            raise HTTPException(status_code=404, detail="task not found")
        events = [
            {
                "sequence": row[0],
                "event_type": row[1],
                "task_id": row[2],
                "worker_id": row[3],
                "idempotency_key": row[4],
                "created_at": row[5],
            }
            for row in connection.execute(
                "SELECT sequence, event_type, task_id, worker_id, idempotency_key, created_at "
                "FROM runtime_events WHERE task_id = %s ORDER BY sequence",
                (task_id,),
            )
        ]
    return {"task_id": task_id, "events": events}


@app.post("/tasks/{task_id}/cancel", response_model=CancelResponse)
def cancel_task(task_id: str) -> CancelResponse:
    with database() as connection:
        row = connection.execute("SELECT state FROM tasks WHERE id = %s FOR UPDATE", (task_id,)).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="task not found")
        if row[0] in TERMINAL_STATES:
            return CancelResponse(id=task_id, state=row[0])
    try:
        result = RuntimeClient(RUNTIME_ADDRESS).cancel_task(task_id)
    except grpc.RpcError as error:
        raise HTTPException(status_code=503, detail=f"runtime unavailable: {error.code().name}") from error
    return CancelResponse(**result)
