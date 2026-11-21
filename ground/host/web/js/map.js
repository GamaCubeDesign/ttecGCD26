// An offline map in SVG: the land from the pair database's region
// (/api/basemap, Natural Earth), the urban areas, city names and province
// lines over it (/api/mapdetail, Natural Earth 1:10m), the large and medium airports
// (/api/airports), and the aircraft with their tracks. No tiles, no
// network: it works where the competition is.
//
// World coordinates are longitude and Mercator latitude, both in degrees.
// Panning and zooming change one transform; only the few markers and
// labels, which keep their size on screen, are placed again.

const NS = 'http://www.w3.org/2000/svg';
const R2D = 180 / Math.PI;
const PICK_PX = 26;      // a click this close to an aircraft picks it
const CLICK_PX = 5;      // a press that moves less than this is a click, not a drag
const CITY_MAX = 70;     // city names on screen at most

function mercY(lat) {
  const phi = Math.max(-85, Math.min(85, lat)) / R2D;
  return -R2D * Math.log(Math.tan(Math.PI / 4 + phi / 2));
}

function el(tag, attrs, parent) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (parent) parent.appendChild(e);
  return e;
}

function ringPath(ring) {
  let d = '';
  for (let i = 0; i < ring.length; i++) {
    const [lon, lat] = ring[i];
    d += (i ? 'L' : 'M') + lon.toFixed(3) + ' ' + mercY(lat).toFixed(3);
  }
  return d + 'Z';
}

export class AdsbMap {
  constructor(svg, onPick) {
    this.svg = svg;
    this.onPick = onPick;
    this.center = [-58.5, mercY(-34.6)];   // UNSAM until there are aircraft
    this.k = 60;                           // pixels per degree
    // Following: the view refits by itself while aircraft appear or leave
    // it, until the operator pans or zooms; "fit" (or a double click)
    // follows again. onFollow tells the page, for the button's state.
    this.follow = true;
    this.onFollow = null;
    this.known = new Set();
    this.anim = null;
    this.aircraft = [];
    this.refs = {};
    this.selected = null;
    this.airports = [];
    this.world = el('g', {}, svg);
    this.land = el('g', {}, this.world);
    this.detail = el('g', {}, this.world);
    this.places = [];
    this.tracks = el('g', {}, this.world);
    this.overlay = el('g', {}, svg);
    this._interact();
    new ResizeObserver(() => this.draw()).observe(svg);
    this._load();
  }

  async _load() {
    try {
      const geo = await (await fetch('/api/basemap')).json();
      let d = '';
      for (const f of geo.features || []) {
        const g = f.geometry || {};
        const polys = g.type === 'Polygon' ? [g.coordinates] : g.type === 'MultiPolygon' ? g.coordinates : [];
        for (const poly of polys) for (const ring of poly) d += ringPath(ring);
      }
      el('path', { d, class: 'land', 'fill-rule': 'evenodd' }, this.land);
    } catch (e) { /* no land: the aircraft still show */ }
    try {
      const det = await (await fetch('/api/mapdetail')).json();
      let u = '';
      for (const r of det.urban || []) u += ringPath(r);
      el('path', { d: u, class: 'urban' }, this.detail);
      let a = '';
      for (const line of det.admin1 || []) {
        line.forEach(([lon, lat], i) => { a += (i ? 'L' : 'M') + lon.toFixed(3) + ' ' + mercY(lat).toFixed(3); });
      }
      el('path', { d: a, class: 'adm1' }, this.detail);
      this.places = det.places || [];    // [name, lat, lon, population, min_zoom], largest first
    } catch (e) { /* no detail: the land alone */ }
    try {
      this.airports = await (await fetch('/api/airports')).json();
    } catch (e) { this.airports = []; }
    this.draw();
  }

  size() {
    const r = this.svg.getBoundingClientRect();
    return [Math.max(1, r.width), Math.max(1, r.height)];
  }

  toScreen(lon, lat) {
    const [w, h] = this.size();
    return [w / 2 + (lon - this.center[0]) * this.k, h / 2 + (mercY(lat) - this.center[1]) * this.k];
  }

  setFollow(on) {
    this.follow = on;
    if (this.onFollow) this.onFollow(on);
  }

  // The view that holds every aircraft and its track, or null.
  _target() {
    const pts = [];
    for (const a of this.aircraft) {
      if (a.lat != null) pts.push([a.lon, mercY(a.lat)]);
      for (const p of a.track) pts.push([p[2], mercY(p[1])]);
    }
    if (!pts.length) return null;
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    const [w, h] = this.size();
    const spanX = Math.max(0.3, Math.max(...xs) - Math.min(...xs));
    const spanY = Math.max(0.3, Math.max(...ys) - Math.min(...ys));
    return { center: [(Math.max(...xs) + Math.min(...xs)) / 2, (Math.max(...ys) + Math.min(...ys)) / 2],
      k: Math.min(w / spanX, h / spanY) * 0.85 };
  }

  // Frames every aircraft and follows again; glides there unless told not
  // to (or the system asks for reduced motion).
  fit(glide = true) {
    const to = this._target();
    if (!to) return false;
    this.setFollow(true);
    this._glide(to, glide);
    return true;
  }

  // Brings an aircraft into view (a pick from the table): nothing if it is
  // already well inside, else the view glides to centre it, at the same zoom.
  reveal(icao) {
    const a = this.aircraft.find((x) => x.icao === icao);
    if (!a || a.lat == null) return;
    const [w, h] = this.size(), [x, y] = this.toScreen(a.lon, a.lat);
    if (x > w * 0.1 && y > h * 0.1 && x < w * 0.9 && y < h * 0.9) return;
    this.setFollow(false);
    this._glide({ center: [a.lon, mercY(a.lat)], k: this.k }, true);
  }

  _glide(to, glide) {
    cancelAnimationFrame(this.anim);
    if (!glide || matchMedia('(prefers-reduced-motion: reduce)').matches) {
      this.center = to.center; this.k = to.k; this.draw();
      return;
    }
    const from = { center: this.center.slice(), k: this.k }, t0 = performance.now(), T = 450;
    const stepFn = (now) => {
      const u = Math.min(1, (now - t0) / T), e = 1 - Math.pow(1 - u, 3);
      // Zoom in log space, so it feels even whether it goes in or out.
      this.k = Math.exp(Math.log(from.k) + (Math.log(to.k) - Math.log(from.k)) * e);
      this.center = [from.center[0] + (to.center[0] - from.center[0]) * e, from.center[1] + (to.center[1] - from.center[1]) * e];
      this.draw();
      if (u < 1) this.anim = requestAnimationFrame(stepFn);
    };
    this.anim = requestAnimationFrame(stepFn);
  }

  // While following: refit when an aircraft never seen appears, or one on
  // the map is outside the view (a 6 % margin), not on every poll.
  _maybeRefit() {
    let fresh = false;
    for (const a of this.aircraft) if (!this.known.has(a.icao)) { this.known.add(a.icao); fresh = true; }
    if (!this.follow) return;
    const [w, h] = this.size(), mx = w * 0.06, my = h * 0.06;
    const out = this.aircraft.some((a) => {
      if (a.lat == null) return false;
      const [x, y] = this.toScreen(a.lon, a.lat);
      return x < mx || y < my || x > w - mx || y > h - my;
    });
    if (fresh || out) this.fit(this.known.size > 0 && this.drawnOnce);
  }

  set(view, selected) {
    this.aircraft = view.aircraft || [];
    this.refs = view.airports || {};
    this.selected = selected;
    // No aircraft (a new mission, a new session): start following afresh.
    if (!this.aircraft.length && this.known.size) { this.known.clear(); this.setFollow(true); }
    this._tracks();
    this._maybeRefit();
    this.draw();
    this.drawnOnce = this.aircraft.length > 0;
  }

  select(icao) {
    this.selected = icao;
    this._tracks();
    this.draw();
  }

  _tracks() {
    this.tracks.textContent = '';
    for (const a of this.aircraft) {
      if (a.track.length < 2) continue;
      let d = '';
      a.track.forEach((p, i) => { d += (i ? 'L' : 'M') + p[2].toFixed(4) + ' ' + mercY(p[1]).toFixed(4); });
      el('path', { d, class: 'trk' + (a.icao === this.selected ? ' sel' : '') }, this.tracks);
    }
  }

  draw() {
    const [w, h] = this.size();
    this.svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
    const tx = w / 2 - this.center[0] * this.k, ty = h / 2 - this.center[1] * this.k;
    this.world.setAttribute('transform', `translate(${tx} ${ty}) scale(${this.k})`);
    const o = this.overlay;
    o.textContent = '';
    const inView = ([x, y]) => x > -20 && y > -20 && x < w + 20 && y < h + 20;
    // City names, the largest first, as far out as Natural Earth places
    // them (min_zoom, a web-map zoom level: k = 256·2^z/360 px per degree),
    // one step earlier, and never over one another.
    const zoom = Math.log2(this.k * 360 / 256);
    const taken = [];
    let shown = 0;
    for (const [name, lat, lon, pop, minZoom] of this.places) {
      if (shown >= CITY_MAX) break;
      if (minZoom > zoom + 1.5) continue;
      const p = this.toScreen(lon, lat);
      if (!inView(p)) continue;
      const big = pop >= 1000000;
      const wl = name.length * (big ? 7.4 : 6.6) + 10, hl = 16;
      const box = [p[0] - 3, p[1] - hl, p[0] + wl, p[1] + 4];
      if (taken.some((b) => box[0] < b[2] && box[2] > b[0] && box[1] < b[3] && box[3] > b[1])) continue;
      taken.push(box);
      el('circle', { cx: p[0], cy: p[1], r: big ? 2.6 : 2, class: 'city' }, o);
      el('text', { x: p[0] + 5, y: p[1] - 4, class: 'city-label' + (big ? ' big' : '') }, o).textContent = name;
      shown++;
    }
    // Background airports: only when zoomed in enough to tell them apart.
    if (this.k > 40) {
      let n = 0;
      for (const [ident, lat, lon, , type] of this.airports) {
        const p = this.toScreen(lon, lat);
        if (!inView(p) || this.refs[ident]) continue;
        el('circle', { cx: p[0], cy: p[1], r: type === 'large_airport' ? 2.6 : 1.8, class: 'apt' }, o);
        if (this.k > 120 && type === 'large_airport') {
          el('text', { x: p[0] + 4, y: p[1] - 3, class: 'apt-label' }, o).textContent = ident;
        }
        if (++n > 600) break;
      }
    }
    // Airports the estimator names: always marked; labelled when they are
    // the selected aircraft's, or when zoomed in enough to read them.
    const mine = new Set();
    const sel = this.aircraft.find((a) => a.icao === this.selected);
    if (sel) {
      for (const d of [sel.origin, sel.destination]) {
        if (d.ident) mine.add(d.ident);
        for (const c of d.candidates) mine.add(c.ident);
      }
      if (sel.combined) { mine.add(sel.combined.origin.ident); mine.add(sel.combined.destination.ident); }
    }
    for (const ident in this.refs) {
      const [lat, lon] = this.refs[ident];
      const p = this.toScreen(lon, lat);
      if (!inView(p)) continue;
      const big = mine.has(ident);
      const r = big ? 4.5 : 2.5;
      const sq = el('rect', { x: p[0] - r, y: p[1] - r, width: 2 * r, height: 2 * r, class: 'apt-ref' + (big ? ' mine' : '') }, o);
      // The name on hover: "SDS2" alone says little.
      const info = this.refs[ident];
      el('title', {}, sq).textContent = ident + (info[2] ? ' · ' + info[2] : '') + (info[3] ? ' (' + info[3].replace('_', ' ') + ')' : '');
      if (big || this.k > 250) el('text', { x: p[0] + 7, y: p[1] - 5, class: 'apt-label' + (big ? ' mine' : '') }, o).textContent = ident;
    }
    for (const a of this.aircraft) {
      if (a.lat == null) continue;
      const p = this.toScreen(a.lon, a.lat);
      if (!inView(p)) continue;
      const sel = a.icao === this.selected;
      const g = el('g', { transform: `translate(${p[0]} ${p[1]}) rotate(${a.trk || 0})`, class: 'ac-g' }, o);
      // A wide transparent disc under the arrow: the click target (PICK_PX).
      el('circle', { r: 14, class: 'hit' }, g);
      el('path', { d: sel ? 'M0 -13 L9 10 L0 5 L-9 10 Z' : 'M0 -11 L7.5 8.5 L0 4.5 L-7.5 8.5 Z', class: 'ac' + (sel ? ' sel' : '') }, g);
      el('text', { x: p[0] + 12, y: p[1] + 4, class: 'ac-label' + (sel ? ' sel' : '') }, o)
        .textContent = a.callsign || a.icao;
    }
  }

  // The aircraft nearest to a pointer event, if within PICK_PX.
  nearest(e) {
    const r = this.svg.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    let best = null, bestD = PICK_PX;
    for (const a of this.aircraft) {
      if (a.lat == null) continue;
      const p = this.toScreen(a.lon, a.lat);
      const d = Math.hypot(p[0] - x, p[1] - y);
      if (d < bestD) { best = a; bestD = d; }
      // Its label counts too: a click on the name picks the aircraft.
      const wl = (a.callsign || a.icao).length * 7 + 14;
      if (!best && x > p[0] && x < p[0] + wl && y > p[1] - 10 && y < p[1] + 8) best = a;
    }
    return best;
  }

  _interact() {
    const svg = this.svg;
    let drag = null;
    svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      const r = svg.getBoundingClientRect();
      const [w, h] = this.size();
      const mx = e.clientX - r.left - w / 2, my = e.clientY - r.top - h / 2;
      const f = Math.exp(-e.deltaY * 0.0015);
      const k = Math.max(2, Math.min(20000, this.k * f));
      // Keep the point under the cursor where it is.
      this.center = [this.center[0] + mx / this.k - mx / k, this.center[1] + my / this.k - my / k];
      this.k = k;
      cancelAnimationFrame(this.anim);
      this.setFollow(false);
      this.draw();
    }, { passive: false });
    // Every press starts a drag; one that hardly moved is a click, and picks
    // the nearest aircraft within PICK_PX of it. The overlay is redrawn on
    // every poll, so the pick goes by distance, not by the element hit.
    svg.addEventListener('pointerdown', (e) => {
      drag = [e.clientX, e.clientY, this.center.slice(), false];
      svg.setPointerCapture(e.pointerId);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!drag) {
        svg.style.cursor = this.nearest(e) ? 'pointer' : '';
        return;
      }
      if (!drag[3] && Math.hypot(e.clientX - drag[0], e.clientY - drag[1]) < CLICK_PX) return;
      if (!drag[3]) { cancelAnimationFrame(this.anim); this.setFollow(false); }
      drag[3] = true;
      svg.style.cursor = 'grabbing';
      this.center = [drag[2][0] - (e.clientX - drag[0]) / this.k, drag[2][1] - (e.clientY - drag[1]) / this.k];
      this.draw();
    });
    svg.addEventListener('pointerup', (e) => {
      if (drag && !drag[3]) {
        const a = this.nearest(e);
        if (a) this.onPick(a.icao);
      }
      drag = null;
      svg.style.cursor = '';
    });
    svg.addEventListener('dblclick', () => { if (this.fit()) this.draw(); });
  }
}
