// The ground station's page: the bar, the tabs, the utilities and the
// events, all fed by one state (state.js). Only the visible tab redraws.

import { $, esc, store } from './util.js';
import { t, labels, nextLang } from './i18n.js';
import { start, subscribe, state } from './state.js';
import { renderUtilities, wire } from './commands.js';
import { renderBar } from './telemetry.js';
import { renderEventsFrame, renderEvents } from './events.js';
import { tab as operations } from './tabs/operations.js';
import { tab as adsb } from './tabs/adsb.js';
import { deploy, aocs, survival } from './tabs/modes.js';
import { tab as link } from './tabs/link.js';

const TABS = [operations, adsb, deploy, aocs, survival, link];
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
}

function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  store('gs-theme', theme);
  $('b-theme').textContent = theme === 'dark' ? t('theme_light') : t('theme_dark');
}

function relabel() {
  document.title = t('title');
  labels();
  $('b-lang').textContent = t('lang');
  $('b-big').textContent = t('big');
  applyTheme(document.documentElement.dataset.theme || 'dark');
  renderUtilities();
  renderEventsFrame();
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
$('b-lang').addEventListener('click', () => { nextLang(); relabel(); });
$('b-theme').addEventListener('click', () => applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
$('b-big').addEventListener('click', () => {
  const on = document.body.classList.toggle('big');
  $('b-big').setAttribute('aria-pressed', String(on));
});
wire($('panels'));
wire($('utilities'));

relabel();
subscribe(render);
start();
