// An offline map in SVG: the land from the pair database's region
// (/api/basemap, Natural Earth), the large and medium airports
// (/api/airports), and the aircraft with their tracks. No tiles, no
// network: it works where the competition is.
//
// World coordinates are longitude and Mercator latitude, both in degrees.
// Panning and zooming change one transform; only the few markers and
// labels, which keep their size on screen, are placed again.

const NS = 'http://www.w3.org/2000/svg';
const R2D = 180 / Math.PI;

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
    this.fitted = false;
    this.aircraft = [];
    this.refs = {};
    this.selected = null;
    this.airports = [];
    this.world = el('g', {}, svg);
    this.land = el('g', {}, this.world);
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

  fit() {
    const pts = [];
    for (const a of this.aircraft) {
      if (a.lat != null) pts.push([a.lon, mercY(a.lat)]);
      for (const p of a.track) pts.push([p[2], mercY(p[1])]);
    }
    if (!pts.length) return false;
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    const [w, h] = this.size();
    const spanX = Math.max(0.3, Math.max(...xs) - Math.min(...xs));
    const spanY = Math.max(0.3, Math.max(...ys) - Math.min(...ys));
    this.center = [(Math.max(...xs) + Math.min(...xs)) / 2, (Math.max(...ys) + Math.min(...ys)) / 2];
    this.k = Math.min(w / spanX, h / spanY) * 0.85;
    return true;
  }

  set(view, selected) {
    this.aircraft = view.aircraft || [];
    this.refs = view.airports || {};
    this.selected = selected;
    if (!this.fitted && this.fit()) this.fitted = true;
    this._tracks();
    this.draw();
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
      el('rect', { x: p[0] - r, y: p[1] - r, width: 2 * r, height: 2 * r, class: 'apt-ref' + (big ? ' mine' : '') }, o);
      if (big || this.k > 250) el('text', { x: p[0] + 7, y: p[1] - 5, class: 'apt-label' + (big ? ' mine' : '') }, o).textContent = ident;
    }
    for (const a of this.aircraft) {
      if (a.lat == null) continue;
      const p = this.toScreen(a.lon, a.lat);
      if (!inView(p)) continue;
      const g = el('g', { transform: `translate(${p[0]} ${p[1]}) rotate(${a.trk || 0})`, 'data-icao': a.icao, style: 'cursor:pointer' }, o);
      el('path', { d: 'M0 -8 L5.5 6 L0 3 L-5.5 6 Z', class: 'ac' + (a.icao === this.selected ? ' sel' : '') }, g);
      el('text', { x: p[0] + 9, y: p[1] + 4, class: 'ac-label', 'data-icao': a.icao, style: 'cursor:pointer' }, o)
        .textContent = a.callsign || a.icao;
    }
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
      this.draw();
    }, { passive: false });
    svg.addEventListener('pointerdown', (e) => {
      const hit = e.target.closest('[data-icao]');
      if (hit) { this.onPick(hit.dataset.icao); return; }
      drag = [e.clientX, e.clientY, this.center.slice()];
      svg.setPointerCapture(e.pointerId);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!drag) return;
      this.center = [drag[2][0] - (e.clientX - drag[0]) / this.k, drag[2][1] - (e.clientY - drag[1]) / this.k];
      this.draw();
    });
    svg.addEventListener('pointerup', () => { drag = null; });
    svg.addEventListener('dblclick', () => { if (this.fit()) this.draw(); });
  }
}
