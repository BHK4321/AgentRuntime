CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY,
    type TEXT NOT NULL DEFAULT 'cpp_callback',
    payload_json TEXT NOT NULL DEFAULT '{}',
    state TEXT NOT NULL,
    max_attempts INTEGER NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    retry_delay_ms BIGINT NOT NULL DEFAULT 0,
    deadline_ms BIGINT NOT NULL DEFAULT 0,
    idempotent BOOLEAN NOT NULL DEFAULT FALSE,
    idempotency_key TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

ALTER TABLE tasks ADD COLUMN IF NOT EXISTS type TEXT NOT NULL DEFAULT 'cpp_callback';
ALTER TABLE tasks ADD COLUMN IF NOT EXISTS payload_json TEXT NOT NULL DEFAULT '{}';

CREATE TABLE IF NOT EXISTS task_dependencies (
    task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    dependency_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, dependency_id)
);

CREATE TABLE IF NOT EXISTS runtime_events (
    sequence BIGSERIAL PRIMARY KEY,
    event_type TEXT NOT NULL,
    task_id TEXT NOT NULL DEFAULT '',
    worker_id TEXT NOT NULL DEFAULT '',
    idempotency_key TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS runtime_events_task_id_idx
    ON runtime_events (task_id, sequence);

CREATE INDEX IF NOT EXISTS tasks_state_idx ON tasks (state);

CREATE TABLE IF NOT EXISTS workers (
    id TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'ACTIVE',
    last_heartbeat TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS executions (
    id BIGSERIAL PRIMARY KEY,
    task_id TEXT NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    worker_id TEXT REFERENCES workers(id) ON DELETE SET NULL,
    attempt INTEGER NOT NULL,
    status TEXT NOT NULL,
    lease_expires_at TIMESTAMPTZ,
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at TIMESTAMPTZ,
    error TEXT
);

CREATE INDEX IF NOT EXISTS executions_task_id_idx ON executions (task_id, id);
CREATE INDEX IF NOT EXISTS executions_active_lease_idx
    ON executions (lease_expires_at)
    WHERE status = 'RUNNING';
