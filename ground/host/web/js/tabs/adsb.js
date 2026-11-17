// ADS-B: the mission, step by step from the button (SET_TIME, then SET_MODE,
// ADR-0020) to the downlink; what the satellite reports (HLR-ADS-08); and
// the aircraft the ground reconstructs, on the map, in a table and one by
// one (origin and destination by each method, ADR-0019).

import { esc, fmtAge, fmtClock, fmtNum, setHTML } from '../util.js';
import { t } from '../i18n.js';
import { serverNow } from '../state.js';
import { latestRun, ack, step, after } from '../runs.js';
import { AdsbMap } from '../map.js';

const MISSION_S = 600;      // obcd's adsb_timeout (OBC-6)
const POLL_MS = 2000;

let view = { revision: 0, aircraft: [], airports: {} };
let etag = null;
let selected = null;
let map = null;
let timer = null;

function tag(d) {
  if (!d) return '<span class="tag UNKNOWN">—</span>';
  const label = d.ident || (d.status === 'UNKNOWN' ? '?' : d.status);
  return `<span class="tag ${esc(d.status)}" title="${esc(d.status + (d.reason ? ' · ' + d.reason : ''))}">${esc(label)}</span>`;
}

async function poll() {
  try {
    const r = await fetch('/api/adsb', { headers: etag ? { 'If-None-Match': etag } : {} });
    if (r.status === 200) {
      etag = r.headers.get('ETag');
      view = await r.json();
      drawAircraft();
    }
  } catch (e) { /* the server is restarting: next poll */ }
}

function missionSteps(run) {
  if (!run) return '';
  const st = ack(run, 'SET_TIME'), sm = ack(run, 'SET_MODE');
  const failed = run.failed;
  const items = [];
  items.push(step(1, t('s_sent'), 'done', new Date(run.sent * 1000).toLocaleTimeString([], { hour12: false })));
  items.push(step(2, t('s_time'), st ? (st.status === 'OK' ? 'done' : 'fail') : failed ? 'fail' : 'now',
    st ? `${st.status} · ${st.latency_ms} ms` : failed || t('waiting')));
  items.push(step(3, t('s_mode'), sm ? (sm.status === 'OK' ? 'done' : 'fail') : st && st.status === 'OK' && !failed ? 'now' : '',
    sm ? `${sm.status} · ${sm.latency_ms} ms` : ''));
  const inMode = run.entered != null, out = run.left != null;
  items.push(step(4, t('s_entered') + ' MISSION_ADSB', inMode ? 'done' : sm && sm.status === 'OK' ? 'now' : '', after(run, run.entered)));
  items.push(step(5, t('s_dump'), run.dump1090 != null ? 'done' : inMode && !out ? 'now' : '', after(run, run.dump1090)));
  items.push(step(6, t('s_tracks'), run.first_tracks != null ? 'done' : inMode && !out ? 'now' : '',
    run.first_tracks != null ? `${after(run, run.first_tracks)} · ${run.tracks}` : ''));
  items.push(step(7, t('s_end'), out ? 'done' : '',
    out ? `${run.left_to} · ${fmtAge(run.left - run.entered)}` : ''));
  return `<ol class="steps">${items.join('')}</ol>`;
}

function drawMission(state) {
  const st = state.st;
  const run = latestRun(st, 'MISSION_ADSB');
  const running = run && run.entered != null && run.left == null;
  const el = document.getElementById('adsb-timer');
  if (running) {
    const gone = serverNow() - run.entered;
    el.innerHTML = `<span class="timer">${esc(fmtClock(MISSION_S - gone))}</span>
      <span class="muted"> ${esc(t('remaining'))} · ${esc(fmtClock(gone))} ${esc(t('elapsed'))}</span>`;
  } else {
    el.textContent = '';
  }
  setHTML(document.getElementById('adsb-steps'), missionSteps(run));
  const s = (st && st.stat) || {};
  const a = (st && st.adsb) || null;
  const p95 = s.latency_p95_ms;
  setHTML(document.getElementById('adsb-figs'), `
    <div class="fig"><span class="lbl">${esc(t('aircraft'))}</span><b>${esc(fmtNum(s.aircraft))}</b></div>
    <div class="fig"><span class="lbl">${esc(t('messages'))}</span><b>${esc(fmtNum(s.msgs_received))}</b></div>
    <div class="fig ${p95 == null ? '' : p95 <= 5000 ? 'ok' : 'bad'}"><span class="lbl">${esc(t('p95'))}</span>
      <b>${p95 == null ? '—' : esc(fmtNum(p95 / 1000, 1))}<small>s</small></b></div>
    <div class="fig"><span class="lbl">${esc(t('snap_recs'))}</span><b>${esc(fmtNum(st && st.tracks))}</b></div>
    <div class="fig ${a && a.error ? 'bad' : ''}"><span class="lbl">${esc(t('est_proc'))}</span>
      <b>${a ? esc(fmtNum(a.processed)) : '—'}<small>${a ? esc(t('processed')) : ''}</small></b>
      <span class="muted">${a ? `${esc(fmtNum(a.rejected))} ${esc(t('rejected'))} · ${esc(fmtNum(a.repeats))} ${esc(t('repeats'))}` : esc(t('adsb_off'))}</span></div>`);
}

function drawAircraft() {
  const body = document.getElementById('adsb-rows');
  if (!body) return;
  const rows = view.aircraft.slice().sort((x, y) => (x.callsign || x.icao).localeCompare(y.callsign || y.icao));
  setHTML(body, rows.length ? rows.map((a) => `<tr data-icao="${esc(a.icao)}" class="${a.icao === selected ? 'sel' : ''}">
      <td>${esc(a.icao)}</td><td>${esc(a.callsign || '')}</td><td>${esc(a.category || '—')}</td><td>${esc(a.phase || '')}</td>
      <td>${esc(a.on_ground ? 'solo' : fmtNum(a.alt))}</td><td>${esc(fmtNum(a.gs))}</td>
      <td>${tag(a.origin)}</td><td>${tag(a.destination)}</td>
      <td>${a.combined ? tag(a.combined.destination) : '—'}</td></tr>`).join('')
    : `<tr><td colspan="9" class="muted">${esc(t('none_yet'))}</td></tr>`);
  if (map) map.set(view, selected);
  drawDetail();
}

function cands(d) {
  if (!d || !d.candidates.length) return '<span class="muted">—</span>';
  return d.candidates.map((c) => `${esc(c.ident)} <span class="muted">${esc(fmtNum(c.score, 2))}</span>`).join(' · ');
}

function drawDetail() {
  const box = document.getElementById('adsb-detail');
  const a = view.aircraft.find((x) => x.icao === selected);
  if (!a) { setHTML(box, `<h2>${esc(t('detail'))}</h2><p class="muted">${esc(t('pick'))}</p>`); return; }
  const pairs = a.pairs && a.pairs.top.length
    ? a.pairs.top.map((c) => `${esc(c.origin)} → ${esc(c.destination)} <span class="muted">${esc(fmtNum(c.score, 2))}</span>`).join('<br>')
    : `<span class="muted">${esc(a.pairs ? a.pairs.status || '—' : '—')}</span>`;
  setHTML(box, `<h2>${esc(t('detail'))}</h2>
    <h3>${esc(a.callsign || a.icao)} <span class="muted">${esc(a.icao)} · ${esc(a.category_label || '')}</span></h3>
    <div class="kv">
      <span>${esc(t('c_phase'))}</span><span>${esc(a.phase || '—')} · ${esc(a.quality || '')}</span>
      <span>${esc(t('c_alt'))} / ${esc(t('c_gs'))}</span><span>${esc(a.on_ground ? 'solo' : fmtNum(a.alt))} / ${esc(fmtNum(a.gs))} · ${esc(fmtNum(a.trk))}°</span>
      <span>${esc(t('local'))} · ${esc(t('c_origin'))}</span><span>${tag(a.origin)} ${cands(a.origin)}</span>
      <span>${esc(t('local'))} · ${esc(t('c_dest'))}</span><span>${tag(a.destination)} ${cands(a.destination)}</span>
      <span>${esc(t('pairs'))}</span><span>${pairs}</span>
      <span>${esc(t('combined'))}</span><span>${a.combined ? tag(a.combined.origin) + ' → ' + tag(a.combined.destination) : '—'}</span>
    </div>`);
}

function pick(icao) {
  selected = selected === icao ? null : icao;
  drawAircraft();
}

export const tab = {
  id: 'adsb', label: 'tab_adsb',
  mount(panel) {
    panel.innerHTML = `
      <section class="card span-5"><h2>${esc(t('mission'))}</h2>
        <p class="lead">${esc(t('adsb_lead'))}</p>
        <div class="row" style="margin-bottom:12px">
          <button class="btn primary" data-cmd="mode mission_adsb">${esc(t('start_adsb'))}</button>
          <button class="btn" data-cmd="mode downlink">${esc(t('end_adsb'))}</button></div>
        <div id="adsb-timer"></div><div id="adsb-steps"></div></section>
      <section class="card span-7"><h2>${esc(t('stats'))}</h2><div class="figs" id="adsb-figs"></div></section>
      <section class="card span-8"><h2>${esc(t('map'))}</h2><svg class="map" id="adsb-map" role="img"></svg></section>
      <section class="card span-4" id="adsb-detail"></section>
      <section class="card"><h2>${esc(t('table'))}</h2><div class="scroll"><table class="tbl"><thead><tr>
        <th>${esc(t('c_icao'))}</th><th>${esc(t('c_cs'))}</th><th>${esc(t('c_cat'))}</th><th>${esc(t('c_phase'))}</th>
        <th>${esc(t('c_alt'))}</th><th>${esc(t('c_gs'))}</th><th>${esc(t('c_origin'))}</th><th>${esc(t('c_dest'))}</th>
        <th>${esc(t('c_comb'))}</th></tr></thead><tbody id="adsb-rows"></tbody></table></div></section>`;
    map = new AdsbMap(panel.querySelector('#adsb-map'), pick);
    panel.querySelector('#adsb-rows').addEventListener('click', (e) => {
      const tr = e.target.closest('tr[data-icao]');
      if (tr) pick(tr.dataset.icao);
    });
    drawAircraft();
  },
  show() { poll(); timer = setInterval(poll, POLL_MS); },
  hide() { clearInterval(timer); timer = null; },
  update(state) { drawMission(state); },
  dot(st) {
    const run = latestRun(st, 'MISSION_ADSB');
    if (!run) return '';
    if (run.failed) return 'bad';
    return run.left != null ? 'ok' : 'run';
  },
};
