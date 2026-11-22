/* Smart Industry Monitor — dashboard (SIM-07).
 * Lê a API do backend em C: /api/machines/latest (a cada 5 s) e
 * /api/machines/{id}/history. Sem frameworks e sem CDN.
 * Todo texto vindo da API entra no DOM por textContent. */
'use strict';

const POLL_MS = 5000;
const LIMITS = { temperature: 75, vibration: 3.8 };

/* Tipos fixos do dataset (não vêm na API). */
const MACHINE_TYPES = {
  1: 'CNC', 2: 'CNC', 3: 'CNC',
  6: 'Bomba', 7: 'Bomba', 8: 'Bomba', 9: 'Bomba',
  11: 'Compressor', 12: 'Compressor', 13: 'Compressor',
};

const METRICS = [
  { key: 'motor_temperature', label: 'Temperatura', unit: '°C', digits: 1, limit: LIMITS.temperature },
  { key: 'current', label: 'Corrente', unit: 'A', digits: 1 },
  { key: 'vibration_rms', label: 'Vibração RMS', unit: '', digits: 2, limit: LIMITS.vibration,
    note: 'unidade não informada no dataset' },
  { key: 'rpm', label: 'Rotação', unit: 'rpm', digits: 0 },
];

const STATE_LABELS = { IDLE: 'Ocioso', NORMAL: 'Normal', PEAK: 'Pico', UNKNOWN: 'Desconhecido' };
const LEVEL_RANK = { critical: 4, serious: 3, warning: 2, good: 1, nodata: 0 };
const LEVEL_ICON = { critical: '✖', serious: '!', warning: '▲', good: '✓', nodata: '–' };

const WINDOWS = { 60: '1 h', 360: '6 h', 1440: '24 h' };

const state = {
  machines: [],
  lastOk: null,
  selected: null,
  windowMin: 60,
  historyKey: null,
  historyBusy: false,
  historyPending: false,
  history: null,
  showTable: false,
};

/* ---------- utilidades ---------- */

function el(tag, attrs, children) {
  const node = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') node.className = v;
      else if (k === 'text') node.textContent = v;
      else node.setAttribute(k, v === true ? '' : String(v));
    }
  }
  for (const child of children || []) {
    if (child === null || child === undefined) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

const SVG_NS = 'http://www.w3.org/2000/svg';
function svg(tag, attrs) {
  const node = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === 'text') node.textContent = v;
    else node.setAttribute(k, String(v));
  }
  return node;
}

function num(value) {
  if (value === null || value === undefined || value === '') return null;
  const n = typeof value === 'number' ? value : Number(value);
  return Number.isFinite(n) ? n : null;
}

const numberFormats = {};
function fmt(value, digits) {
  const n = num(value);
  if (n === null) return '—';
  if (!numberFormats[digits]) {
    numberFormats[digits] = new Intl.NumberFormat('pt-BR', {
      minimumFractionDigits: digits, maximumFractionDigits: digits,
    });
  }
  return numberFormats[digits].format(n);
}

function parseTime(text) {
  if (typeof text !== 'string') return null;
  const t = Date.parse(text);
  return Number.isFinite(t) ? t : null;
}

const pad = (n) => String(n).padStart(2, '0');
function fmtDateTime(ms) {
  const d = new Date(ms);
  return `${pad(d.getUTCDate())}/${pad(d.getUTCMonth() + 1)}/${d.getUTCFullYear()} ` +
         `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
}
function fmtShort(ms, withDate) {
  const d = new Date(ms);
  const hm = `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
  return withDate ? `${pad(d.getUTCDate())}/${pad(d.getUTCMonth() + 1)} ${hm}` : hm;
}
/* Formato aceito pelo backend: AAAA-MM-DDTHH:MM:SS (UTC). */
function apiTime(ms) {
  return new Date(ms).toISOString().slice(0, 19);
}

function fieldValue(machine, key) {
  const f = machine && machine[key];
  return f && typeof f === 'object' ? f.value : null;
}

function isFailure(value) {
  return value === true || num(value) === 1 || value === '1' || value === 'true';
}

function stateCode(value) {
  if (value === null || value === undefined) return null;
  return String(value).toUpperCase();
}
function stateLabel(value) {
  const code = stateCode(value);
  if (code === null) return '—';
  return STATE_LABELS[code] || String(value);
}

/* ---------- regras de alerta (decididas pela equipe) ---------- */

function latestTimestamp(machine) {
  let latest = null;
  for (const key of ['operating_state', 'motor_temperature', 'current', 'vibration_rms', 'rpm', 'failure_within_24h']) {
    const f = machine[key];
    const t = f && typeof f === 'object' ? parseTime(f.timestamp) : null;
    if (t !== null && (latest === null || t > latest)) latest = t;
  }
  return latest;
}

function alertsOf(machine) {
  const hasData = ['operating_state', 'motor_temperature', 'current', 'vibration_rms', 'rpm', 'failure_within_24h']
    .some((key) => machine[key] && typeof machine[key] === 'object');
  if (!hasData) return [{ level: 'nodata', label: 'Sem dados' }];

  const alerts = [];
  if (isFailure(fieldValue(machine, 'failure_within_24h'))) {
    alerts.push({ level: 'critical', label: 'Falha prevista (24 h)' });
  }
  const temp = num(fieldValue(machine, 'motor_temperature'));
  if (temp !== null && temp > LIMITS.temperature) alerts.push({ level: 'serious', label: 'Temperatura alta' });
  const vib = num(fieldValue(machine, 'vibration_rms'));
  if (vib !== null && vib > LIMITS.vibration) alerts.push({ level: 'serious', label: 'Vibração alta' });
  if (stateCode(fieldValue(machine, 'operating_state')) === 'PEAK') {
    alerts.push({ level: 'warning', label: 'Sobrecarga' });
  }
  if (alerts.length === 0) alerts.push({ level: 'good', label: 'Normal' });
  return alerts;
}

function topLevel(alerts) {
  return alerts.reduce((best, a) => (LEVEL_RANK[a.level] > LEVEL_RANK[best] ? a.level : best), alerts[0].level);
}

/* ---------- API ---------- */

async function getJson(url) {
  const response = await fetch(url, { cache: 'no-store' });
  let body = null;
  try { body = await response.json(); } catch (e) { body = null; }
  if (!response.ok) {
    const err = new Error((body && body.error) || `HTTP ${response.status}`);
    err.status = response.status;
    throw err;
  }
  return body;
}

/* ---------- visão ao vivo ---------- */

function setConnection(kind, text) {
  const box = document.getElementById('connection');
  box.className = `connection ${kind}`;
  document.getElementById('connection-text').textContent = text;
}

function nowLabel() {
  const d = new Date();
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

async function poll() {
  try {
    const data = await getJson('/api/machines/latest');
    const list = Array.isArray(data && data.machines) ? data.machines : [];
    state.machines = list
      .filter((m) => m && Number.isInteger(m.machine_id))
      .sort((a, b) => a.machine_id - b.machine_id);
    state.lastOk = Date.now();
    setConnection('ok', `Atualizado às ${nowLabel()}`);
    renderLive(false);
    maybeRefreshHistory();
  } catch (err) {
    const stale = state.machines.length > 0;
    const reason = err.status === 502 ? 'InterSCity indisponível' : 'Sem conexão com o backend';
    setConnection(stale ? 'stale' : 'down', reason + (stale ? ' · mostrando últimos dados' : ''));
    renderLive(true, err.status === 502
      ? 'O backend não conseguiu consultar o InterSCity. Tentando de novo a cada 5 s.'
      : 'O backend não respondeu. Confira se ele está rodando. Tentando de novo a cada 5 s.');
  } finally {
    setTimeout(poll, POLL_MS);
  }
}

function renderLive(stale, errorText) {
  const grid = document.getElementById('grid');
  const counts = { good: 0, alert: 0, critical: 0, nodata: 0 };
  const cards = [];

  for (const machine of state.machines) {
    const alerts = alertsOf(machine);
    const level = topLevel(alerts);
    if (level === 'good') counts.good++;
    else if (level === 'critical') counts.critical++;
    else if (level === 'nodata') counts.nodata++;
    else counts.alert++;
    cards.push(machineCard(machine, alerts, level, stale));
  }
  grid.replaceChildren(...cards);
  const neverLoaded = state.lastOk === null;
  if (cards.length === 0) {
    grid.append(el('p', { class: 'message wide', text: neverLoaded && errorText
      ? errorText : 'Nenhuma máquina configurada no backend.' }));
  }

  const show = (id, n) => { document.getElementById(id).textContent = neverLoaded ? '–' : String(n); };
  show('sum-total', state.machines.length);
  show('sum-good', counts.good);
  show('sum-alert', counts.alert);
  show('sum-critical', counts.critical);
  show('sum-nodata', counts.nodata);
}

function machineCard(machine, alerts, level, stale) {
  const id = machine.machine_id;
  const type = MACHINE_TYPES[id] || '—';
  const latest = latestTimestamp(machine);

  const badges = alerts.map((a) => el('span', { class: 'badge' }, [
    el('span', { class: `icon ${a.level}`, 'aria-hidden': 'true', text: LEVEL_ICON[a.level] }),
    a.label,
  ]));

  const metrics = METRICS.map((m) => {
    const value = num(fieldValue(machine, m.key));
    const over = m.limit !== undefined && value !== null && value > m.limit;
    return el('div', { class: over ? 'metric over' : 'metric' }, [
      el('dt', { text: m.label }),
      el('dd', null, [fmt(value, m.digits), m.unit && value !== null ? el('span', { class: 'unit', text: m.unit }) : null]),
    ]);
  });
  metrics.push(el('div', { class: 'metric' }, [
    el('dt', { text: 'Estado' }),
    el('dd', { text: stateLabel(fieldValue(machine, 'operating_state')) }),
  ]));
  const failure = fieldValue(machine, 'failure_within_24h');
  metrics.push(el('div', { class: isFailure(failure) ? 'metric over' : 'metric' }, [
    el('dt', { text: 'Falha em 24 h' }),
    el('dd', { text: failure === null || failure === undefined ? '—' : (isFailure(failure) ? 'Sim' : 'Não') }),
  ]));

  const card = el('button', {
    type: 'button',
    class: `card level-${level}${stale ? ' stale' : ''}`,
    'aria-pressed': state.selected === id ? 'true' : 'false',
    'data-machine': id,
    'aria-label': `Máquina ${id}, ${type}: ${alerts.map((a) => a.label).join(', ')}. Abrir histórico.`,
  }, [
    el('div', { class: 'card-head' }, [
      el('span', { class: 'card-title', text: `Máquina ${id}` }),
      el('span', { class: 'card-type', text: type }),
    ]),
    el('div', { class: 'alerts' }, badges),
    el('dl', { class: 'metrics' }, metrics),
    el('div', { class: 'card-foot', text: latest === null ? 'Sem leituras no InterSCity' : `Último dado: ${fmtDateTime(latest)} UTC` }),
  ]);
  card.addEventListener('click', () => openHistory(id));
  return card;
}

/* ---------- histórico ---------- */

function selectedMachine() {
  return state.machines.find((m) => m.machine_id === state.selected) || null;
}

function openHistory(id) {
  state.selected = id;
  state.historyKey = null;
  document.getElementById('history').hidden = false;
  for (const card of document.querySelectorAll('.card')) {
    card.setAttribute('aria-pressed', Number(card.dataset.machine) === id ? 'true' : 'false');
  }
  loadHistory(true);
  document.getElementById('history').scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function closeHistory() {
  state.selected = null;
  state.history = null;
  state.historyKey = null;
  document.getElementById('history').hidden = true;
  for (const card of document.querySelectorAll('.card')) card.setAttribute('aria-pressed', 'false');
}

function maybeRefreshHistory() {
  if (state.selected === null) return;
  const machine = selectedMachine();
  const latest = machine ? latestTimestamp(machine) : null;
  if (`${state.selected}|${state.windowMin}|${latest}` !== state.historyKey) loadHistory(false);
}

function showHistoryMessage(text) {
  const box = document.getElementById('history-message');
  box.hidden = !text;
  box.textContent = text || '';
}

async function loadHistory(clear) {
  if (state.historyBusy) {
    state.historyPending = true;
    return;
  }
  const id = state.selected;
  const machine = selectedMachine();
  const type = MACHINE_TYPES[id] || '';
  document.getElementById('history-title').textContent = `Máquina ${id}${type ? ' · ' + type : ''} — histórico`;
  for (const b of document.querySelectorAll('[data-window]')) {
    b.setAttribute('aria-pressed', Number(b.dataset.window) === state.windowMin ? 'true' : 'false');
  }

  const latest = machine ? latestTimestamp(machine) : null;
  state.historyKey = `${id}|${state.windowMin}|${latest}`;
  if (latest === null) {
    state.history = null;
    document.getElementById('history-range').textContent = '';
    showHistoryMessage('Esta máquina ainda não tem leituras no InterSCity.');
    document.getElementById('history-charts').replaceChildren();
    document.getElementById('history-table').replaceChildren();
    document.getElementById('history-table').hidden = true;
    return;
  }

  const end = latest + 1000;
  const start = latest - state.windowMin * 60 * 1000;
  document.getElementById('history-range').textContent =
    `Últimas ${WINDOWS[state.windowMin]} até o dado mais recente: ${fmtDateTime(start)} → ${fmtDateTime(latest)} UTC`;
  if (clear) {
    showHistoryMessage('Carregando histórico…');
    document.getElementById('history-charts').replaceChildren();
  }

  state.historyBusy = true;
  try {
    const url = `/api/machines/${encodeURIComponent(id)}/history?start=${apiTime(start)}&end=${apiTime(end)}`;
    const data = await getJson(url);
    if (state.selected !== id) return;
    state.history = { data, start, end: latest };
    showHistoryMessage('');
    renderHistory();
  } catch (err) {
    if (state.selected !== id) return;
    state.historyKey = null;
    if (err.status === 413) showHistoryMessage('Janela grande demais para uma consulta. Escolha uma janela menor.');
    else if (err.status === 502) showHistoryMessage('InterSCity indisponível. Tentando de novo na próxima atualização.');
    else if (err.status === 404) showHistoryMessage('Máquina não encontrada no backend.');
    else showHistoryMessage(`Não foi possível carregar o histórico (${err.message}).`);
  } finally {
    state.historyBusy = false;
    if (state.historyPending) {
      state.historyPending = false;
      if (state.selected !== null) loadHistory(true);
    }
  }
}

function seriesPoints(series, key, numeric) {
  const raw = series && Array.isArray(series[key]) ? series[key] : [];
  const points = [];
  for (const p of raw) {
    const t = parseTime(p && p.timestamp);
    if (t === null) continue;
    const v = numeric ? num(p.value) : p.value;
    if (numeric && v === null) continue;
    points.push({ t, v });
  }
  points.sort((a, b) => a.t - b.t);
  return points;
}

function renderHistory() {
  const charts = document.getElementById('history-charts');
  const h = state.history;
  if (!h) return;
  const series = (h.data && h.data.series) || {};
  const x0 = h.start;
  const x1 = Math.max(h.end, h.start + 60000);
  const withDate = state.windowMin > 360;

  const blocks = [];
  for (const m of METRICS) {
    const points = seriesPoints(series, m.key, true);
    const parts = [m.note || m.unit];
    if (m.limit !== undefined) {
      parts.push(`linha tracejada = limite de alerta (${fmt(m.limit, m.digits)}${m.unit ? ' ' + m.unit : ''})`);
    }
    blocks.push(chartBlock(m.label, parts.join(' · '),
      (w) => lineChart(points, { x0, x1, withDate, limit: m.limit, unit: m.unit, digits: m.digits, label: m.label }, w)));
  }

  const statePoints = seriesPoints(series, 'operating_state', false);
  blocks.push(chartBlock('Estado de operação', 'PEAK = sobrecarga', (w) => bandChart(statePoints, {
    x0, x1, withDate,
    classify: (v) => {
      const code = stateCode(v);
      if (code === 'PEAK') return { color: 'var(--warning)', label: 'Pico (sobrecarga)' };
      if (code === 'NORMAL') return { color: 'var(--state-normal)', label: 'Normal' };
      if (code === 'IDLE') return { color: 'var(--state-idle)', label: 'Ocioso' };
      return { color: 'var(--state-unknown)', label: v === null || v === undefined ? 'Desconhecido' : String(v) };
    },
    legend: [['var(--state-normal)', 'Normal'], ['var(--state-idle)', 'Ocioso'], ['var(--warning)', 'Pico (sobrecarga)'], ['var(--state-unknown)', 'Desconhecido']],
  }, w), true));

  const failurePoints = seriesPoints(series, 'failure_within_24h', false);
  blocks.push(chartBlock('Falha prevista em 24 h', 'rótulo do dataset', (w) => bandChart(failurePoints, {
    x0, x1, withDate,
    classify: (v) => (isFailure(v)
      ? { color: 'var(--critical)', label: 'Falha prevista' }
      : { color: 'var(--state-unknown)', label: 'Sem falha prevista' }),
    legend: [['var(--state-unknown)', 'Sem falha prevista'], ['var(--critical)', 'Falha prevista']],
  }, w), true));

  charts.replaceChildren(...blocks);
  drawCharts();
  renderTable(series);
}

/* O conteúdo é desenhado depois de o bloco entrar na página, na largura real. */
function chartBlock(title, subtitle, draw, wide) {
  const slot = el('div', { class: 'chart-slot' });
  const block = el('div', { class: wide ? 'chart wide' : 'chart' }, [
    el('h3', { text: title }),
    el('p', { class: 'chart-sub', text: subtitle }),
    slot,
  ]);
  block.draw = () => slot.replaceChildren(draw(Math.max(280, Math.round(slot.clientWidth))));
  return block;
}

function drawCharts() {
  for (const block of document.querySelectorAll('#history-charts .chart')) {
    if (block.draw) block.draw();
  }
}

/* Escala "bonita" para o eixo Y. */
function niceTicks(min, max, count) {
  if (min === max) { min -= 1; max += 1; }
  const span = max - min;
  const step0 = span / count;
  const mag = Math.pow(10, Math.floor(Math.log10(step0)));
  const norm = step0 / mag;
  const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 2.5 ? 2.5 : norm <= 5 ? 5 : 10) * mag;
  const lo = Math.floor(min / step) * step;
  const hi = Math.ceil(max / step) * step;
  const ticks = [];
  for (let v = lo; v <= hi + step / 2; v += step) ticks.push(Number(v.toFixed(10)));
  return ticks;
}

function timeTicks(x0, x1) {
  const span = x1 - x0;
  const steps = [5, 10, 15, 30, 60, 120, 180, 360, 720].map((m) => m * 60000);
  const step = steps.find((s) => span / s <= 6) || 1440 * 60000;
  const ticks = [];
  for (let t = Math.ceil(x0 / step) * step; t <= x1; t += step) ticks.push(t);
  return ticks;
}

const PAD = { l: 46, r: 14, t: 10, b: 24 };

function lineChart(points, opt, W) {
  if (points.length === 0) return el('div', { class: 'empty', text: 'Sem dados nesta janela' });
  const H = 190;
  const values = points.map((p) => p.v);
  let lo = Math.min(...values);
  let hi = Math.max(...values);
  if (opt.limit !== undefined) { lo = Math.min(lo, opt.limit); hi = Math.max(hi, opt.limit); }
  const yTicks = niceTicks(lo, hi, 4);
  const y0 = yTicks[0];
  const y1 = yTicks[yTicks.length - 1];
  const sx = (t) => PAD.l + ((t - opt.x0) / (opt.x1 - opt.x0)) * (W - PAD.l - PAD.r);
  const sy = (v) => PAD.t + (1 - (v - y0) / (y1 - y0)) * (H - PAD.t - PAD.b);

  const root = svg('svg', { viewBox: `0 0 ${W} ${H}`, width: W, height: H, role: 'img', 'aria-label': `${opt.label}: ${points.length} leituras` });
  const axis = svg('g', { class: 'axis' });
  for (const v of yTicks) {
    root.append(svg('line', { class: v === y0 ? 'baseline' : 'gridline', x1: PAD.l, x2: W - PAD.r, y1: sy(v), y2: sy(v) }));
    axis.append(svg('text', { x: PAD.l - 8, y: sy(v) + 4, 'text-anchor': 'end', text: fmt(v, Math.abs(y1 - y0) < 5 ? 1 : 0) }));
  }
  for (const t of timeTicks(opt.x0, opt.x1)) {
    axis.append(svg('text', { x: sx(t), y: H - 6, 'text-anchor': 'middle', text: fmtShort(t, opt.withDate) }));
  }
  root.append(axis);

  if (opt.limit !== undefined) {
    const ly = sy(opt.limit);
    root.append(svg('line', { class: 'limit-line', x1: PAD.l, x2: W - PAD.r, y1: ly, y2: ly }));
    root.append(svg('text', { class: 'limit-label', x: W - PAD.r, y: ly - 5, 'text-anchor': 'end',
      text: `limite ${fmt(opt.limit, opt.digits)}${opt.unit ? ' ' + opt.unit : ''}` }));
  }

  /* Quebra a linha só em buracos maiores que 3x o intervalo típico (mediana). */
  const steps = points.slice(1).map((p, i) => p.t - points[i].t).sort((a, b) => a - b);
  const typical = steps.length ? steps[steps.length >> 1] : 0;
  const maxGap = Math.max(3 * typical, 9 * 60000);
  let d = '';
  points.forEach((p, i) => {
    const gap = i > 0 && p.t - points[i - 1].t > maxGap;
    d += `${i === 0 || gap ? 'M' : 'L'}${sx(p.t).toFixed(1)},${sy(p.v).toFixed(1)}`;
  });
  root.append(svg('path', { class: 'series-line', d }));
  /* Poucas leituras na largura: cada uma ganha um marcador. */
  if (points.length <= (W - PAD.l - PAD.r) / 14) {
    for (const p of points) {
      root.append(svg('circle', { class: 'single-point', cx: sx(p.t), cy: sy(p.v), r: 4 }));
    }
  }
  if (opt.limit !== undefined) {
    for (const p of points) {
      if (p.v > opt.limit) root.append(svg('circle', { class: 'over-point', cx: sx(p.t), cy: sy(p.v), r: 4 }));
    }
  }

  /* Camada de hover: linha vertical que encosta no ponto mais próximo. */
  const cross = svg('line', { class: 'crosshair', y1: PAD.t, y2: H - PAD.b, visibility: 'hidden' });
  const dot = svg('circle', { class: 'hover-dot', r: 4.5, visibility: 'hidden' });
  const hit = svg('rect', { x: PAD.l, y: 0, width: W - PAD.l - PAD.r, height: H, fill: 'transparent' });
  root.append(cross, dot, hit);
  const hide = () => { cross.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden'); hideTooltip(); };
  hit.addEventListener('pointermove', (ev) => {
    const box = root.getBoundingClientRect();
    const x = ((ev.clientX - box.left) / box.width) * W;
    const t = opt.x0 + ((x - PAD.l) / (W - PAD.l - PAD.r)) * (opt.x1 - opt.x0);
    const p = nearest(points, t);
    const px = sx(p.t);
    cross.setAttribute('x1', px); cross.setAttribute('x2', px); cross.setAttribute('visibility', 'visible');
    dot.setAttribute('cx', px); dot.setAttribute('cy', sy(p.v)); dot.setAttribute('visibility', 'visible');
    const over = opt.limit !== undefined && p.v > opt.limit ? ' · acima do limite' : '';
    showTooltip(ev.clientX, ev.clientY, `${fmtDateTime(p.t)} UTC`, `${opt.label}: ${fmt(p.v, opt.digits)}${opt.unit ? ' ' + opt.unit : ''}${over}`);
  });
  hit.addEventListener('pointerleave', hide);
  return root;
}

function nearest(points, t) {
  let lo = 0;
  let hi = points.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (points[mid].t < t) lo = mid + 1; else hi = mid;
  }
  if (lo > 0 && Math.abs(points[lo - 1].t - t) < Math.abs(points[lo].t - t)) lo -= 1;
  return points[lo];
}

/* Faixa de categorias no tempo (estado e falha). Cada leitura vale até a próxima. */
function bandChart(points, opt, W) {
  if (points.length === 0) return el('div', { class: 'empty', text: 'Sem dados nesta janela' });
  const H = 58;
  const bandY = 6;
  const bandH = 26;
  const sx = (t) => PAD.l + ((t - opt.x0) / (opt.x1 - opt.x0)) * (W - PAD.l - PAD.r);
  const root = svg('svg', { viewBox: `0 0 ${W} ${H}`, width: W, height: H, role: 'img', 'aria-label': `${points.length} leituras` });

  /* Leituras seguidas na mesma categoria viram um único trecho. */
  const runs = [];
  for (const p of points) {
    const cls = opt.classify(p.v);
    const last = runs[runs.length - 1];
    if (last && last.cls.label === cls.label) last.last = p.t;
    else runs.push({ cls, first: p.t, last: p.t });
  }
  const tail = Math.min(points[points.length - 1].t + 3 * 60000, opt.x1);
  runs.forEach((run, i) => {
    const until = i + 1 < runs.length ? runs[i + 1].first : tail;
    const a = sx(Math.max(run.first, opt.x0));
    const b = Math.max(sx(Math.min(until, opt.x1)), a + 2);
    const rect = svg('rect', { x: a, y: bandY, width: Math.max(b - a - 2, 1.5), height: bandH, rx: 2, fill: run.cls.color });
    const range = run.first === run.last
      ? `${fmtDateTime(run.first)} UTC`
      : `${fmtDateTime(run.first)} → ${fmtShort(run.last, true)} UTC`;
    rect.addEventListener('pointermove', (ev) => showTooltip(ev.clientX, ev.clientY, range, run.cls.label));
    rect.addEventListener('pointerleave', hideTooltip);
    root.append(rect);
  });
  const axis = svg('g', { class: 'axis' });
  for (const t of timeTicks(opt.x0, opt.x1)) {
    axis.append(svg('text', { x: sx(t), y: H - 6, 'text-anchor': 'middle', text: fmtShort(t, opt.withDate) }));
  }
  root.append(axis);

  const legend = el('div', { class: 'band-legend' }, opt.legend.map(([color, label]) =>
    el('span', null, [el('i', { class: 'swatch', style: `background:${color}` }), label])));
  return el('div', null, [root, legend]);
}

function renderTable(series) {
  const wrap = document.getElementById('history-table');
  const rows = new Map();
  const cols = [
    ['motor_temperature', 'Temperatura (°C)', 1], ['current', 'Corrente (A)', 1],
    ['vibration_rms', 'Vibração RMS', 2], ['rpm', 'Rotação (rpm)', 0],
    ['operating_state', 'Estado', null], ['failure_within_24h', 'Falha em 24 h', null],
  ];
  for (const [key] of cols) {
    for (const p of seriesPoints(series, key, false)) {
      if (!rows.has(p.t)) rows.set(p.t, {});
      rows.get(p.t)[key] = p.v;
    }
  }
  const times = [...rows.keys()].sort((a, b) => b - a);
  const head = el('tr', null, [el('th', { text: 'Horário (UTC)' }), ...cols.map(([, label]) => el('th', { text: label }))]);
  const body = times.map((t) => {
    const r = rows.get(t);
    return el('tr', null, [el('td', { text: fmtDateTime(t) }), ...cols.map(([key, , digits]) => {
      const v = r[key];
      let text = '—';
      if (v !== undefined && v !== null) {
        if (key === 'operating_state') text = stateLabel(v);
        else if (key === 'failure_within_24h') text = isFailure(v) ? 'Sim' : 'Não';
        else text = fmt(v, digits);
      }
      return el('td', { text });
    })]);
  });
  wrap.replaceChildren(el('table', null, [el('thead', null, [head]), el('tbody', null, body)]));
  if (times.length === 0) wrap.replaceChildren(el('p', { class: 'message', text: 'Sem leituras nesta janela.' }));
  wrap.hidden = !state.showTable;
}

/* ---------- tooltip ---------- */

function showTooltip(x, y, title, text) {
  const tip = document.getElementById('tooltip');
  tip.replaceChildren(el('strong', { text: title }), el('span', { text }));
  tip.hidden = false;
  const w = tip.offsetWidth;
  const left = x + 14 + w > window.innerWidth ? x - 14 - w : x + 14;
  tip.style.left = `${Math.max(4, left)}px`;
  tip.style.top = `${Math.max(4, y - tip.offsetHeight - 10)}px`;
}
function hideTooltip() {
  document.getElementById('tooltip').hidden = true;
}

/* ---------- início ---------- */

document.getElementById('history-close').addEventListener('click', closeHistory);
document.getElementById('history-table-toggle').addEventListener('click', (ev) => {
  state.showTable = !state.showTable;
  ev.currentTarget.setAttribute('aria-pressed', state.showTable ? 'true' : 'false');
  document.getElementById('history-table').hidden = !state.showTable;
});
for (const b of document.querySelectorAll('[data-window]')) {
  b.addEventListener('click', () => {
    state.windowMin = Number(b.dataset.window);
    loadHistory(true);
  });
}
let resizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(drawCharts, 150);
});
document.addEventListener('keydown', (ev) => {
  if (ev.key === 'Escape' && state.selected !== null) closeHistory();
});

poll();
