# GitHub auto-deploy: Render backend and Vercel frontend

This test deployment uses one Render Docker web service for PostgreSQL, the
Python chat API, FastAPI, and the C++ runtime. The browser UI is static on
Vercel. PostgreSQL, uploads, and task results all live on the container's
temporary filesystem. This deployment does not create a Render Postgres
resource and has no 30-day database expiration.
Both projects deploy automatically from commits pushed to the connected GitHub
branch. Local, uncommitted changes are not deployed.

## 1. Push this code to GitHub

Commit the deployment files and the current AgentOS implementation, then push
to `main`. Do not commit `.env` files or API keys. The root `.gitignore` excludes
those files. If you use a different production branch, select it in both hosts.

## 2. Create the Render backend

In Render, connect your GitHub account and create a **Blueprint** from this
repository's root `render.yaml`. Connecting the GitHub account is important:
deploying by pasting a public repository URL does not enable Render auto-deploys.
The Blueprint creates only `agentos-backend` in Singapore. During
setup, enter `OLLAMA_API_KEY` and a strong `AGENTOS_APP_PASSWORD` in Render's
secret prompts. The password protects the public chat API; enter it in the UI
when asked. Do not put it in the repository.

The web service uses a free test plan. Render's free web service spins down
after inactivity, and its filesystem is erased on restart, spin-down, or
redeploy. On each container start, the deployment entrypoint initializes a
fresh local PostgreSQL cluster, creates the `agentos` database, and applies
`database/schema.sql`. Existing tasks, documents, and results are lost. A task
running when the container stops is interrupted, not recovered. The free
instance has limited memory; if PostgreSQL and the three application processes
exceed it, use a larger plan or a separate database.

Wait for the Render deployment to become healthy. Copy its HTTPS URL (for
example, `https://agentos-backend.onrender.com`). The root page on Render also
serves the UI for a direct backend smoke test; it requires the password.

## 3. Create the Vercel frontend

Import the **same GitHub repository** into Vercel. Set its Root Directory to
`chat_service/static` and its Framework Preset to **Other**. Set the Render
HTTPS origin in `chat_service/static/vercel.json`, then deploy. That file serves
the static UI and rewrites `/api/*` to the Render chat service; the Ollama key
stays on Render. If Render assigns a different URL, update the rewrite target
and push the change to `main` before deploying the frontend.

Copy the Vercel production URL. In the Render service's Environment settings,
set `AGENTOS_FRONTEND_ORIGIN` to that exact origin, for example
`https://agentos-frontend.vercel.app`, with no trailing slash or path. Save and
redeploy Render. This allows browser writes from that Vercel origin while
rejecting unrelated origins. Vercel preview URLs need their own allowed origin;
the production URL is sufficient for this test setup.

Open the Vercel URL, enter `AGENTOS_APP_PASSWORD` when prompted, and submit a
two-second sleep task. The browser keeps the password only in session storage.
Check **All tasks** and **View events** to verify the runtime and database.

## Updates

Push a commit to `main`. Render's `autoDeployTrigger: commit` builds and deploys
the backend from the new commit. Vercel's Git integration deploys the frontend
from the same commit. A push that only changes the frontend still triggers a
Render build with this simple configuration. During a redeploy, Render can run
old and new containers briefly at the same time. Each has its own temporary
database, so their tasks and files are separate. After traffic moves to the
new container, the old tasks and files disappear when it stops.

This is a single-user test deployment. Chat sessions, PostgreSQL task records,
uploads, and file results reset when Render replaces or spins down the
container. Re-upload documents for a new test session. Your local development
setup is unchanged and still uses its configured PostgreSQL service.
The generated `python_script` handler executes code with the service account's
permissions, so keep the deployment password private and avoid giving access
to untrusted users.
