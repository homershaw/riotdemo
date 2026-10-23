'use strict';
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let status = null, regUnit = null, regRows = [], regReads = [];

function toast(msg, bad) {
  const t = $('toast');
  t.textContent = msg;
  t.style.background = bad ? 'var(--bad)' : '';
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.hidden = true, bad ? 6000 : 2500);
}

async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  const type = r.headers.get('content-type') || '';
  const body = type.includes('json') ? await r.json() : await r.text();
  if (!r.ok) throw new Error(body.error || body || r.statusText);
  return body;
}
const form = obj => new URLSearchParams(obj);
const post = (path, obj) => api(path, {method: 'POST', body: form(obj)});

// ---------- routing ----------
const pages = {dash: loadDash, regs: loadRegs, devices: loadDevices, profiles: loadProfiles, sim: loadSim, settings: loadConfig};
function route() {
  const page = (location.hash || '#dash').slice(1);
  for (const s of document.querySelectorAll('main > section')) s.hidden = s.id !== page;
  for (const a of document.querySelectorAll('nav a')) a.classList.toggle('on', a.hash === '#' + page);
  (pages[page] || loadDash)().catch(e => toast(e.message, true));
}
window.addEventListener('hashchange', route);

// ---------- dashboard ----------
const dl = pairs => pairs.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join('');
function ago(ms) { return ms == null ? 'never' : ms < 1000 ? `${ms} ms ago` : `${(ms / 1000).toFixed(1)} s ago`; }

function showPolled(ageMs) {
  const p = $('poll'), live = ageMs != null && ageMs < 5000;
  p.className = 'pill' + (live ? ' good' : '');
  p.textContent = live ? 'POLLED' : ageMs == null ? 'not polled' : `idle ${Math.round(ageMs / 1000)} s`;
}

async function loadDash() {
  status = await api('/api/status');
  const s = status, m = s.modbus;
  $('net').innerHTML = dl([
    ['Wi-Fi', esc(s.wifi.state) + (s.wifi.ap ? ' + setup AP' : '')],
    ['Network', s.wifi.network ? esc(s.wifi.network) + (s.wifi.pairedRiot ? ' <span class="tag">RIOT pairing</span>' : '') : '-'],
    ['Saved network', esc(s.wifi.ssid)],
    ['IP', esc(s.wifi.ip || '-')], ['RSSI', s.wifi.rssi ? s.wifi.rssi + ' dBm' : '-'],
    ['Host', esc(s.name) + '.local'], ['Uptime', Math.floor(s.uptimeS / 3600) + ' h ' + Math.floor(s.uptimeS % 3600 / 60) + ' min'],
    ['Free heap', `${(s.heap / 1024).toFixed(0)} KB (min ${(s.minHeap / 1024).toFixed(0)})`]]);
  $('mb').innerHTML = dl([
    ['Port', m.port], ['Clients', `${m.clients} now, ${m.totalClients} total` + (m.rejectedClients ? `, ${m.rejectedClients} refused` : '')],
    ['Requests', m.requests], ['Replies', m.replies], ['Exceptions', `<span class="${m.exceptions ? 'bad' : ''}">${m.exceptions}</span>` +
      ` (01: ${m.exceptionsByCode[1]}, 02: ${m.exceptionsByCode[2]}, 03: ${m.exceptionsByCode[3]})`],
    ['No reply', `${m.silent} (unknown unit / broadcast)`],
    ['Last', m.requests ? `${esc(m.lastClient)} unit ${m.lastUnit} FC${m.lastFc} @${m.lastAddress}` +
      (m.lastException ? ` <span class="bad">exception ${m.lastException}</span>` : ' OK') + `, ${ago(m.lastAgeMs)}` : 'none']]);
  const fcs = Object.entries(m.byFc);
  $('byfc').innerHTML = fcs.length ? fcs.map(([fc, n]) => `<span class="tag">FC${fc}: ${n}</span>`).join(' ') : 'none yet';
  $('dashdev').innerHTML = s.devices.map(d => `<div>Unit <b>${d.unit}</b> ${esc(d.model)} ` +
    (d.loaded ? '' : '<span class="bad">not loaded</span> ') + (d.enabled ? '' : '<span class="muted">disabled</span>') + '</div>').join('') || 'none';
  fillUnits();
}

function fillUnits() {
  if (!status) return;
  $('name').textContent = status.name;
  $('fw').textContent = status.firmware;
  showPolled(status.modbus.lastAgeMs);
  for (const id of ['regUnit', 'simUnit']) {
    const sel = $(id), keep = sel.value;
    sel.innerHTML = status.devices.filter(d => d.loaded).map(d => `<option value="${d.unit}">${d.unit}: ${esc(d.model)}</option>`).join('');
    if (keep) sel.value = keep;
  }
}

// ---------- registers ----------
function fmt(t, v) {
  if (v == null) return '-';
  if (t.type === 'bool') return v ? 'ON' : 'OFF';
  return t.type === 'float32' ? Number(v.toPrecision(7)) : +v.toFixed(4);
}
const hex = w => w.toString(16).padStart(4, '0').toUpperCase();

async function loadRegs() {
  if (!status) status = await api('/api/status');
  fillUnits();
  regUnit = +$('regUnit').value;
  if (!regUnit) { $('regTable').innerHTML = '<tr><td>No loaded devices</td></tr>'; return; }
  const d = await api('/api/values/' + regUnit);
  regRows = d.tags;
  regReads = d.tags.map(t => t.reads);
  $('regInfo').textContent = d.model + (d.frozen ? ' (frozen)' : '');
  $('regTable').innerHTML = '<tr><th>Tag</th><th>FC</th><th>Wire</th><th>Value</th><th>Raw</th><th>Sim</th><th>Reads</th><th>Poll</th><th></th></tr>' +
    d.tags.map((t, i) => `<tr id="r${i}"><td title="${esc(t.id)}">${esc(t.name)}</td><td>${t.fc || '-'}</td>` +
      `<td class="num">${t.mapped ? t.wire : '<span class="bad">unmapped</span>'}</td>` +
      `<td class="num val" data-i="${i}">${fmt(t, t.value)} <span class="muted">${esc(t.units)}</span></td>` +
      `<td class="muted">${t.raw.map(hex).join(' ')}</td><td id="s${i}">${t.overridden ? '<span class="ov">override</span>' : esc(t.sim)}</td>` +
      `<td class="num" id="c${i}">${t.reads}</td><td class="num">${t.pollMs ? (t.pollMs / 1000).toFixed(1) + ' s' : '-'}</td>` +
      `<td>${t.overridden ? `<button data-resume="${esc(t.id)}">Resume</button>` : ''}</td></tr>`).join('');
}

function liveValues(msg) {
  if (msg.unit !== regUnit || $('regs').hidden || msg.v.length !== regRows.length) return;
  msg.v.forEach((v, i) => {
    const t = regRows[i], cell = document.querySelector(`td.val[data-i="${i}"]`);
    if (!cell) return;
    cell.firstChild.textContent = fmt(t, v) + ' ';
    const wasOv = t.overridden;
    t.overridden = !!msg.o[i];
    if (wasOv !== t.overridden) loadRegs();  // resume button and sim column change
    if (msg.r[i] !== regReads[i]) {
      regReads[i] = msg.r[i];
      $('c' + i).textContent = msg.r[i];
      const row = $('r' + i);
      row.classList.remove('read'); void row.offsetWidth; row.classList.add('read');
    }
  });
}

$('regUnit').onchange = () => loadRegs().catch(e => toast(e.message, true));
$('regTable').onclick = async e => {
  try {
    const resume = e.target.dataset.resume;
    if (resume) { await post(`/api/values/${regUnit}/resume`, {tag: resume}); toast('Resumed ' + resume); return setTimeout(loadRegs, 300); }
    const cell = e.target.closest('td.val');
    if (!cell) return;
    const t = regRows[+cell.dataset.i];
    if (!t.mapped) return;
    let value;
    if (t.type === 'bool') value = t.value ? 0 : 1;
    else {
      const v = prompt(`New value for ${t.name}${t.units ? ' (' + t.units + ')' : ''}`, t.value);
      if (v === null || v.trim() === '') return;
      value = v.trim();
    }
    await post('/api/values/' + regUnit, {tag: t.id, value});
    toast(`${t.name} overridden`);
    setTimeout(loadRegs, 300);
  } catch (err) { toast(err.message, true); }
};
$('resumeAll').onclick = async () => {
  try { await post(`/api/values/${regUnit}/resume`, {tag: '*'}); toast('All tags back on simulation'); setTimeout(loadRegs, 300); }
  catch (e) { toast(e.message, true); }
};

// ---------- devices ----------
async function loadDevices() {
  const [d, p] = await Promise.all([api('/api/devices'), api('/api/profiles')]);
  $('devRunning').innerHTML = '<table><tr><th>Unit</th><th>Profile</th><th>Model</th><th>Tags</th><th>Sim</th><th>Notes</th></tr>' +
    d.devices.map(x => `<tr><td>${x.unit}</td><td>${esc(x.profile)}</td><td>${esc(x.model || '')}</td>` +
      `<td>${x.status === 'ok' ? `${x.mapped}/${x.tags}` : '-'}</td><td>${esc(x.sim || '-')}</td><td>` +
      (x.status !== 'ok' ? `<span class="bad">${esc(x.message)}</span>` : '') + (x.enabled ? '' : ' disabled') +
      (x.warnings && x.warnings.length ? ` <span class="ov">${x.warnings.length} warning(s)</span>` : '') +
      (x.simMessage ? ` <span class="bad">${esc(x.simMessage)}</span>` : '') + '</td></tr>').join('') + '</table>';
  $('devSaved').innerHTML = '<table><tr><th>Unit</th><th>Profile</th><th>Sim</th><th>Enabled</th><th></th></tr>' +
    d.saved.map(x => `<tr><td>${x.unit}</td><td>${esc(x.profile)}</td><td>${esc(x.sim || '-')}</td><td>${x.enabled ? 'yes' : 'no'}</td>` +
      `<td><button data-del="${x.unit}">Remove</button></td></tr>`).join('') + '</table>';
  $('devProfile').innerHTML = p.profiles.filter(x => x.status === 'ok').map(x => `<option>${esc(x.file)}</option>`).join('');
}
$('devForm').onsubmit = async e => {
  e.preventDefault();
  const f = e.target;
  try {
    await post('/api/devices', {action: 'set', unit: f.unit.value, profile: f.profile.value, sim: f.sim.value, enabled: f.enabled.checked ? 1 : 0});
    toast('Saved. Reboot to apply.');
    loadDevices();
  } catch (err) { toast(err.message, true); }
};
$('devSaved').onclick = async e => {
  const unit = e.target.dataset.del;
  if (!unit || !confirm(`Remove unit ${unit} from the device map?`)) return;
  try { await post('/api/devices', {action: 'delete', unit}); toast('Removed. Reboot to apply.'); loadDevices(); }
  catch (err) { toast(err.message, true); }
};
async function reboot() {
  if (!confirm('Reboot the test unit now?')) return;
  try { await api('/api/reboot', {method: 'POST'}); toast('Rebooting...'); setTimeout(() => location.reload(), 8000); }
  catch (e) { toast(e.message, true); }
}
$('devReboot').onclick = reboot;

// ---------- profiles ----------
function validationHtml(v) {
  if (v.status !== 'ok') return `<p class="bad">${esc(v.message)}</p>`;
  return `<p>${esc(v.vendor)} ${esc(v.model)}: ${v.mapped} of ${v.tags} tags served, address basis <b>${esc(v.addressMode)}</b></p>` +
    (v.warnings.length ? '<ul class="warn">' + v.warnings.map(w => `<li>${esc(w)}</li>`).join('') + '</ul>' : '<p class="good">No warnings.</p>') +
    '<div class="scroll"><table><tr><th>Tag</th><th>FC</th><th>Register</th><th>Wire</th><th>Space</th><th>Type</th><th>Scale</th><th>Units</th><th>Write</th></tr>' +
    v.table.map(t => `<tr><td title="${esc(t.name)}">${esc(t.id)}</td><td>${t.fc || '-'}</td><td class="num">${t.reg}</td>` +
      `<td class="num">${t.mapped ? t.wire : '<span class="bad">unmapped</span>'}</td><td>${esc(t.space)}</td>` +
      `<td>${esc(t.type)}${t.order ? ' ' + t.order : ''}</td><td class="num">${t.scale}</td><td>${esc(t.units)}</td>` +
      `<td>${t.writable ? 'FC' + (t.writeFc || 'any') : '-'}</td></tr>`).join('') + '</table></div>';
}
function showValidation(title, v) {
  $('valTitle').textContent = title;
  $('valBody').innerHTML = validationHtml(v);
  $('valCard').hidden = false;
  $('valCard').scrollIntoView({behavior: 'smooth'});
}
async function loadProfiles() {
  const p = await api('/api/profiles');
  $('profList').innerHTML = '<table><tr><th>File</th><th>Model</th><th>Tags</th><th>Status</th><th>Used by</th><th></th></tr>' +
    p.profiles.map(x => `<tr><td>${esc(x.file)}</td><td>${esc(x.model)}</td><td>${x.tags}</td>` +
      `<td>${x.status === 'ok' ? (x.warnings ? `<span class="ov">${x.warnings} warning(s)</span>` : '<span class="good">ok</span>') : `<span class="bad">${esc(x.message)}</span>`}</td>` +
      `<td>${x.usedBy.length ? 'unit ' + x.usedBy.join(', ') : '-'}</td>` +
      `<td><button data-val="${esc(x.file)}">Validate</button> <a class="button" href="/api/profiles/${encodeURIComponent(x.file)}">Download</a> ` +
      `<button data-rm="${esc(x.file)}">Delete</button></td></tr>`).join('') + '</table>';
}
$('profList').onclick = async e => {
  try {
    if (e.target.dataset.val) showValidation(e.target.dataset.val, await api(`/api/profiles/${encodeURIComponent(e.target.dataset.val)}/validate`));
    if (e.target.dataset.rm && confirm('Delete ' + e.target.dataset.rm + '?')) {
      await api('/api/profiles/' + encodeURIComponent(e.target.dataset.rm), {method: 'DELETE'});
      toast('Deleted');
      loadProfiles();
    }
  } catch (err) { toast(err.message, true); }
};
$('upForm').onsubmit = async e => {
  e.preventDefault();
  const r = await fetch('/api/profiles', {method: 'POST', body: new FormData(e.target)});
  const body = await r.json().catch(() => ({error: r.statusText}));
  if (body.validation) showValidation((body.saved ? 'Saved ' : 'Not saved: ') + body.file, body.validation);
  else toast(body.error || 'Upload failed', true);
  if (body.saved) { toast('Uploaded ' + body.file); e.target.reset(); loadProfiles(); }
};

// ---------- simulation ----------
async function loadSim() {
  if (!status) status = await api('/api/status');
  fillUnits();
  const unit = $('simUnit').value;
  if (!unit) return;
  $('simText').value = await api('/api/sim/' + unit);
  $('simMsg').textContent = 'Values are in engineering units. Save applies immediately; no reboot needed.';
}
$('simUnit').onchange = () => loadSim().catch(e => toast(e.message, true));
$('simLoad').onclick = () => loadSim().catch(e => toast(e.message, true));
$('simTemplate').onclick = async () => {
  if (!confirm('Replace the editor contents with a generated template? Nothing is saved until you press Save.')) return;
  try {
    $('simText').value = await api(`/api/sim/${$('simUnit').value}/template`);
    $('simMsg').textContent = 'Template loaded (not saved). Replace the PLACEHOLDER ranges with realistic limits, then Save.';
  } catch (e) { toast(e.message, true); }
};
$('simSave').onclick = async () => {
  try {
    const r = await api('/api/sim/' + $('simUnit').value, {method: 'PUT', body: $('simText').value});
    $('simMsg').innerHTML = `Saved ${esc(r.file)}: ${r.entries} entries, applied.` +
      (r.warnings.length ? '<ul class="warn">' + r.warnings.map(w => `<li>${esc(w)}</li>`).join('') + '</ul>' : '');
    toast('Simulation applied');
  } catch (e) { toast(e.message, true); }
};

// ---------- settings ----------
async function loadConfig() { $('cfgText').value = await api('/api/config'); }
$('cfgLoad').onclick = () => loadConfig().catch(e => toast(e.message, true));
$('cfgSave').onclick = async () => {
  try {
    const r = await api('/api/config', {method: 'PUT', body: $('cfgText').value});
    $('cfgMsg').innerHTML = 'Saved. Reboot to apply.' + (r.warnings.length ? '<ul class="warn">' + r.warnings.map(w => `<li>${esc(w)}</li>`).join('') + '</ul>' : '');
  } catch (e) { toast(e.message, true); }
};
$('cfgImport').onchange = async e => {
  const file = e.target.files[0];
  if (file) { $('cfgText').value = await file.text(); $('cfgMsg').textContent = 'Imported ' + file.name + ' into the editor (not saved yet).'; }
  e.target.value = '';
};
$('cfgReboot').onclick = reboot;

// ---------- live updates ----------
function connect() {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.t === 'status') showPolled(m.lastAgeMs);
    if (m.t === 'values') liveValues(m);
  };
  ws.onclose = () => setTimeout(connect, 2000);
}
connect();
setInterval(() => { if (!$('dash').hidden) loadDash().catch(() => {}); }, 3000);
api('/api/status').then(s => { status = s; fillUnits(); }).catch(() => {});
route();
