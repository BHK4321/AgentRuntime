const $ = (id) => document.getElementById(id);
let busy = false;
let appPassword = sessionStorage.getItem('agentos-app-password') || '';
let taskView = 'chat';
let monitorOffset = 0;
const monitorLimit = 50;
async function api(path, options = {}) {
  const send = () => {
    const headers = {...options.headers};
    if (!(options.body instanceof FormData)) headers['Content-Type'] = 'application/json';
    if (appPassword) headers.Authorization = `Basic ${btoa(`agentos:${appPassword}`)}`;
    return fetch(path, {...options, headers});
  };
  let response = await send();
  if (response.status === 401) {
    const entered = window.prompt('Enter your AgentOS deployment password:');
    if (!entered) throw new Error('Deployment password is required. Reload to try again.');
    appPassword = entered;
    sessionStorage.setItem('agentos-app-password', appPassword);
    response = await send();
    if (response.status === 401) {
      sessionStorage.removeItem('agentos-app-password'); appPassword = '';
      throw new Error('Incorrect deployment password. Reload to try again.');
    }
  }
  const data = await response.json();
  if (!response.ok) throw new Error(typeof data.detail === 'string' ? data.detail : JSON.stringify(data.detail));
  return data;
}
function notice(text = '') { $('notice').textContent = text; $('notice').hidden = !text; }
function setBusy(value) {
  busy = value;
  for (const id of ['send', 'file', 'attach', 'new-chat']) $(id).disabled = value;
  $('send').textContent = value ? 'Working…' : 'Send ↑';
}
function message(role, content, tools = []) {
  $('welcome')?.remove();
  const block = document.createElement('div'); block.className = `message ${role}`;
  const label = document.createElement('div'); label.className = 'speaker'; label.textContent = role === 'user' ? 'YOU' : 'AGENTOS';
  const text = document.createElement('div'); text.className = 'message-content';
  if (role === 'assistant' && window.renderMarkdown) text.append(window.renderMarkdown(content));
  else text.textContent = content;
  block.append(label, text);
  if (tools.length) {
    const details = document.createElement('details'), summary = document.createElement('summary'), pre = document.createElement('pre');
    summary.textContent = `${tools.length} runtime tool call${tools.length === 1 ? '' : 's'}`;
    pre.textContent = JSON.stringify(tools, null, 2); details.append(summary, pre); block.append(details);
  }
  $('messages').append(block); block.scrollIntoView({behavior: 'smooth'});
}
async function refreshTasks() {
  if (taskView === 'all') return refreshAllTasks();
  try {
    const {tasks} = await api('/api/tasks');
    if (taskView !== 'chat') return;
    $('task-count').textContent = tasks.length;
    if (!tasks.length) { $('tasks').innerHTML = '<div class="empty-state">No tasks in this chat yet.</div>'; return; }
    $('tasks').replaceChildren();
    for (const task of tasks) {
      const card = document.createElement('div'); card.className = 'task';
      for (const [tag, cls, text] of [['strong', '', task.type], ['div', 'state', task.state], ['div', 'id', task.id]]) {
        const el = document.createElement(tag); el.className = cls; el.textContent = text; card.append(el);
      }
      if (task.error) { const error = document.createElement('p'); error.className = 'error'; error.textContent = task.error; card.append(error); }
      const terminal = ['Completed', 'Failed', 'Blocked'].includes(task.state);
      if (!terminal || (task.state === 'Completed' && task.type !== 'sleep') || (task.state === 'Failed' && task.type === 'python_script')) {
        const button = document.createElement('button'); button.textContent = terminal ? 'View result ↗' : 'Cancel task';
        button.onclick = async () => {
          button.disabled = true;
          try {
            if (terminal) {
              const result = await api(`/api/tasks/${encodeURIComponent(task.id)}/result`);
              $('artifact-content').textContent = (result.content ?? 'No file output.') + (result.report ? '\n\nExecution report:\n' + JSON.stringify(result.report, null, 2) : ''); $('artifact').showModal();
            } else {
              await api(`/api/tasks/${encodeURIComponent(task.id)}/cancel`, {method: 'POST'}); await refreshTasks();
            }
          } catch (error) { notice(error.message); } finally { button.disabled = false; }
        }; card.append(button);
      }
      $('tasks').append(card);
    }
  } catch (error) { notice(error.message); }
}
async function refreshAllTasks() {
  try {
    const page = await api(`/api/monitor/tasks?limit=${monitorLimit}&offset=${monitorOffset}`);
    if (taskView !== 'all') return;
    $('task-count').textContent = page.total;
    $('tasks').replaceChildren();
    for (const task of page.tasks) {
      const card = document.createElement('div'); card.className = 'task';
      for (const [tag, cls, value] of [['strong', '', task.type], ['div', 'state', task.state], ['div', 'id', task.id]]) {
        const el = document.createElement(tag); el.className = cls; el.textContent = value; card.append(el);
      }
      const meta = document.createElement('div'); meta.className = 'monitor-meta';
      meta.textContent = `Attempts ${task.attempts}/${task.max_attempts} · Updated ${new Date(task.updated_at).toLocaleString()}`;
      card.append(meta);
      if (task.dependencies.length) {
        const deps = document.createElement('div'); deps.className = 'monitor-dependencies';
        deps.textContent = `Depends on: ${task.dependencies.join(', ')}`; card.append(deps);
      }
      const events = document.createElement('button'); events.className = 'monitor-events'; events.textContent = 'View events';
      events.onclick = async () => {
        events.disabled = true;
        try {
          const history = await api(`/api/monitor/tasks/${encodeURIComponent(task.id)}/events`);
          $('artifact-content').textContent = history.events.length
            ? history.events.map(event => `${new Date(event.created_at).toLocaleString()}  ${event.event_type}${event.worker_id ? `  (${event.worker_id})` : ''}`).join('\n')
            : 'No events recorded.';
          $('artifact').showModal();
        } catch (error) { notice(error.message); } finally { events.disabled = false; }
      }; card.append(events); $('tasks').append(card);
    }
    if (!page.tasks.length) {
      const empty = document.createElement('div'); empty.className = 'empty-state'; empty.textContent = page.total ? 'No tasks on this page.' : 'No runtime tasks yet.'; $('tasks').append(empty);
    }
    $('monitor-pages').hidden = page.total <= monitorLimit;
    $('monitor-prev').disabled = monitorOffset === 0;
    $('monitor-next').disabled = monitorOffset + monitorLimit >= page.total;
    $('monitor-page').textContent = page.total ? `${monitorOffset + 1}–${Math.min(monitorOffset + monitorLimit, page.total)} of ${page.total}` : '';
  } catch (error) { notice(error.message); }
}
$('view-chat').onclick = () => {
  taskView = 'chat'; $('view-chat').classList.add('selected'); $('view-all').classList.remove('selected');
  $('monitor-pages').hidden = true; $('activity-description').textContent = 'Follow the work, from submission to result.'; refreshTasks();
};
$('view-all').onclick = () => {
  taskView = 'all'; monitorOffset = 0; $('view-all').classList.add('selected'); $('view-chat').classList.remove('selected');
  $('activity-description').textContent = 'Tasks from every chat and API client. Select a task to inspect its events.'; refreshTasks();
};
$('monitor-prev').onclick = () => { monitorOffset = Math.max(0, monitorOffset - monitorLimit); refreshTasks(); };
$('monitor-next').onclick = () => { monitorOffset += monitorLimit; refreshTasks(); };
$('chat-form').onsubmit = async (event) => {
  event.preventDefault(); const prompt = $('prompt').value.trim(); if (!prompt || busy) return;
  setBusy(true); notice(); message('user', prompt); $('prompt').value = '';
  try { const reply = await api('/api/chat', {method: 'POST', body: JSON.stringify({message: prompt})}); message('assistant', reply.message, reply.tools); }
  catch (error) { message('assistant', error.message); }
  finally { setBusy(false); await refreshTasks(); }
};
$('prompt').onkeydown = (event) => { if (event.key === 'Enter' && !event.shiftKey) { event.preventDefault(); $('chat-form').requestSubmit(); } };
document.querySelectorAll('[data-prompt]').forEach(button => button.onclick = () => { $('prompt').value = button.dataset.prompt; $('prompt').focus(); });
$('attach').onclick = () => $('file').click();
$('file').onchange = async () => {
  const file = $('file').files[0]; if (!file || busy) return;
  if (file.size > 10 * 1024 * 1024) { notice('Choose a supported document smaller than 10 MiB.'); $('file').value = ''; return; }
  setBusy(true); notice();
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    let binary = '';
    for (let offset = 0; offset < bytes.length; offset += 0x8000) binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    const content_base64 = btoa(binary);
    const doc = await api('/api/documents', {method: 'POST', body: JSON.stringify({filename: file.name, content_base64})});
    const chip = document.createElement('span'); chip.className = 'attachment'; chip.textContent = `✓ ${doc.filename}`; chip.title = doc.id; $('attachments').append(chip);
    notice('Document uploaded. You can now ask AgentOS to inspect or process its extracted text.');
  } catch (error) { notice(error.message); }
  finally { $('file').value = ''; setBusy(false); }
};
$('new-chat').onclick = () => location.reload();
$('close-artifact').onclick = () => $('artifact').close();
async function poll() { await refreshTasks(); setTimeout(poll, 4000); }
async function start() {
  setBusy(true);
  try {
    const config = await api('/api/session', {method: 'POST'}); $('model').textContent = config.model;
    if (!config.configured) notice('Add your Ollama Cloud API key to chat_service/.env and restart the chat service.');
    poll();
  } catch (error) { notice(error.message); }
  finally { setBusy(false); }
}
start();
