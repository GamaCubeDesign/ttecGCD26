// ADS-B: the mission, step by step from the button (SET_TIME, then SET_MODE,
// ADR-0020) to the downlink; what the satellite reports (HLR-ADS-08); and
// the aircraft the ground reconstructs, on the map, in a table and one by
// one (origin and destination by each method, ADR-0019).

import { esc, fmtAge, fmtClock, fmtNum, setHTML, store } from '../util.js';
import { t } from '../i18n.js';
import { serverNow, state } from '../state.js';
import { latestRun, ack, step, after } from '../runs.js';
import { AdsbMap } from '../map.js';

const MISSION_S = 600;      // obcd's adsb_timeout (OBC-6), unless gs_web.py says otherwise
const POLL_MS = 2000;

let view = { revision: 0, aircraft: [], airports: {} };
let etag = null;
let selected = null;
let map = null;
let timer = null;
// The aircraft table's filter: all, those heard now, those heard before.
// "Now" is still in the satellite's table: adsbd drops an aircraft silent
// for track_expiry_ms (60 s, flight/adsbd/core.c), the estimator's ACTIVE.
const NOW_S = 60;
const SEEN = ['all', 'now', 'past'];
let seen = SEEN.includes(store('gs-adsb-seen')) ? store('gs-adsb-seen') : 'all';
let mapKey = '';

// The ground's clock now, in s: a replay plays old times, so there "now" is
// the newest aircraft's last update.
function refNow() {
  if (state.st && state.st.source === 'replay') {
    const ts = view.aircraft.map((a) => a.t || 0).filter(Boolean);
    if (ts.length) return Math.max(...ts) / 1000;
  }
  return serverNow();
}

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
  // From the click to the end of the mission, while nothing failed, the
  // mission card steps aside for the map: the map's header keeps the
  // clock, the step reached and the button that ends it. A failure, or
  // the end, brings the card back with every step.
  const active = run && !run.failed && run.left == null;
  const steps = missionSteps(run);
  document.getElementById('adsb-mission').hidden = !!active;
  const live = document.getElementById('adsb-live');
  live.hidden = !active;
  if (active) {
    const done = (steps.match(/class="done"/g) || []).length;
    let clock = `<span class="muted">${esc(t('starting'))}</span>`;
    if (running) {
      const gone = serverNow() - run.entered;
      const total = (st && st.mission_s) || MISSION_S;      // gs_web.py --mission-s
      clock = `<b class="timer-s">${esc(fmtClock(Math.max(0, total - gone)))}</b> <span class="muted">${esc(t('remaining'))}</span>`;
    }
    setHTML(document.getElementById('adsb-live-info'),
      `${clock} <span class="muted">· ${esc(t('step'))} ${done}/7</span> <span class="caret">▾</span>`);
    setHTML(document.getElementById('adsb-live-steps'), steps);
  } else {
    showSteps(false);
  }
  setHTML(document.getElementById('adsb-steps'), steps);
  const s = (st && st.stat) || {};
  const a = (st && st.adsb) || null;
  const p95 = s.latency_p95_ms;
  setHTML(document.getElementById('adsb-figs'), `<div class="figs mini">
    <div class="fig"><span class="lbl">${esc(t('aircraft'))}</span><b>${esc(fmtNum(s.aircraft))}</b></div>
    <div class="fig"><span class="lbl">${esc(t('messages'))}</span><b>${esc(fmtNum(s.msgs_received))}</b></div>
    <div class="fig ${p95 == null ? '' : p95 <= 5000 ? 'ok' : 'bad'}"><span class="lbl">${esc(t('p95'))}</span>
      <b>${p95 == null ? '—' : esc(fmtNum(p95 / 1000, 1))}<small>s</small></b></div>
    <div class="fig" title="${esc(t('snap_recs'))}"><span class="lbl">${esc(t('snap_short'))}</span><b>${esc(fmtNum(st && st.tracks))}</b></div></div>
    <p class="muted" style="margin:8px 0 0;font-size:.82em">${esc(t('est_proc'))}: ${a
      ? `${esc(fmtNum(a.processed))} ${esc(t('processed'))} · ${esc(fmtNum(a.rejected))} ${esc(t('rejected'))} · ${esc(fmtNum(a.repeats))} ${esc(t('repeats'))}${a.error ? ' · <b style="color:var(--bad-fg)">' + esc(a.error) + '</b>' : ''}`
      : esc(t('adsb_off'))}</p>`);
}

function drawAircraft() {
  const body = document.getElementById('adsb-rows');
  if (!body) return;
  // a.t: the aircraft's last update aboard, in the ground's clock (ms).
  const ref = refNow();
  const all = view.aircraft.map((a) => {
    const age = a.t ? Math.max(0, ref - a.t / 1000) : null;
    return { a, age, now: age != null && age <= NOW_S };
  });
  const nNow = all.filter((x) => x.now).length;
  setHTML(document.getElementById('adsb-seen'), [['all', all.length], ['now', nNow], ['past', all.length - nNow]].map(([k, n]) =>
    `<button type="button" data-seen="${k}" aria-pressed="${k === seen}" title="${esc(t('seen_' + k + '_tip'))}">${esc(t('seen_' + k))} <span class="n">${n}</span></button>`).join(''));
  const shown = all.filter((x) => seen === 'all' || (seen === 'now') === x.now);
  const rows = shown.sort((x, y) => (x.now !== y.now ? (x.now ? -1 : 1)
    : (x.a.callsign || x.a.icao).localeCompare(y.a.callsign || y.a.icao)));
  setHTML(body, rows.length ? rows.map(({ a, age, now }) => `<tr data-icao="${esc(a.icao)}" class="${a.icao === selected ? 'sel' : ''}${now ? '' : ' past'}">
      <td>${now ? `<span class="seen-now"><i></i>${esc(t('seen_now'))}</span>`
        : `<span class="seen-past">${esc(age == null ? '—' : t('seen_ago').replace('{}', fmtAge(age)))}</span>`}</td>
      <td>${esc(a.icao)}</td><td>${esc(a.callsign || '')}</td><td title="${esc(a.category_source === 'kinematics' ? t('cat_inferred') : '')}">${esc(a.category || '—')}${a.category_source === 'kinematics' ? '<sup>*</sup>' : ''}</td><td>${esc(a.phase || '')}</td>
      <td>${esc(a.on_ground ? 'solo' : fmtNum(a.alt))}</td><td>${esc(fmtNum(a.gs))}</td>
      <td>${tag(a.origin)}</td><td>${tag(a.destination)}</td>
      <td>${a.combined ? tag(a.combined.destination) : '—'}</td></tr>`).join('')
    : `<tr><td colspan="10" class="muted">${esc(t(all.length ? 'seen_none' : 'none_yet'))}</td></tr>`);
  // The map shows what the table shows; redrawn only when that changes,
  // since the table is redrawn every second for the ages.
  const key = `${view.revision}|${seen}|${selected}|${rows.map((x) => x.a.icao).join(',')}`;
  if (map && key !== mapKey) {
    mapKey = key;
    map.set({ ...view, aircraft: rows.map((x) => x.a) }, selected);
  }
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
    <h3>${esc(a.callsign || a.icao)} <span class="muted">${esc(a.icao)} · ${esc(a.category_label || '')}${a.category_source === 'kinematics' ? ' · ' + esc(t('cat_inferred')) : ''}</span></h3>
    <div class="kv">
      <span>${esc(t('c_phase'))}</span><span>${esc(a.phase || '—')} · ${esc(a.quality || '')}</span>
      <span>${esc(t('c_alt'))} / ${esc(t('c_gs'))}</span><span>${esc(a.on_ground ? 'solo' : fmtNum(a.alt))} / ${esc(fmtNum(a.gs))} · ${esc(fmtNum(a.trk))}°</span>
      <span>${esc(t('local'))} · ${esc(t('c_origin'))}</span><span>${tag(a.origin)} ${cands(a.origin)}</span>
      <span>${esc(t('local'))} · ${esc(t('c_dest'))}</span><span>${tag(a.destination)} ${cands(a.destination)}</span>
      <span>${esc(t('pairs'))}</span><span>${pairs}</span>
      <span>${esc(t('combined'))}</span><span>${a.combined ? tag(a.combined.origin) + ' → ' + tag(a.combined.destination) : '—'}</span>
    </div>`);
}

// The step list behind "step 6/7" in the map's header, open on a click.
function showSteps(on) {
  const pop = document.getElementById('adsb-live-steps'), b = document.getElementById('adsb-live-info');
  if (!pop) return;
  pop.hidden = !on;
  b.setAttribute('aria-expanded', String(on));
}

// The table toggles; the map always selects (a click near the aircraft
// already picked keeps it).
function pick(icao, keep) {
  selected = keep ? icao : selected === icao ? null : icao;
  drawAircraft();
}

export const tab = {
  id: 'adsb', label: 'tab_adsb',
  mount(panel) {
    // The map takes the tab; the mission, the figures and the aircraft
    // picked go in a column beside it, the table below.
    panel.innerHTML = `
      <section class="card span-8 map-card">
        <div class="map-head"><h2>${esc(t('map'))}</h2>
          <span class="live" id="adsb-live" hidden><button class="live-info" type="button" id="adsb-live-info" aria-expanded="false" title="${esc(t('show_steps'))}"></button>
            <button class="btn" data-cmd="mode downlink">${esc(t('end_adsb'))}</button></span>
          <button class="btn" type="button" id="adsb-fit" aria-pressed="true" title="${esc(t('fit_hint'))}">${esc(t('fit'))}</button></div>
        <div class="live-steps mission" id="adsb-live-steps" hidden></div>
        <svg class="map" id="adsb-map" role="img"></svg>
        <div class="legend">
          <span><i class="l-ac"></i>${esc(t('legend_ac'))}</span>
          <span><i class="l-ref"></i>${esc(t('legend_ref'))}</span>
          <span><i class="l-apt"></i>${esc(t('legend_apt'))}</span>
          <span><i class="l-city"></i>${esc(t('legend_city'))}</span>
          <span class="hint">${esc(t('map_hint'))}</span></div></section>
      <div class="stack span-4">
        <section class="card mission" id="adsb-mission"><h2>${esc(t('mission'))}</h2>
          <div class="row" style="margin-bottom:10px">
            <button class="btn primary" data-cmd="mode mission_adsb" title="${esc(t('adsb_lead'))}">${esc(t('start_adsb'))}</button>
            <button class="btn" data-cmd="mode downlink">${esc(t('end_adsb'))}</button></div>
          <div id="adsb-steps"></div></section>
        <section class="card"><h2>${esc(t('stats'))}</h2><div id="adsb-figs"></div></section>
        <section class="card" id="adsb-detail"></section>
      </div>
      <section class="card"><div class="card-head"><h2>${esc(t('table'))}</h2><div class="adsb-seen" id="adsb-seen" role="group"></div></div>
        <div class="scroll"><table class="tbl"><thead><tr>
        <th>${esc(t('c_seen'))}</th><th>${esc(t('c_icao'))}</th><th>${esc(t('c_cs'))}</th><th>${esc(t('c_cat'))}</th><th>${esc(t('c_phase'))}</th>
        <th>${esc(t('c_alt'))}</th><th>${esc(t('c_gs'))}</th><th>${esc(t('c_origin'))}</th><th>${esc(t('c_dest'))}</th>
        <th>${esc(t('c_comb'))}</th></tr></thead><tbody id="adsb-rows"></tbody></table></div></section>`;
    map = new AdsbMap(panel.querySelector('#adsb-map'), (icao) => pick(icao, true));
    panel.querySelector('#adsb-fit').addEventListener('click', () => map.fit());
    map.onFollow = (on) => panel.querySelector('#adsb-fit').setAttribute('aria-pressed', String(on));
    panel.querySelector('#adsb-live-info').addEventListener('click', () =>
      showSteps(panel.querySelector('#adsb-live-steps').hidden));
    panel.querySelector('#adsb-rows').addEventListener('click', (e) => {
      const tr = e.target.closest('tr[data-icao]');
      if (!tr) return;
      pick(tr.dataset.icao);
      // Up to the map, which shows the aircraft picked (and its detail).
      if (selected) {
        panel.querySelector('.map-card').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
        map.reveal(selected);
      }
    });
    panel.querySelector('#adsb-seen').addEventListener('click', (e) => {
      const b = e.target.closest('[data-seen]');
      if (!b) return;
      seen = b.dataset.seen;
      store('gs-adsb-seen', seen);
      drawAircraft();
    });
    mapKey = '';
    drawAircraft();
  },
  show() { poll(); timer = setInterval(poll, POLL_MS); },
  hide() { clearInterval(timer); timer = null; },
  update(s) { drawMission(s); drawAircraft(); },   // the ages move on by themselves
  dot(st) {
    const run = latestRun(st, 'MISSION_ADSB');
    if (!run) return '';
    if (run.failed) return 'bad';
    return run.left != null ? 'ok' : 'run';
  },
};
