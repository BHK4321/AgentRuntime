import os
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from chat_service.main import app


class DeploymentAccessTests(unittest.TestCase):
    def test_password_protects_chat_and_leaves_health_check_available(self):
        with patch.dict(os.environ, {"AGENTOS_APP_PASSWORD": "demo-secret"}):
            with TestClient(app) as client:
                self.assertEqual(client.get("/health").status_code, 200)
                unauthenticated = client.get("/")
                self.assertEqual(unauthenticated.status_code, 401)
                self.assertNotIn("www-authenticate", unauthenticated.headers)
                self.assertEqual(client.get("/", auth=("agentos", "wrong")).status_code, 401)
                self.assertEqual(client.get("/", auth=("agentos", "demo-secret")).status_code, 200)

    def test_cross_origin_write_is_rejected(self):
        with TestClient(app) as client:
            response = client.post("/api/session", headers={"Origin": "https://wrong.example"})
        self.assertEqual(response.status_code, 403)
