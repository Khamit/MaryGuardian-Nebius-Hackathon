/* X-ray / Case file: страница документа с рамками «что нашли», «что скрыл Guardian», «что подозрительно».
 * Подключается одной строкой после privacy.js:   <script src="/static/xray.js"></script>
 * Использует глобальные хелперы index.html: lib(), post(), esc(), toast(), loadSide(), loadList(), pick(), DOCS. */
(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const SEV = { 3: ['b-err', 'high risk'], 2: ['b-warn', 'check'], 1: ['b-ok', 'ok'] };

  // ---------- стили ----------
  const css = document.createElement('style');
  css.textContent = `
  .xr-back{display:none;position:fixed;inset:0;background:rgba(0,0,0,.78);z-index:1500;align-items:center;justify-content:center}
  .xr-back.show{display:flex}
  .xr-box{background:var(--card);border:1px solid var(--border);border-radius:12px;width:min(1200px,96vw);height:min(92vh,920px);display:flex;flex-direction:column}
  .xr-box>header{display:flex;gap:12px;align-items:center;justify-content:space-between;padding:10px 16px;border-bottom:1px solid var(--border)}
  .xr-cap{flex:1;color:var(--dim);font-size:.84rem;min-height:1.2em;transition:opacity .3s}
  .xr-body{display:grid;grid-template-columns:minmax(0,1fr) 350px;gap:12px;flex:1;min-height:0;padding:12px}
  @media (max-width:900px){.xr-body{grid-template-columns:1fr}}
  .xr-page{overflow:auto;background:#0b0d11;border-radius:8px;padding:12px}
  .xr-stage{position:relative;max-width:760px;margin:0 auto}
  .xr-stage img{display:block;width:100%;height:auto;background:#fff;border-radius:4px}
  #xr-layer{position:absolute;inset:0;pointer-events:none}
  .xb{position:absolute;box-sizing:border-box;opacity:0;transform:scale(1.07);transition:opacity .35s,transform .35s;border-radius:3px}
  .xb.on{opacity:1;transform:none}
  .xb-field{border:2px solid #22c55e;background:rgba(34,197,94,.14)}
  .xb-risk{border:2px solid #ef4444;background:rgba(239,68,68,.2)}
  .xb-risk.on{animation:xpulse 1.1s ease-in-out infinite}
  .xb-inject{border:2px solid #f59e0b;background:rgba(245,158,11,.22)}
  .xb-hidden{border:2px dashed #a855f7;background:rgba(168,85,247,.2)}
  .xb-mask{background:#000;color:#fff;overflow:hidden;display:flex;align-items:center;padding:0 3px;white-space:nowrap;font-family:ui-monospace,Menlo,monospace;font-weight:600;border-radius:2px}
  .xb-field::before,.xb-risk::before,.xb-inject::before,.xb-hidden::before{content:attr(data-label);position:absolute;top:-15px;left:-2px;font:600 9px system-ui,sans-serif;padding:1px 5px;border-radius:3px;white-space:nowrap;color:#fff;background:#14532d}
  .xb-risk::before{background:#7f1d1d}.xb-inject::before{background:#78350f}.xb-hidden::before{background:#581c87}
  .xr-cloud .xb:not(.xb-mask){opacity:.12!important;animation:none}
  .xr-side{overflow:auto;display:flex;flex-direction:column;gap:10px}
  .xr-verdict{border:1px solid var(--border);border-radius:10px;padding:12px}
  .xr-verdict.s3{border-color:#7f1d1d;background:var(--err-bg)}.xr-verdict.s2{border-color:#5a4210;background:var(--warn-bg)}.xr-verdict.s0,.xr-verdict.s1{border-color:#1d4a2e;background:var(--ok-bg)}
  .xr-verdict h3{font-size:.95rem;margin-bottom:4px}
  .xr-flag{border-left:3px solid var(--border-hi);padding:6px 10px;margin-top:8px;font-size:.82rem;color:var(--dim)}
  .xr-flag.s3{border-color:var(--err)}.xr-flag.s2{border-color:var(--warn)}.xr-flag.s1{border-color:var(--ok)}
  .xr-flag b{color:var(--text);font-size:.74rem;letter-spacing:.4px}
  .xr-legend{display:flex;flex-wrap:wrap;gap:6px;font-size:.72rem;color:var(--dim)}
  .xr-legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}
  .xr-ins{border:1px solid var(--border);border-radius:8px;padding:9px 11px;margin:10px 0;font-size:.82rem;color:var(--dim)}
  @keyframes xpulse{50%{box-shadow:0 0 0 5px rgba(239,68,68,.25)}}
  @media (prefers-reduced-motion:reduce){.xb{transition:none}.xb-risk.on{animation:none}}`;
  document.head.appendChild(css);

  // ---------- каркас окна ----------
  const modal = document.createElement('div');
  modal.className = 'xr-back';
  modal.id = 'xr';
  modal.innerHTML = `<div class="xr-box">
    <header><b id="xr-title">X-ray</b><span class="xr-cap" id="xr-cap"></span>
      <span class="row"><button class="btn-outline" id="xr-replay"><i class="bi bi-play-fill"></i> Replay</button>
      <button class="btn-outline" id="xr-cloud"><i class="bi bi-cloud"></i> Cloud view</button>
      <button class="btn-outline" id="xr-close">Close</button></span></header>
    <div class="xr-body"><div class="xr-page" id="xr-page"><div class="xr-stage" id="xr-stage"><img id="xr-img" alt="document page"><div id="xr-layer"></div></div></div>
    <aside class="xr-side" id="xr-side"></aside></div></div>`;
  document.body.appendChild(modal);
  modal.addEventListener('click', e => { if (e.target === modal) closeX(); });
  $('xr-close').onclick = closeX;
  $('xr-replay').onclick = () => play();
  $('xr-cloud').onclick = () => { stage().classList.toggle('xr-cloud'); setCloudButton(); };
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && modal.classList.contains('show')) closeX(); });

  const stage = () => $('xr-stage');
  const caption = t => { $('xr-cap').textContent = t || ''; };
  const setCloudButton = () => { $('xr-cloud').classList.toggle('btn-primary', stage().classList.contains('xr-cloud')); };
  function closeX() { modal.classList.remove('show'); runId++; }

  // ---------- данные ----------
  let cur = { id: 0, page: 0, data: null }, runId = 0;

  async function openXray(id, page) {
    cur = { id, page: page || 0, data: null };
    modal.classList.add('show');
    stage().classList.remove('xr-cloud'); setCloudButton();
    $('xr-layer').innerHTML = ''; $('xr-img').removeAttribute('src');
    $('xr-side').innerHTML = '<div class="empty">Reading the page…</div>';
    caption('Rendering the page locally…');
    const r = await lib('xray', { id, page: cur.page });
    if (!r.ok) { $('xr-side').innerHTML = `<div class="empty">${esc(r.error || 'X-ray failed')}</div>`; caption(''); return; }
    cur.data = r;
    $('xr-title').textContent = 'X-ray · ' + (r.doc && r.doc.filename || '');
    const img = $('xr-img');
    img.onload = () => { buildLayer(r); renderSide(r); play(); };
    img.src = 'data:image/png;base64,' + r.png_b64;
  }

  function buildLayer(r) {
    const layer = $('xr-layer'), W = $('xr-img').clientWidth;
    layer.innerHTML = r.boxes.map((b, i) => {
      const fs = Math.max(7, Math.min(13, b.h * W * 1.3 * 0.62));
      return `<div class="xb xb-${esc(b.kind)}" data-kind="${esc(b.kind)}" data-label="${esc(b.kind === 'mask' ? '' : b.label)}" title="${esc(b.tip || b.label)}"
        style="left:${(b.x * 100).toFixed(3)}%;top:${(b.y * 100).toFixed(3)}%;width:${(b.w * 100).toFixed(3)}%;height:${(b.h * 100).toFixed(3)}%;font-size:${fs.toFixed(1)}px">${b.kind === 'mask' ? esc(b.label) : ''}</div>`;
    }).join('');
  }

  // ---------- анимация ----------
  async function play() {
    if (!cur.data) return;
    const my = ++runId;
    const boxes = [...document.querySelectorAll('#xr-layer .xb')];
    const by = k => boxes.filter(b => b.dataset.kind === k);
    const alive = () => my === runId && modal.classList.contains('show');
    boxes.forEach(b => b.classList.remove('on'));
    stage().classList.remove('xr-cloud'); setCloudButton();
    const wave = async (list, text, gap) => {
      if (!list.length || !alive()) return;
      caption(text);
      for (const b of list) { if (!alive()) return; b.classList.add('on'); await sleep(gap); }
      await sleep(450);
    };
    caption('Reading the page locally…'); await sleep(500);
    await wave(by('field'), 'Fields found by the rules and checked by code: issuer, number, dates, amount, bank account', 260);
    await wave([...by('hidden'), ...by('inject')], 'Checking for text a person cannot see and for instructions aimed at an AI', 380);
    if (by('mask').length && alive()) {
      by('field').forEach(b => b.style.opacity = '.0');
      await wave(by('mask'), 'Guardian replaces these values with placeholders before anything reaches the cloud model', 110);
      await sleep(900);
      if (!alive()) return;
      by('mask').forEach(b => b.classList.remove('on'));
      by('field').forEach(b => b.style.opacity = '');
    }
    await wave(by('risk'), cur.data.verdict && cur.data.verdict.headline || '', 300);
    if (alive()) caption(cur.data.verdict && cur.data.verdict.headline || 'Done');
  }

  // ---------- правая панель ----------
  function renderSide(r) {
    const flags = r.flags || [], top = flags.reduce((m, f) => Math.max(m, f.severity), 0);
    const headline = top >= 3 ? 'High risk: do not pay before verifying' : top === 2 ? 'Needs a closer look' : flags.length ? 'Consistent with what is on file' : 'No consistency problems found';
    r.verdict = { headline };
    const payee = flags.some(f => f.code === 'PAYEE_CHANGED' || f.code === 'PAYEE_EXTRA' || f.code === 'CHANGE_NOTICE');
    const pages = r.pages > 1 ? `<div class="row"><button class="btn-outline" id="xr-prev" ${cur.page <= 0 ? 'disabled' : ''}>‹</button><span style="color:var(--dim)">page ${cur.page + 1} / ${r.pages}</span><button class="btn-outline" id="xr-next" ${cur.page >= r.pages - 1 ? 'disabled' : ''}>›</button></div>` : '';
    $('xr-side').innerHTML =
      `<div class="xr-verdict s${top}"><h3>${esc(headline)}</h3><div style="font-size:.8rem;color:var(--dim)">${esc(r.doc && r.doc.summary || '')}</div>` +
      flags.map(f => `<div class="xr-flag s${f.severity}"><b>${esc(f.code)}</b><div>${esc(f.message)}</div></div>`).join('') + '</div>' +
      (r.hidden_lines ? `<div class="xr-flag s3"><b>HIDDEN TEXT</b><div>${r.hidden_lines} line(s) are printed in white: invisible to a person, readable by a model.</div></div>` : '') +
      `<div class="xr-legend"><span><i style="background:#22c55e"></i>found</span><span><i style="background:#000"></i>hidden from the cloud</span><span><i style="background:#ef4444"></i>differs from file</span><span><i style="background:#f59e0b"></i>AI instruction</span><span><i style="background:#a855f7"></i>invisible text</span></div>` +
      pages +
      (payee ? `<div class="row"><button class="btn-primary" id="xr-draft"><i class="bi bi-envelope"></i> Draft verification request</button>
        <button class="btn-outline" id="xr-confirm" title="Use only after you verified the account through a channel you trust (a call to a number from your own records)"><i class="bi bi-telephone-check"></i> Confirmed by phone: trust this account</button></div>` : '') +
      '<div id="xr-draftbox"></div>';
    const prev = $('xr-prev'), next = $('xr-next');
    if (prev) prev.onclick = () => openXray(cur.id, cur.page - 1);
    if (next) next.onclick = () => openXray(cur.id, cur.page + 1);
    const c = $('xr-confirm'); if (c) c.onclick = confirmPayee;
    const d = $('xr-draft'); if (d) d.onclick = draftRequest;
  }

  async function confirmPayee() {
    const r = await lib('payee_confirm', { id: cur.id });
    if (!r.ok) { toast(r.error || 'Could not confirm', false); return; }
    toast('Account recorded as confirmed. Checks were recalculated.');
    if (window.loadSide) loadSide();
    if (window.loadList) loadList();
    openXray(cur.id, cur.page);
  }

  // Человек остаётся в контуре: черновик → правка → Approve. Ничего никуда не отправляется.
  async function draftRequest() {
    const box = $('xr-draftbox'), d = cur.data.doc || {};
    box.innerHTML = '<div class="empty">Drafting through the Guardian gateway…</div>';
    const up = await post('/api/upload', { paths: [d.filename] });
    if (!up.ok) { box.innerHTML = `<div class="pii-alert">${esc(up.error || 'Could not read the document')}</div>`; return; }
    const instruction = 'Write a short, polite email to ' + (d.company || 'the vendor') + ' asking them to confirm that their bank details have changed' +
      (d.doc_number ? ' (invoice ' + d.doc_number + ')' : '') + '. Say that we will not pay to the new account until we confirm it by phone using a contact from our own records, ' +
      'not one from this document. Do not repeat any account numbers.';
    const r = await post('/api/draft', { instruction });
    if (!r.ok) { box.innerHTML = `<div class="pii-alert">${esc(r.error || 'Draft failed')}</div>`; return; }
    box.innerHTML = `<h2 style="margin-top:6px">Draft (nothing is sent)</h2><textarea id="xr-draft-text" rows="9">${esc(r.draft)}</textarea>
      <div class="row mt8"><button class="btn-success" id="xr-approve"><i class="bi bi-check-lg"></i> Approve</button>
      <button class="btn-outline" id="xr-copy"><i class="bi bi-clipboard"></i> Copy</button><span id="xr-draft-st" style="color:var(--dim);font-size:.8rem"></span></div>`;
    $('xr-approve').onclick = async () => {
      const a = await post('/api/edit', { draft: $('xr-draft-text').value });
      $('xr-draft-st').textContent = a.ok ? 'Approved by you. Copy it into your own mail client.' : (a.error || 'error');
    };
    $('xr-copy').onclick = () => { navigator.clipboard && navigator.clipboard.writeText($('xr-draft-text').value); toast('Copied'); };
  }

  // ---------- интеграция с index.html ----------
  window.openXray = openXray;
  window.riskBadge = d => d && d.risk >= 3 ? '<span class="badge b-err" title="Consistency check failed">risk</span>'
                        : d && d.risk === 2 ? '<span class="badge b-warn" title="Needs a closer look">check</span>' : '';

  const basePick = window.pick;
  if (typeof basePick === 'function') {
    window.pick = async function (id) {
      await basePick(id);
      // DOCS объявлен через let в index.html: свойством window он не является, но по имени виден из любого скрипта страницы
      const d = (typeof DOCS !== 'undefined' ? DOCS : []).find(x => x.id === id);
      const det = $('detail'), h2 = det && det.querySelector('h2');
      if (!d || !h2) return;
      const old = det.querySelector('.xr-ins'); if (old) old.remove();
      const f = await lib('doc_flags', { id });
      const flags = f.ok ? f.flags : [];
      const isPdf = /\.pdf$/i.test(d.filename || '');
      const div = document.createElement('div');
      div.className = 'xr-ins';
      div.innerHTML = flags.map(x => `<div class="xr-flag s${x.severity}"><b>${esc(x.code)}</b><div>${esc(x.message)}</div></div>`).join('') +
        (isPdf ? `<div class="row" style="margin-top:8px"><button class="btn-primary" onclick="openXray(${Number(id)})"><i class="bi bi-search"></i> Open X-ray</button></div>` : '');
      if (div.innerHTML) h2.insertAdjacentElement('afterend', div);
    };
  }
})();