// The events bar, fixed at the bottom of every tab: the ground's log,
// newest first, appended row by row as it arrives, with a filter by kind.
// Its size button is wired in main.js.

import { $, esc, fmtTime } from './util.js';
import { t } from './i18n.js';

const ROWS_MAX = 400;
const FILTERS = [
  ['all', null], ['cmd', ['cmd']], ['bad', ['bad']], ['mode', ['mode']], ['tm', ['tm', 'info', 'ok']],
];
let filter = 'all';
let shownN = 0;
// Entries that arrived while the bar was hidden, and whether one failed:
// the folded bar shows them so nothing goes unseen.
let unseen = 0;
let unseenBad = false;

function paintUnseen() {
  const b = $('ev-unseen');
  if (!b) return;
  b.hidden = !unseen;
  b.className = 'ev-unseen' + (unseenBad ? ' bad' : '');
  b.textContent = unseen + ' ' + t('ev_new');
}

export function clearUnseen() { unseen = 0; unseenBad = false; paintUnseen(); }

function row(e) {
  return `<div class="ev ${esc(e.kind)}" data-k="${esc(e.kind)}"><time>${esc(fmtTime(e.t))}</time><span>${esc(e.text)}</span></div>`;
}

function visible(kind) {
  const f = FILTERS.find(([k]) => k === filter);
  return !f[1] || f[1].includes(kind);
}

function applyFilter() {
  for (const el of $('events').querySelectorAll('.ev')) el.hidden = !visible(el.dataset.k);
  for (const b of $('events').querySelectorAll('.filters button')) b.setAttribute('aria-pressed', String(b.dataset.f === filter));
}

export function renderEventsFrame() {
  $('events').innerHTML = `<div class="dock-head"><h2>${esc(t('events'))}</h2>
    <div class="filters">${FILTERS.map(([k]) => `<button type="button" data-f="${k}">${esc(t('f_' + k))}</button>`).join('')}</div>
    <span id="ev-unseen" hidden></span>
    <div class="dock-tools"><button class="tool" type="button" id="ev-size"></button>
    <button class="tool" type="button" id="ev-hide"></button></div></div>
    <div class="list" id="evlist"></div>`;
  $('events').querySelector('.filters').addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (b) { filter = b.dataset.f; applyFilter(); }
  });
  shownN = 0;
}

export function renderEvents(state) {
  const list = $('evlist');
  if (!list) return;
  const fresh = state.log.filter((e) => e.n > shownN);
  if (!fresh.length) return;
  // A reconnect brings the whole recent log again: start over then.
  if (shownN && fresh[0].n !== shownN + 1 && state.st && state.st.full) list.innerHTML = '';
  list.insertAdjacentHTML('afterbegin', fresh.slice().reverse().map(row).join(''));
  shownN = fresh[fresh.length - 1].n;
  if (document.body.classList.contains('dock-hidden')) {
    unseen += fresh.filter((e) => visible(e.kind)).length;
    unseenBad = unseenBad || fresh.some((e) => e.kind === 'bad');
    paintUnseen();
  }
  while (list.childElementCount > ROWS_MAX) list.lastElementChild.remove();
  applyFilter();
}
