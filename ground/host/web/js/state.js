// The ground's state, as gs_web.py pushes it on /events: a full view on
// connecting, then views whose log holds only the new entries. Modules
// subscribe and are called with the state on every message, and once a
// second so the ages they show keep counting.

const LOG_KEEP = 500;
const subscribers = new Set();

export const state = {
  st: null,          // the last view
  log: [],           // every log entry heard, oldest first, at most LOG_KEEP
  arrivedAt: 0,      // Date.now() when it came
  connected: false,
};

// How old a server-side age is now: the age it had when it arrived, plus the
// time since.
export function since(age) {
  if (age == null) return null;
  return age + (Date.now() - state.arrivedAt) / 1000;
}

// Server epoch seconds of "now", following the server's clock.
export function serverNow() {
  if (!state.st || state.st.now == null) return Date.now() / 1000;
  return state.st.now + (Date.now() - state.arrivedAt) / 1000;
}

export function subscribe(fn) {
  subscribers.add(fn);
  if (state.st) fn(state, false);
}

function emit(fresh) {
  for (const fn of subscribers) {
    try { fn(state, fresh); } catch (e) { console.error(e); }
  }
}

export function merge(view) {
  const last = state.log.length ? state.log[state.log.length - 1].n : 0;
  if (view.full) {
    state.log = view.log.slice();
  } else {
    for (const e of view.log || []) if (e.n > last) state.log.push(e);
  }
  if (state.log.length > LOG_KEEP) state.log.splice(0, state.log.length - LOG_KEEP);
  state.st = view;
  state.arrivedAt = Date.now();
}

function connect() {
  const es = new EventSource('/events');
  es.onopen = () => { state.connected = true; emit(false); };
  es.onmessage = (e) => { merge(JSON.parse(e.data)); emit(true); };
  es.onerror = () => {
    state.connected = false;
    es.close();
    emit(false);
    setTimeout(connect, 2000);
  };
}

export function start() {
  connect();
  setInterval(() => emit(false), 1000);
}
