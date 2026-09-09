import grpc

from .generated import agentos_pb2, agentos_pb2_grpc


class RuntimeClient:
    def __init__(self, address: str):
        self.channel = grpc.insecure_channel(address)
        self.stub = agentos_pb2_grpc.RuntimeStub(self.channel)

    def submit_workflow(self, tasks: list[dict]) -> list[str]:
        request = agentos_pb2.SubmitWorkflowRequest(
            tasks=[
                agentos_pb2.TaskDefinition(
                    id=task["id"],
                    type=task["type"],
                    payload_json=task["payload_json"],
                    dependencies=task["dependencies"],
                    max_attempts=task["max_attempts"],
                    retry_delay_ms=task["retry_delay_ms"],
                    deadline_ms=task["deadline_ms"],
                    idempotent=task["idempotent"],
                    idempotency_key=task["idempotency_key"] or "",
                )
                for task in tasks
            ]
        )
        return list(self.stub.SubmitWorkflow(request).task_ids)

    def cancel_task(self, task_id: str) -> dict[str, str]:
        response = self.stub.CancelTask(
            agentos_pb2.CancelTaskRequest(task_id=task_id)
        )
        return {"id": response.task_id, "state": response.state}
