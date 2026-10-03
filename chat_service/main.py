"""Run with: python -m uvicorn chat_service.main:app --host 127.0.0.1 --port 8080"""
import base64
import binascii
import hmac
import os
from contextlib import asynccontextmanager
from pathlib import Path
from uuid import uuid4

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from starlette.middleware.trustedhost import TrustedHostMiddleware

from api.documents import DocumentUpload
from .agent import Agent, Session, friendly_error

ROOT = Path(__file__).resolve().parent
load_dotenv(ROOT / ".env")


@asynccontextmanager
async def lifespan(app):
    runtime_headers = {}
    if token := os.getenv("AGENTOS_INTERFACE_TOKEN"):
        runtime_headers["Authorization"] = f"Bearer {token}"
    key = os.getenv("OLLAMA_API_KEY", "")
    async with httpx.AsyncClient(base_url=os.getenv("AGENTOS_INTERFACE_URL", "http://127.0.0.1:8000").rstrip("/"),
                                headers=runtime_headers, timeout=30) as runtime, \
               httpx.AsyncClient(base_url=os.getenv("OLLAMA_BASE_URL", "https://ollama.com").rstrip("/"),
                                 headers={"Authorization": f"Bearer {key}"}, timeout=120) as ollama:
        app.state.agent = Agent(runtime, ollama, os.getenv("OLLAMA_MODEL", "gpt-oss:20b"))
        app.state.configured = bool(key)
        app.state.sessions = {}
        yield


app = FastAPI(title="AgentOS Local Chat", lifespan=lifespan)
allowed_hosts = ["localhost", "127.0.0.1", "[::1]", "testserver"]
if external_host := os.getenv("RENDER_EXTERNAL_HOSTNAME"):
    allowed_hosts.append(external_host)
if frontend_origin := os.getenv("AGENTOS_FRONTEND_ORIGIN", "").rstrip("/"):
    from urllib.parse import urlsplit
    frontend_host = urlsplit(frontend_origin).hostname
    if frontend_host:
        allowed_hosts.append(frontend_host)
app.add_middleware(TrustedHostMiddleware, allowed_hosts=allowed_hosts)


@app.middleware("http")
async def app_password(request: Request, call_next):
    password = os.getenv("AGENTOS_APP_PASSWORD")
    if password and request.url.path != "/health":
        authorization = request.headers.get("authorization", "")
        try:
            scheme, encoded = authorization.split(" ", 1)
            credentials = base64.b64decode(encoded, validate=True).decode("utf-8")
        except (ValueError, UnicodeDecodeError, binascii.Error):
            credentials, scheme = "", ""
        expected = "agentos:" + password
        if scheme.lower() != "basic" or not hmac.compare_digest(credentials, expected):
            return Response(status_code=401, headers={"WWW-Authenticate": 'Basic realm="AgentOS"'})
    return await call_next(request)


@app.middleware("http")
async def local_origin(request: Request, call_next):
    origin = request.headers.get("origin")
    allowed_origins = {str(request.base_url).rstrip("/")}
    if external_host:
        allowed_origins.add(f"https://{external_host}")
    if frontend_origin:
        allowed_origins.add(frontend_origin)
    if request.method not in {"GET", "HEAD"} and origin and origin not in allowed_origins:
        return JSONResponse({"detail": "Cross-origin requests are disabled"}, status_code=403)
    return await call_next(request)


@app.get("/health")
def health():
    return {"status": "ok"}


def session_for(request: Request) -> Session:
    session = request.app.state.sessions.get(request.cookies.get("agentos_chat"))
    if session is None:
        raise HTTPException(401, "Chat session expired. Reload the page.")
    return session


@app.get("/")
def index():
    return FileResponse(ROOT / "static" / "index.html")


@app.post("/api/session")
def start_session(request: Request, response: Response):
    sessions = request.app.state.sessions
    previous = request.cookies.get("agentos_chat")
    if previous in sessions:
        if sessions[previous].lock.locked():
            raise HTTPException(409, "Wait for the current request to finish")
        sessions.pop(previous)
    if len(sessions) >= 100:
        raise HTTPException(429, "Too many local sessions. Restart the chat service.")
    session_id = uuid4().hex
    sessions[session_id] = Session()
    response.set_cookie("agentos_chat", session_id, httponly=True, samesite="strict")
    return {"model": request.app.state.agent.model, "configured": request.app.state.configured}


class ChatMessage(BaseModel):
    message: str = Field(min_length=1, max_length=8000)


@app.post("/api/chat")
async def chat(body: ChatMessage, request: Request):
    session = session_for(request)
    if not request.app.state.configured:
        raise HTTPException(503, "Set OLLAMA_API_KEY in chat_service/.env, then restart this service.")
    if session.lock.locked():
        raise HTTPException(409, "A request is already running in this chat")
    async with session.lock:
        try:
            return await request.app.state.agent.chat(session, body.message)
        except (ValueError, httpx.HTTPError) as error:
            raise HTTPException(502, friendly_error(error)) from error


@app.post("/api/documents", status_code=201)
async def upload(body: DocumentUpload, request: Request):
    session = session_for(request)
    if session.lock.locked():
        raise HTTPException(409, "Wait for the chat request to finish before uploading")
    if len(session.documents) >= 20:
        raise HTTPException(422, "Maximum 20 documents per chat")
    async with session.lock:
        try:
            document = await request.app.state.agent.runtime_json("POST", "/documents", json=body.model_dump())
        except httpx.HTTPError as error:
            raise HTTPException(502, friendly_error(error)) from error
        session.documents[document["id"]] = document
        return document


@app.get("/api/tasks")
async def tasks(request: Request):
    session = session_for(request)
    agent = request.app.state.agent
    result = []
    for task_id, known in list(session.tasks.items()):
        try:
            current = known if known["state"] in {"Completed", "Failed", "Blocked"} else await agent.task(session, task_id, include_result=False)
            result.append(current)
        except httpx.HTTPError as error:
            result.append({**known, "error": friendly_error(error)})
    return {"tasks": result}


@app.get("/api/monitor/tasks")
async def monitor_tasks(request: Request, limit: int = Query(50, ge=1, le=100), offset: int = Query(0, ge=0)):
    session_for(request)
    try:
        return await request.app.state.agent.runtime_json(
            "GET", "/tasks", params={"limit": limit, "offset": offset}
        )
    except httpx.HTTPError as error:
        raise HTTPException(502, friendly_error(error)) from error


@app.get("/api/monitor/tasks/{task_id}/events")
async def monitor_events(task_id: str, request: Request):
    session_for(request)
    try:
        result = await request.app.state.agent.runtime_json("GET", f"/tasks/{task_id}/events")
    except httpx.HTTPError as error:
        raise HTTPException(502, friendly_error(error)) from error
    return {"task_id": task_id, "events": [
        {key: event.get(key) for key in ("sequence", "event_type", "worker_id", "created_at")}
        for event in result["events"]
    ]}


@app.get("/api/tasks/{task_id}/result")
async def result(task_id: str, request: Request):
    session = session_for(request)
    if task_id not in session.tasks:
        raise HTTPException(404, "Task does not belong to this chat")
    try:
        return await request.app.state.agent.runtime_json("GET", f"/tasks/{task_id}/result")
    except httpx.HTTPError as error:
        raise HTTPException(502, friendly_error(error)) from error


@app.post("/api/tasks/{task_id}/cancel")
async def cancel(task_id: str, request: Request):
    session = session_for(request)
    if task_id not in session.tasks:
        raise HTTPException(404, "Task does not belong to this chat")
    try:
        return await request.app.state.agent.runtime_json("POST", f"/tasks/{task_id}/cancel")
    except httpx.HTTPError as error:
        raise HTTPException(502, friendly_error(error)) from error


app.mount("/static", StaticFiles(directory=ROOT / "static"), name="static")
