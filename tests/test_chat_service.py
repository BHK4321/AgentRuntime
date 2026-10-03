import base64
import json
import os
import tempfile
import unittest
import zipfile
from io import BytesIO
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

import httpx
from fastapi import FastAPI
from fastapi.testclient import TestClient

from api.documents import create_document_router
from chat_service.agent import Agent, Session, Workflow, TOOLS
from chat_service.main import app


class AgentTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.requests = []
        self.model_calls = 0
        def runtime(request):
            self.requests.append(request)
            if request.method == "POST":
                tasks = json.loads(request.content)["tasks"]
                return httpx.Response(201, json={"task_ids": [task["id"] for task in tasks]})
            if request.url.path.endswith("/result"):
                return httpx.Response(200, json={"content": '{"words": 2}'})
            return httpx.Response(200, json={"id": request.url.path.split("/")[-1], "state": "Completed", "payload": {"output_path": "output/old.txt"}})
        self.runtime = httpx.AsyncClient(base_url="https://runtime.test", transport=httpx.MockTransport(runtime))
        self.ollama = httpx.AsyncClient(base_url="https://model.test", transport=httpx.MockTransport(self.model))
        self.agent = Agent(self.runtime, self.ollama, "test-model")
        self.session = Session(documents={"doc": {"id": "doc", "filename": "sample.txt", "storage_path": "uploads/doc.txt"}})

    def model(self, request):
        self.model_calls += 1
        body = json.loads(request.content)
        self.assertEqual(body["model"], "test-model")
        if self.model_calls == 1:
            return httpx.Response(200, json={"message": {"role": "assistant", "content": "", "tool_calls": [
                {"function": {"name": "submit_workflow", "arguments": {"steps": [{"name": "wait", "type": "sleep", "seconds": 1}]}}}
            ]}})
        self.assertEqual(body["messages"][-1]["role"], "tool")
        self.assertEqual(body["messages"][-1]["tool_name"], "submit_workflow")
        return httpx.Response(200, json={"message": {"content": "Your task was submitted."}})

    async def asyncTearDown(self):
        await self.runtime.aclose()
        await self.ollama.aclose()

    async def test_tool_loop_submits_and_returns_result_to_model(self):
        result = await self.agent.chat(self.session, "Run a test")
        self.assertEqual(result["message"], "Your task was submitted.")
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(len(self.session.tasks), 1)

    async def test_nested_tool_schema_is_visible_without_references(self):
        schema = next(tool["function"]["parameters"] for tool in TOOLS if tool["function"]["name"] == "submit_workflow")
        self.assertNotIn('"$ref"', json.dumps(schema))
        step = schema["properties"]["steps"]["items"]
        self.assertIn("type", step["required"])
        self.assertIn("input", step["properties"])

    async def test_bad_word_count_call_receives_usable_correction(self):
        calls = []
        def model(request):
            body = json.loads(request.content)
            calls.append(body)
            if len(calls) == 1:
                arguments = {"steps": [{"name": "count", "function": "word_count", "params": {"file_id": "doc"}}]}
            elif len(calls) == 2:
                correction = json.loads(body["messages"][-1]["content"])
                self.assertEqual(self.requests, [])
                self.assertNotIn("pydantic.dev", json.dumps(correction))
                arguments = correction["expected_arguments_example"]
                self.assertEqual(arguments["steps"][0]["input"], "doc")
            else:
                return httpx.Response(200, json={"message": {"content": "Submitted."}})
            return httpx.Response(200, json={"message": {"tool_calls": [
                {"function": {"name": "submit_workflow", "arguments": arguments}}]}})
        async with httpx.AsyncClient(base_url="https://model.test", transport=httpx.MockTransport(model)) as client:
            await Agent(self.runtime, client, "test").chat(self.session, "Count words")
        self.assertEqual(len(self.requests), 1)
        task = json.loads(self.requests[0].content)["tasks"][0]
        self.assertEqual(task["type"], "word_count")
        self.assertEqual(task["payload"]["input_path"], "uploads/doc.txt")

    async def test_chained_file_workflow_maps_paths_and_dependencies(self):
        await self.agent.submit(self.session, Workflow(steps=[
            {"name": "upper", "type": "text_transform", "input": "doc", "operation": "uppercase"},
            {"name": "count", "type": "word_count", "input": "upper"},
        ]))
        tasks = json.loads(self.requests[0].content)["tasks"]
        self.assertEqual(tasks[0]["payload"]["input_path"], "uploads/doc.txt")
        self.assertEqual(tasks[1]["payload"]["input_path"], tasks[0]["payload"]["output_path"])
        self.assertEqual(tasks[1]["dependencies"], [tasks[0]["id"]])

    async def test_sleep_dag_submitted_atomically_with_out_of_order_steps(self):
        await self.agent.submit(self.session, Workflow(steps=[
            {"name": "join", "type": "sleep", "seconds": 1, "dependencies": ["left", "right"]},
            {"name": "left", "type": "sleep", "seconds": 2, "dependencies": ["start"]},
            {"name": "start", "type": "sleep", "seconds": 1},
            {"name": "right", "type": "sleep", "seconds": 2, "dependencies": ["start"]},
        ]))
        self.assertEqual(len(self.requests), 1)
        tasks = json.loads(self.requests[0].content)["tasks"]
        by_name = {task["id"].rsplit("-", 1)[1]: task for task in tasks}
        self.assertEqual(by_name["join"]["dependencies"], [by_name["left"]["id"], by_name["right"]["id"]])
        self.assertEqual(by_name["left"]["dependencies"], [by_name["start"]["id"]])
        self.assertEqual(by_name["right"]["dependencies"], [by_name["start"]["id"]])
        self.assertEqual(by_name["start"]["dependencies"], [])

    async def test_rejects_unknown_paths_cycles_and_unsupported_operations(self):
        for steps in [
            [{"name": "bad", "type": "word_count", "input": "../../secret"}],
            [{"name": "bad", "type": "sleep", "dependencies": ["bad"]}],
            [{"name": "bad", "type": "text_transform", "input": "doc"}],
            [{"name": "bad", "type": "shell"}],
        ]:
            with self.assertRaises(ValueError):
                await self.agent.submit(self.session, Workflow(steps=steps))
        self.assertEqual(self.requests, [])

    async def test_task_access_is_scoped_to_session(self):
        with self.assertRaises(ValueError):
            await self.agent.tool(self.session, "cancel_task", {"task_id": "other-user-task"})
        self.assertEqual(self.requests, [])

    async def test_script_submission_keeps_code_and_document_mapping(self):
        code = "from pathlib import Path\nPath('output/result.txt').write_text('done')"
        await self.agent.submit(self.session, Workflow(steps=[{"name": "custom", "type": "python_script", "input": "doc", "code": code}]))
        task = json.loads(self.requests[0].content)["tasks"][0]
        self.assertEqual(task["payload"]["code"], code)
        self.assertEqual(task["payload"]["input_path"], "uploads/doc.txt")
        self.assertTrue(task["payload"]["output_path"].endswith(".txt"))

    async def test_document_preview_scope_and_script_code_validation(self):
        with self.assertRaises(ValueError):
            await self.agent.tool(self.session, "preview_document", {"document_id": "not-mine"})
        with self.assertRaises(ValueError):
            await self.agent.submit(self.session, Workflow(steps=[{"name": "custom", "type": "python_script", "input": "doc"}]))
        self.assertEqual(self.requests, [])

    async def test_script_submissions_are_bounded_per_turn(self):
        def model(request):
            return httpx.Response(200, json={"message": {"tool_calls": [{"function": {
                "name": "submit_workflow", "arguments": {"steps": [{"name": "custom", "type": "python_script", "input": "doc", "code": "pass"}]}
            }}]}})
        async with httpx.AsyncClient(base_url="https://model.test", transport=httpx.MockTransport(model)) as client:
            result = await Agent(self.runtime, client, "test").chat(self.session, "Process the document")
        self.assertEqual(len(self.requests), 3)
        self.assertIn("Three script executions", result["tools"][-1]["result"]["error"])

    async def test_prior_completed_output_has_no_external_dependency(self):
        self.session.tasks["old"] = {"payload": {"output_path": "output/old.txt"}, "state": "Completed"}
        await self.agent.submit(self.session, Workflow(steps=[{"name": "count", "type": "word_count", "input": "old"}]))
        task = json.loads(self.requests[-1].content)["tasks"][0]
        self.assertEqual(task["dependencies"], [])
        self.assertEqual(task["payload"]["input_path"], "output/old.txt")

    async def test_timeout_retains_task_ids_for_reconciliation(self):
        async def timeout(request):
            raise httpx.ReadTimeout("timeout", request=request)
        async with httpx.AsyncClient(base_url="https://runtime.test", transport=httpx.MockTransport(timeout)) as client:
            agent = Agent(client, self.ollama, "test")
            with self.assertRaises(httpx.ReadTimeout):
                await agent.submit(self.session, Workflow(steps=[{"name": "wait", "type": "sleep"}]))
        self.assertEqual(next(iter(self.session.tasks.values()))["state"], "Submission uncertain")

    async def test_completed_result_reaches_model_adapter(self):
        self.session.tasks["mine"] = {"payload": {}, "state": "Submitted"}
        result = await self.agent.task(self.session, "mine")
        self.assertEqual(json.loads(result["result"])["words"], 2)

    async def test_invalid_model_reply_does_not_execute(self):
        async with httpx.AsyncClient(base_url="https://model.test", transport=httpx.MockTransport(
                lambda request: httpx.Response(200, json={"message": {"tool_calls": ["invalid"]}}))) as client:
            with self.assertRaises(ValueError):
                await Agent(self.runtime, client, "test").chat(self.session, "Run a task")
        self.assertEqual(self.requests, [])

    async def test_model_loop_is_bounded(self):
        calls = []
        def model(request):
            calls.append(request)
            return httpx.Response(200, json={"message": {"tool_calls": [
                {"function": {"name": "unknown", "arguments": {}}}]}})
        async with httpx.AsyncClient(base_url="https://model.test", transport=httpx.MockTransport(model)) as client:
            result = await Agent(self.runtime, client, "test").chat(self.session, "Hello")
        self.assertEqual(len(calls), 6)
        self.assertIn("Tool limit reached", result["message"])
        self.assertIn("No new tasks were submitted", result["message"])
        self.assertEqual(self.requests, [])


class DocumentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {"AGENTOS_WORK_DIR": self.temp.name})
        self.env.start()
        self.row = None
        self.inserted = []
        self.fail = False
        owner = self
        class Connection:
            def execute(self, sql, args):
                if owner.fail:
                    raise RuntimeError("database unavailable")
                if sql.startswith("INSERT"):
                    owner.inserted.append(args)
                return self
            def fetchone(self):
                return owner.row
        @contextmanager
        def database():
            yield Connection()
        api = FastAPI()
        api.include_router(create_document_router(database))
        self.client = TestClient(api, raise_server_exceptions=False)

    def tearDown(self):
        self.client.close()
        self.env.stop()
        self.temp.cleanup()

    def test_upload_writes_bytes_and_metadata(self):
        result = self.client.post("/documents", json={"filename": "../sample.txt", "content": "Hello world"})
        self.assertEqual(result.status_code, 201)
        document = result.json()
        self.assertEqual((Path(self.temp.name) / document["storage_path"]).read_text(), "Hello world")
        self.assertEqual(self.inserted[0][2], document["storage_path"])
        self.assertEqual(self.inserted[0][3], 11)

    def test_failed_insert_removes_file(self):
        self.fail = True
        self.assertEqual(self.client.post("/documents", json={"filename": "a.txt", "content": "a"}).status_code, 500)
        self.assertEqual(list(Path(self.temp.name).rglob("*.txt")), [])

    def test_docx_upload_preserves_original_and_previews_extracted_text(self):
        package = BytesIO()
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("word/document.xml", """<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Hello Word</w:t></w:r></w:p></w:body></w:document>""")
        original = package.getvalue()
        response = self.client.post("/documents", json={
            "filename": "sample.docx", "content_base64": base64.b64encode(original).decode("ascii")
        })
        self.assertEqual(response.status_code, 201, response.text)
        document = response.json()
        root = Path(self.temp.name)
        self.assertEqual((root / document["storage_path"]).read_bytes(), original)
        self.assertEqual((root / document["text_path"]).read_text(encoding="utf-8"), "Hello Word")
        self.row = (document["storage_path"],)
        preview = self.client.get(f"/documents/{document['id']}/preview")
        self.assertEqual(preview.json()["content"], "Hello Word")

    def test_rejects_malformed_pdf_and_binary_text(self):
        for name, content in [("a.pdf", "abc"), ("a.txt", "\x00")]:
            self.assertEqual(self.client.post("/documents", json={"filename": name, "content": content}).status_code, 422)

    def test_result_requires_completion_and_confined_path(self):
        self.row = ("word_count", '{"output_path":"../secret.txt"}', "Running")
        self.assertEqual(self.client.get("/tasks/task/result").status_code, 409)
        self.row = (*self.row[:2], "Completed")
        self.assertEqual(self.client.get("/tasks/task/result").status_code, 422)
        path = Path(self.temp.name) / "stats.json"
        path.write_text('{"words": 2}')
        self.row = ("word_count", '{"output_path":"stats.json"}', "Completed")
        self.assertEqual(self.client.get("/tasks/task/result").json()["content"], '{"words": 2}')

    def test_failed_script_report_can_be_retrieved(self):
        self.row = ("python_script", '{"output_path":"output/script.txt"}', "Failed")
        path = Path(self.temp.name) / "output/script.txt.report.json"
        path.parent.mkdir()
        path.write_text('{"ok":false,"stderr":"ValueError: missing column"}')
        result = self.client.get("/tasks/task/result")
        self.assertEqual(result.status_code, 200)
        self.assertIn("missing column", result.json()["report"]["stderr"])


class LocalServiceTests(unittest.TestCase):
    def test_ui_session_missing_key_and_origin_protection(self):
        with patch.dict(os.environ, {"OLLAMA_API_KEY": ""}), TestClient(app) as client:
            self.assertEqual(client.get("/").status_code, 200)
            self.assertEqual(client.get("/static/app.js").status_code, 200)
            self.assertEqual(client.get("/api/tasks").status_code, 401)
            config = client.post("/api/session").json()
            self.assertFalse(config["configured"])
            self.assertNotIn("api_key", config)
            self.assertEqual(client.get("/api/tasks").json(), {"tasks": []})
            self.assertEqual(client.post("/api/chat", json={"message": "Hello"}).status_code, 503)
            self.assertEqual(client.post("/api/session", headers={"Origin": "https://untrusted.test"}).status_code, 403)
            self.assertEqual(client.post("/api/tasks/unknown/cancel").status_code, 404)


if __name__ == "__main__":
    unittest.main()
