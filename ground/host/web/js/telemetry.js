// The bar on top: contact, the OBC's mode, the link and the housekeeping,
// visible from every tab.

import { $, esc, fmtAge, setHTML } from './util.js';
import { t } from './i18n.js';
import { since } from './state.js';

const STALE_S = 35;   // three missed HK periods (10 s) and some

function metric(label, value, unit, nodata) {
  return `<div class="m${nodata ? ' nodata' : ''}"><span class="lbl">${esc(label)}</span>` +
    `<b>${nodata ? '—' : esc(value) + (unit ? `<small>${esc(unit)}</small>` : '')}</b></div>`;
}

export function renderBar(state) {
  const st = state.st;
  // Contact, then the mode, then the link.
  let contact = `<span class="chip"><i></i>${esc(t('wait'))}</span>`;
  if (!state.connected) contact = `<span class="chip bad"><i></i>${esc(t('offline'))}</span>`;
  else if (st && st.contact === true) contact = `<span class="chip ok"><i></i>${esc(t('contact'))}</span>`;
  else if (st && st.contact === false) contact = `<span class="chip bad"><i></i>${esc(t('nocontact'))}</span>`;
  const mode = st && st.mode
    ? `<span class="chip mode">${esc(st.mode)} <small>${esc(t('for'))} ${esc(fmtAge(since(st.mode_age)))}</small></span>`
    : `<span class="chip"><i></i>${esc(t('mode_none'))}</span>`;
  const rssi = st && st.rssi && st.rssi.length ? st.rssi[st.rssi.length - 1] : null;
  const snr = st && st.snr && st.snr.length ? st.snr[st.snr.length - 1] : null;
  const age = st ? since(st.frame_age) : null;
  const linkCls = age == null ? '' : age < 15 ? ' ok' : age < 60 ? ' warn' : ' bad';
  const link = `<span class="chip${linkCls}"><i></i>${rssi == null ? '—' : esc(rssi) + ' dBm · ' + esc(snr) + ' dB'}` +
    ` <small>${esc(t('last_frame'))} ${esc(fmtAge(age))}</small></span>`;
  setHTML($('status'), contact + mode + link);

  const hk = st && st.hk;
  const stale = !hk || since(st.hk_age) > STALE_S;
  const div = (v, d, n) => (v == null ? null : (v / d).toFixed(n));
  const time = hk && hk.time_synced != null
    ? metric(t('time_sync'), hk.time_synced ? '✓ ' + t('synced') : '✗ ' + t('unsynced'), '', false)
    : metric(t('time_sync'), null, '', true);
  const html =
    metric(t('battery'), div(hk && hk.battery_mv, 1000, 2), 'V', !hk || hk.battery_mv == null) +
    metric(t('current'), hk && hk.current_ma, 'mA', !hk || hk.current_ma == null) +
    metric(t('tbat'), div(hk && hk.temp_bat_ccel, 100, 1), '°C', !hk || hk.temp_bat_ccel == null) +
    metric(t('tsoc'), div(hk && hk.temp_soc_ccel, 100, 1), '°C', !hk || hk.temp_soc_ccel == null) +
    metric(t('uptime'), hk && hk.uptime_s != null ? fmtAge(hk.uptime_s) : null, '', !hk || hk.uptime_s == null) +
    time +
    metric(t('adsb_msgs'), hk && hk.adsb_msgs != null ? hk.adsb_msgs.toLocaleString('pt-BR') : null,
      hk && hk.dump1090_up ? '▲' : '', !hk || hk.adsb_msgs == null);
  const box = $('hk');
  setHTML(box, html);
  box.classList.toggle('stale', stale);

  const banner = $('banner');
  if (st && st.source === 'replay') { banner.className = 'banner show demo'; banner.textContent = t('demo'); }
  else if (st && st.source === 'ssh' && !st.error) { banner.className = 'banner show demo'; banner.textContent = t('ssh_link'); }
  else if (st && st.error) { banner.className = 'banner show'; banner.textContent = st.error; }
  else if (st && st.adsb && st.adsb.error) { banner.className = 'banner show'; banner.textContent = 'ADS-B: ' + st.adsb.error; }
  else { banner.className = 'banner'; }
  // Over ssh gs_cli says "udp": the page names the link the operator has.
  const radio = st && st.source === 'ssh' ? 'ssh' : st && st.ground && st.ground.radio;
  const g = st && st.ground && st.ground.radio ? radio + ' · ' + (st.ground.profile || '') : (st ? st.source : '');
  $('source').textContent = g || '';
}
