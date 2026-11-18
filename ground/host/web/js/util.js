// Small helpers shared by every module of the page.

export const $ = (id) => document.getElementById(id);

export function esc(s) {
  return String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

// Seconds as "42 s", "3 min 05 s", "1 h 02 min".
export function fmtAge(s) {
  if (s == null || !isFinite(s)) return '—';
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' min ' + String(s % 60).padStart(2, '0') + ' s';
  return Math.floor(s / 3600) + ' h ' + String(Math.floor((s % 3600) / 60)).padStart(2, '0') + ' min';
}

// Seconds as a clock, "09:58".
export function fmtClock(s) {
  if (s == null || !isFinite(s)) return '—:—';
  const neg = s < 0;
  s = Math.abs(Math.round(s));
  return (neg ? '−' : '') + String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
}

// Epoch seconds as local wall time, "18:17:29".
export function fmtTime(t) {
  if (t == null) return '—';
  return new Date(t * 1000).toLocaleTimeString([], { hour12: false });
}

export function fmtNum(v, digits = 0) {
  if (v == null || !isFinite(v)) return '—';
  return Number(v).toLocaleString('pt-BR', { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

// Replaces an element's content only when it changed: no reflow otherwise.
export function setHTML(node, html) {
  if (node && node.__html !== html) {
    node.innerHTML = html;
    node.__html = html;
  }
}

export function store(key, value) {
  try {
    if (value === undefined) return localStorage.getItem(key);
    localStorage.setItem(key, value);
  } catch (e) { /* private mode: nothing remembered */ }
  return null;
}
