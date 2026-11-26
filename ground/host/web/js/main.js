// The ground station's page: the bar, the tabs and the events bar, all fed
// by one state (state.js). Only the visible tab redraws.

import { $, esc, store } from './util.js';
import { t, labels, setLang, currentLang } from './i18n.js';
import { start, subscribe, state } from './state.js';
import { refreshArm, wire } from './commands.js';
import { renderBar } from './telemetry.js';
import { renderEventsFrame, renderEvents, clearUnseen } from './events.js';
import { tab as operations } from './tabs/operations.js';
import { tab as adsb } from './tabs/adsb.js';
import { deploy, aocs, survival } from './tabs/modes.js';
import { tab as link } from './tabs/link.js';
import { tab as obc } from './tabs/obc.js';

const TABS = [operations, adsb, deploy, aocs, survival, obc, link];
// The tab in the address (#adsb) wins over the one remembered.
let active = location.hash.slice(1) || store('gs-tab') || 'ops';
if (!TABS.some((x) => x.id === active)) active = 'ops';

function buildTabs() {
  $('tabs').innerHTML = TABS.map((x) =>
    `<button class="tab" role="tab" type="button" data-tab="${x.id}" aria-selected="${x.id === active}">` +
    `<span class="dot" id="dot-${x.id}"></span>${esc(t(x.label))}</button>`).join('');
  $('panels').innerHTML = TABS.map((x) => `<div class="panel" id="panel-${x.id}" role="tabpanel" ${x.id === active ? '' : 'hidden'}></div>`).join('');
  for (const x of TABS) x.mount($('panel-' + x.id));
}

function show(id) {
  const prev = TABS.find((x) => x.id === active);
  if (prev && prev.hide) prev.hide();
  active = id;
  store('gs-tab', id);
  if (location.hash !== '#' + id) history.replaceState(null, '', '#' + id);
  for (const b of $('tabs').querySelectorAll('.tab')) b.setAttribute('aria-selected', String(b.dataset.tab === id));
  for (const x of TABS) $('panel-' + x.id).hidden = x.id !== id;
  const cur = TABS.find((x) => x.id === id);
  if (cur.show) cur.show();
  if (state.st) cur.update(state, true);
  refreshArm();
}
// A link to a tab (#deploy), as the operations tab's test tiles are.
window.addEventListener('hashchange', () => {
  const id = location.hash.slice(1);
  if (id !== active && TABS.some((x) => x.id === id)) show(id);
});

// The theme button shows the theme it switches to: a sun on the dark page,
// a moon on the light one; its name is the tooltip.
const SUN = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4.2"/>' +
  '<path d="M12 2.5v2.6M12 18.9v2.6M2.5 12h2.6M18.9 12h2.6M5.3 5.3l1.8 1.8M16.9 16.9l1.8 1.8M5.3 18.7l1.8-1.8M16.9 7.1l1.8-1.8"/></svg>';
const MOON = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 14.6A8.3 8.3 0 0 1 9.4 4a8.3 8.3 0 1 0 10.6 10.6Z"/></svg>';

function applyTheme(theme) {
  const changed = document.documentElement.dataset.theme !== theme;
  document.documentElement.dataset.theme = theme;
  store('gs-theme', theme);
  const b = $('b-theme');
  b.innerHTML = theme === 'dark' ? SUN : MOON;
  b.title = theme === 'dark' ? t('theme_light') : t('theme_dark');
  b.setAttribute('aria-label', b.title);
  // The charts are canvases, painted with the colours of the moment: paint
  // them again in the new theme's.
  if (changed) window.dispatchEvent(new Event('gs-redraw'));
}

function relabel() {
  document.title = t('title');
  labels();
  for (const b of $('b-lang').querySelectorAll('[data-lang]')) b.setAttribute('aria-pressed', String(b.dataset.lang === currentLang()));
  // A magnifier: letters larger, for a projector; its name is the tooltip.
  $('b-big').title = t('big');
  $('b-big').setAttribute('aria-label', t('big'));
  applyTheme(document.documentElement.dataset.theme || 'dark');
  renderEventsFrame();
  applyDock(store('gs-dock') === '1', store('gs-dock-hidden') === '1');
  for (const x of TABS) if (x.hide) x.hide();
  buildTabs();
  show(active);
  if (state.st) render(state, true);
}

function render(s, fresh) {
  renderBar(s);
  renderEvents(s);
  const cur = TABS.find((x) => x.id === active);
  cur.update(s, fresh);
  for (const x of TABS) {
    const dot = $('dot-' + x.id);
    if (dot && x.dot) dot.className = 'dot ' + x.dot(s.st);
  }
}

$('tabs').addEventListener('click', (e) => {
  const b = e.target.closest('[data-tab]');
  if (b) show(b.dataset.tab);
});
$('b-lang').addEventListener('click', (e) => {
  const b = e.target.closest('[data-lang]');
  if (b && b.dataset.lang !== currentLang()) { setLang(b.dataset.lang); relabel(); }
});
// A soft switch: a cross-fade where the browser has View Transitions,
// colours easing for 450 ms where not.
let themingTimer = null;
$('b-theme').addEventListener('click', () => {
  const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  if (document.startViewTransition) { document.startViewTransition(() => applyTheme(next)); return; }
  const root = document.documentElement;
  root.classList.add('theming');
  applyTheme(next);
  clearTimeout(themingTimer);
  themingTimer = setTimeout(() => root.classList.remove('theming'), 450);
});
$('b-big').addEventListener('click', () => {
  const on = document.body.classList.toggle('big');
  $('b-big').setAttribute('aria-pressed', String(on));
});
wire($('panels'));
// The events bar, fixed at the bottom of every tab: hidden (its header
// alone, counting what arrives), short (a few lines) or tall; remembered.
function applyDock(big, hidden) {
  document.body.classList.toggle('dock-big', big);
  document.body.classList.toggle('dock-hidden', hidden);
  store('gs-dock', big ? '1' : '0');
  store('gs-dock-hidden', hidden ? '1' : '0');
  if (!hidden) clearUnseen();
  const b = document.getElementById('ev-size');
  if (b) {
    b.hidden = hidden;
    b.textContent = big ? '▾' : '▴'; b.title = t(big ? 'ev_shrink' : 'ev_grow'); b.setAttribute('aria-label', b.title);
  }
  const h = document.getElementById('ev-hide');
  if (h) { h.textContent = t(hidden ? 'ev_show' : 'ev_hide'); h.setAttribute('aria-expanded', String(!hidden)); }
  window.dispatchEvent(new Event('resize'));
}
$('events').addEventListener('click', (e) => {
  const body = document.body.classList;
  if (e.target.closest('#ev-size')) applyDock(!body.contains('dock-big'), false);
  if (e.target.closest('#ev-hide')) applyDock(body.contains('dock-big'), !body.contains('dock-hidden'));
});
window.addEventListener('gs-redraw', () => { if (state.st) render(state, true); });
// The bar's height, for what sticks below it (the map).
new ResizeObserver(() => document.documentElement.style.setProperty('--bar-h', $('bar').offsetHeight + 'px')).observe($('bar'));

relabel();
subscribe(render);
start();
