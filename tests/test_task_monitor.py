import unittest
from contextlib import contextmanager
from unittest.mock import patch

from fastapi.testclient import TestClient

from api.main import app


class FakeResult:
    def __init__(self, rows):
        self.rows = rows

    def fetchone(self):
        return self.rows[0]

    def fetchall(self):
        return self.rows

    def __iter__(self):
        return iter(self.rows)


class FakeConnection:
    def execute(self, query, parameters=None):
        if query.startswith("SELECT COUNT"):
            return FakeResult([(1,)])
        if query.startswith("SELECT id, type, state"):
            assert parameters == (50, 0)
            return FakeResult([("task-2", "sleep", "Completed", 1, 2,
                                "2026-10-03T00:00:00Z", "2026-10-03T00:00:01Z")])
        if query.startswith("SELECT task_id, dependency_id"):
            assert parameters == (["task-2"],)
            return FakeResult([("task-2", "task-1")])
        raise AssertionError(query)


@contextmanager
def fake_database():
    yield FakeConnection()


class TaskMonitorTests(unittest.TestCase):
    def test_list_returns_persisted_task_state_and_dependencies_without_payload(self):
        with patch("api.main.database", fake_database), TestClient(app) as client:
            response = client.get("/tasks")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["total"], 1)
        task = response.json()["tasks"][0]
        self.assertEqual(task["dependencies"], ["task-1"])
        self.assertEqual(task["attempts"], 1)
        self.assertNotIn("payload", task)
        self.assertNotIn("idempotency_key", task)

    def test_page_size_is_bounded(self):
        with TestClient(app) as client:
            response = client.get("/tasks?limit=101")
        self.assertEqual(response.status_code, 422)
