/* Privacy + Settings tabs. Подключается одной строкой в конец index.html:
 *   <script src="/static/privacy.js"></script>
 * Использует глобальные хелперы index.html: lib(), get(), esc(), toast(), showTab().
 * Вкладки добавляются динамически по тем же соглашениям (data-tab / #pane-<name>), поэтому разметку править не нужно. */
(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const num = n => Number(n || 0).toLocaleString('en-US');
  const API = {
    outbound: () => get('/api/outbound'),
    backend:  () => get('/api/backend'),
  };

  // ---------- каркас вкладок ----------
  function addTab(name, title, icon) {
    const b = document.createElement('button');
    b.className = 'tab'; b.dataset.tab = name;
    b.innerHTML = `<i class="bi ${icon}"></i> ${title}`;
    const audit = document.querySelector('#tabs [data-tab="audit"]');
    $('tabs').insertBefore(b, audit || null);
    const p = document.createElement('div');
    p.className = 'pane'; p.id = 'pane-' + name;
    document.querySelector('.container').appendChild(p);
    return p;
  }
  const privacyPane  = addTab('privacy',  'Privacy',  'bi-shield-lock');
  const settingsPane = addTab('settings', 'Settings', 'bi-gear');

  const baseShow = window.showTab;
  window.showTab = function (name) {
    baseShow(name);
    if (name === 'privacy')  loadPrivacy();
    if (name === 'settings') loadSettings();
  };

  // ---------- Privacy ----------
  const chips = obj => Object.entries(obj || {}).map(([k, v]) => `<span class="chip" style="cursor:default">${esc(k)} ×${num(v)}</span>`).join('') || '<span style="color:var(--mute)">—</span>';
  const stat = (v, l, sub) => `<div class="stat"><div class="v">${v}</div><div class="l">${l}</div><div class="sub">${sub || ''}</div></div>`;

  async function loadPrivacy() {
    const r = await API.outbound();
    if (!r || !r.ok) { privacyPane.innerHTML = '<div class="empty">Server unavailable</div>'; return; }
    const t = r.totals || {}, rows = r.recent || [];
    const statusBadge = s => s === 'sent' ? '<span class="badge b-ok">sent</span>' : s === 'blocked' ? '<span class="badge b-err">blocked</span>' : '<span class="badge b-warn">error</span>';
    privacyPane.innerHTML =
      '<div class="stats">' +
        stat(num(t.requests), 'Cloud requests', 'Nemotron on Nebius Token Factory') +
        stat(num(t.masked_values), 'Values masked', 'replaced by placeholders before sending') +
        stat(num(t.blocked), 'Blocked by Guardian', 'not sent: policy, budget or leftover PII') +
        stat(num((t.prompt_tokens || 0) + (t.completion_tokens || 0)), 'Tokens used', `${num(t.prompt_tokens)} in / ${num(t.completion_tokens)} out`) +
      '</div>' +
      `<div class="card"><h2>Masked by type</h2>${chips(t.masked_by_type)}` +
      '<div style="color:var(--mute);font-size:.8rem;margin-top:8px">The map placeholder → original lives only in this process’s memory for the duration of one request. Documents are never modified; only masked snippets leave this computer.</div></div>' +
      '<div class="card"><h2>Try the masking (runs locally, sends nothing)</h2>' +
      '<textarea id="mp-text" rows="3" placeholder="Paste text with an IBAN, card number, e-mail, SSN…"></textarea>' +
      '<div class="row mt8"><button class="btn-primary" id="mp-btn"><i class="bi bi-eye-slash"></i> Show what the cloud would see</button></div>' +
      '<div class="md" id="mp-out" style="display:none"></div></div>' +
      '<div class="card"><h2>Outbound requests (newest first)</h2>' +
      (rows.length ? '<table><thead><tr><th>Time</th><th>Purpose</th><th>Model tier</th><th>Masked</th><th>Tokens</th><th>ms</th><th>Result</th></tr></thead><tbody>' +
        rows.map((e, i) => `<tr style="cursor:pointer" data-i="${i}"><td>${esc(e.ts)}</td><td>${esc(e.purpose)}</td><td>${esc(e.tier)}<div style="color:var(--mute);font-size:.72rem">${esc(e.model)}</div></td>` +
          `<td>${chips(e.masked)}</td><td>${num((e.prompt_tokens || 0) + (e.completion_tokens || 0))}</td><td>${num(e.latency_ms)}</td><td>${statusBadge(e.status)}</td></tr>` +
          `<tr id="pv-${i}" style="display:none"><td colspan="7"><div style="color:var(--mute);font-size:.76rem;margin-bottom:4px">${e.reason ? esc(e.reason) + ' · ' : ''}What the cloud saw (masked) · sha256 ${esc((e.sha256 || '').slice(0, 16))}…</div><pre style="white-space:pre-wrap;color:var(--dim);font-size:.8rem">${esc(e.preview || '(nothing was sent)')}</pre></td></tr>`).join('') +
        '</tbody></table>' : '<div class="empty">No cloud requests yet. Scan the inbox with a Nemotron backend to see them here.</div>') + '</div>';

    privacyPane.querySelectorAll('tr[data-i]').forEach(tr => tr.addEventListener('click', () => {
      const row = $('pv-' + tr.dataset.i); row.style.display = row.style.display === 'none' ? 'table-row' : 'none';
    }));
    $('mp-btn').addEventListener('click', async () => {
      const r2 = await lib('mask_preview', { text: $('mp-text').value });
      const out = $('mp-out'); out.style.display = 'block';
      out.innerHTML = r2.ok ? `<pre style="white-space:pre-wrap">${esc(r2.masked)}</pre><div style="color:var(--mute);font-size:.8rem">${num(r2.total)} value(s) masked</div>` : esc(r2.error || 'error');
    });
  }

  // ---------- Settings ----------
  let dlTimer = null;
  const MODELS = [
    ['llm',    'Local classifier', 'Qwen3.5-2B (GGUF), offline alternative to Nemotron Nano'],
    ['ocr',    'Local OCR model',  'OvisOCR2 (GGUF), optional: reads scanned documents on this machine'],
    ['mmproj', 'OCR vision projector', 'mmproj-F16 (GGUF), required together with the OCR model'],
  ];
  const mb = b => (b / 1048576).toFixed(0) + ' MB';

  function modelRow(w, title, sub, loc, dl) {
    const d = (dl || {})[w], present = loc[w] && loc[w].present;
    let right;
    if (d && (d.state === 'downloading' || d.state === 'verifying')) {
      const pct = d.total ? Math.round(100 * d.done / d.total) : 0;
      right = `<div style="min-width:220px"><div class="bar" style="display:block"><i style="width:${pct}%"></i></div><div style="font-size:.76rem;color:var(--dim)">${d.state} ${d.total ? mb(d.done) + ' / ' + mb(d.total) : ''}</div></div><button class="btn-outline" data-cancel="${w}">Cancel</button>`;
    } else if (present) {
      right = '<span class="badge b-ok">downloaded</span>';
    } else {
      right = `${d && d.state === 'error' ? `<span class="badge b-err" title="${esc(d.error)}">failed</span>` : ''}` +
              (loc[w] && loc[w].url_configured ? `<button class="btn-outline" data-dl="${w}"><i class="bi bi-download"></i> Download</button>` : '<span style="color:var(--mute);font-size:.8rem">set the URL in .env</span>');
    }
    return `<div class="row" style="justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)"><div><b>${title}</b><div style="color:var(--mute);font-size:.78rem">${sub}</div></div><div class="row">${right}</div></div>`;
  }

  async function loadSettings() {
    const r = await API.backend();
    if (!r || !r.ok) { settingsPane.innerHTML = '<div class="empty">Server unavailable</div>'; return; }
    const prof = await lib('profile_get');
    const lrn = await lib('learning');
    const m = r.remote.models, roles = r.remote.roles, loc = r.local;
    const mode = r.backend, demo = r.demo_mode;
    const btn = (id, label, on, dis) => `<button class="${on ? 'btn-primary' : 'btn-outline'}" data-mode="${id}" ${dis || demo ? 'disabled' : ''}>${label}</button>`;

    // === ИЗМЕНЕНИЕ: секция локальных моделей показывается ТОЛЬКО если сборка их поддерживает ===
    const localModelsSection = (loc && loc.compiled === true) ?
      '<div class="card"><h2>Optional local models (offline, not required)</h2>' +
      '<div style="color:var(--mute);font-size:.8rem;margin-bottom:6px">Downloaded from HuggingFace on demand. Only the model file is downloaded; none of your documents are sent anywhere.</div>' +
      MODELS.map(([w, t, s]) => modelRow(w, t, s, loc, r.downloads)).join('') +
      `<div class="row mt8"><button class="btn-outline" id="load-local" ${demo || !(loc.llm && loc.llm.present) ? 'disabled' : ''}><i class="bi bi-play-circle"></i> Load local model</button></div></div>`
      : '';
    // === КОНЕЦ ИЗМЕНЕНИЯ ===

    settingsPane.innerHTML =
      '<div class="card"><h2>Model backend</h2>' +
      `<div class="row">${btn('remote', '<i class="bi bi-cloud"></i> NVIDIA Nemotron (Nebius Token Factory)', mode === 'remote', !r.remote.configured)}` +
      `${btn('local', '<i class="bi bi-pc-display"></i> Local model', mode === 'local', !(loc && loc.loaded))}${btn('off', 'Rules only', mode === 'off')}</div>` +
      (r.remote.configured ? '' : '<div class="ocr-alert">NEBIUS_API_KEY is not set — cloud models are unavailable. Add it to <code>.env</code> and restart.</div>') +
      (demo ? '<div class="ocr-alert">Demo mode: settings are read-only and the data is synthetic.</div>' : '') +
      '<table style="margin-top:10px"><thead><tr><th>Tier</th><th>Model</th><th>Used for</th></tr></thead><tbody>' +
      ['nano', 'super', 'ultra'].map(k => `<tr><td>${k}</td><td><code>${esc(m[k])}</code> <span class="badge" data-chk="${k}"></span></td><td>${esc(roles[k])}</td></tr>`).join('') +
      '</tbody></table><div class="row mt8"><button class="btn-outline" id="chk-models"><i class="bi bi-check2-circle"></i> Verify models on Token Factory</button>' +
      `<span style="color:var(--mute);font-size:.8rem">endpoint: ${esc(r.remote.host)} · snippet limit ${num(r.privacy.snippet_chars)} chars · ${num(r.privacy.requests_per_hour_limit)} requests/hour</span></div></div>` +

      '<div class="card"><h2>Always hide these (names, addresses)</h2>' +
      '<div style="color:var(--mute);font-size:.8rem;margin-bottom:6px">Structured IDs (cards, IBAN, SSN, e-mail…) are masked automatically. Names and addresses cannot be recognised reliably, so list them here, separated by ";". Optional type prefix: <code>PERSON:Jane Roe; ADDRESS:12 Main St</code></div>' +
      `<textarea id="pt" rows="2">${esc((prof.profile && prof.profile.protected_terms) || '')}</textarea>` +
      '<div class="row mt8"><button class="btn-primary" id="pt-save"><i class="bi bi-check-lg"></i> Save</button></div></div>' +

      // === ВСТАВЛЯЕМ УСЛОВНУЮ СЕКЦИЮ ===
      localModelsSection +

      `<div class="card"><h2>What I learned from your corrections</h2><div class="row">${chips({ 'type rules': lrn.type_rules, 'collection rules': lrn.collection_rules, 'applied this session': lrn.applied })}</div></div>`;

    settingsPane.querySelectorAll('[data-mode]').forEach(b => b.addEventListener('click', async () => {
      const x = await lib('backend_set', { backend: b.dataset.mode });
      x.ok ? toast('Backend: ' + x.backend) : toast(x.error, false); loadSettings();
    }));
    settingsPane.querySelectorAll('[data-dl]').forEach(b => b.addEventListener('click', async () => {
      const x = await lib('model_download', { what: b.dataset.dl });
      x.ok ? toast('Download started') : toast(x.error, false); loadSettings();
    }));
    settingsPane.querySelectorAll('[data-cancel]').forEach(b => b.addEventListener('click', async () => { await lib('model_cancel', { what: b.dataset.cancel }); loadSettings(); }));
    $('chk-models').addEventListener('click', async () => {
      const x = await lib('models_check');
      if (!x.ok) { toast(x.error, false); return; }
      for (const k of Object.keys(x.models)) {
        const el = settingsPane.querySelector(`[data-chk="${k}"]`);
        el.className = 'badge ' + (x.models[k].available ? 'b-ok' : 'b-err'); el.textContent = x.models[k].available ? 'available' : 'not found';
      }
      toast(x.all_available ? 'All configured models are available' : 'Some model IDs were not found: check your .env', x.all_available);
    });
    $('pt-save').addEventListener('click', async () => { const x = await lib('profile_set', { protected_terms: $('pt').value }); toast(x.ok ? 'Saved' : x.error, x.ok); });
    const ll = $('load-local');
    if (ll) ll.addEventListener('click', async () => { toast('Loading model…'); const x = await lib('model_load'); x.ok ? toast('Local model loaded') : toast(x.error, false); loadSettings(); });

    // пока что-то качается — обновляем строку прогресса
    clearTimeout(dlTimer);
    const busy = Object.values(r.downloads || {}).some(d => d.state === 'downloading' || d.state === 'verifying');
    if (busy && settingsPane.classList.contains('show')) dlTimer = setTimeout(loadSettings, 1200);
  }
})();