// The page's words, in Portuguese (the team's), Spanish (the venue's) and
// English. A key missing in a language falls back to Portuguese.

import { store } from './util.js';

const T = {
  pt: {
    title: 'GCD26 · Estação de solo', lang: 'PT', theme_dark: 'tema escuro', theme_light: 'tema claro', big: 'projetar: letras maiores',
    contact: 'EM CONTATO', nocontact: 'SEM CONTATO', wait: 'AGUARDANDO', offline: 'SEM SERVIDOR',
    mode_none: 'modo desconhecido', for: 'há', last_frame: 'último quadro',
    battery: 'Bateria', current: 'Corrente', tbat: 'T. bateria', text: 'T. externa', tsoc: 'T. processador',
    uptime: 'Ligado', time_sync: 'Hora', adsb_msgs: 'ADS-B', synced: 'sincronizada', unsynced: 'sem hora',
    nodata: 'sem dado', demo: 'MODO REPLAY: os comandos não chegam a nenhum satélite.',
    tab_ops: 'Operação', tab_adsb: 'ADS-B', tab_deploy: 'Implantação', tab_aocs: 'AOCS', tab_surv: 'Sobrevivência', tab_link: 'Enlace',
    utilities: 'Utilitários', util_hint: 'ping, hk, taxa, stream', side_hide: 'ocultar eventos e utilitários', side_show: 'mostrar eventos e utilitários', g_link: 'Enlace e telemetria', g_rate: 'Taxa do enlace', g_stream: 'Retratos de ADS-B',
    b_time: 'sincronizar hora', b_stream: 'stream 5 s', b_stop: 'parar',
    arm: 'Armar: burn-wire e AOCS (desarma em 15 s)', sent: 'enviado', no_server: 'sem conexão com o servidor',
    events: 'Eventos', f_all: 'todos', f_cmd: 'comandos', f_bad: 'falhas', f_mode: 'modos', f_tm: 'telemetria',
    modes: 'Modos do OBC (Figura 44)', in_mode: 'neste modo há', never: 'sem telemetria ainda',
    m_pre: 'espera de comandos', m_basic: 'burn-wire (5 s)', m_aocs: 'empresta o I²C', m_adsb: 'decodifica ADS-B',
    m_down: 'envia o resumo', m_surv: 'só o termostato',
    runs: 'Execuções', r_cmd: 'comando', r_sent: 'enviado', r_ack: 'ACK', r_in: 'no modo', r_out: 'saiu para', r_dur: 'duração',
    no_runs: 'Nenhum comando de modo ainda.',
    s_sent: 'Comando enviado', s_time: 'Hora sincronizada (SET_TIME)', s_mode: 'Modo aceito (SET_MODE)',
    s_entered: 'OBC no modo', s_dump: 'Decodificador ligado', s_tracks: 'Primeiros retratos', s_end: 'Fim da missão e downlink',
    s_back: 'De volta a PRE_TEST', s_burn: 'Burn-wire acionado (5 s)',
    adsb_lead: 'Um clique: a ground manda a hora, espera o ACK e só então pede MISSION_ADSB (ADR-0020). O OBC encerra a missão sozinho no fim do tempo dela (10 min em voo; menos na bancada, com MISSION_S) e passa ao downlink.',
    start_adsb: 'Iniciar missão ADS-B', end_adsb: 'Encerrar agora (downlink)', mission: 'Missão', elapsed: 'decorrido', remaining: 'restante', starting: 'iniciando a missão…', step: 'passo',
    stats: 'Estatísticas', legend_ac: 'aeronave', legend_ref: 'aeroporto citado pelo estimador', legend_apt: 'aeroporto', legend_city: 'área urbana', fit: 'enquadrar', fit_hint: 'aceso: o mapa acompanha as aeronaves sozinho; mover ou dar zoom desliga', show_steps: 'ver os passos da missão', map_hint: 'arraste para mover, roda para zoom, duplo clique enquadra', aircraft: 'Aeronaves', messages: 'Mensagens', p95: 'Latência p95', snap_recs: 'Registros no último retrato', snap_short: 'Último retrato',
    est_proc: 'Estimador', processed: 'processadas', rejected: 'rejeitadas', repeats: 'repetidas',
    map: 'Mapa', table: 'Aeronaves', detail: 'Detalhe da aeronave', pick: 'Clique numa aeronave no mapa ou na tabela.',
    c_icao: 'ICAO', c_cs: 'Indicativo', c_cat: 'Cat.', c_phase: 'Fase', c_alt: 'Alt. (ft)', c_gs: 'Vel. (kt)',
    c_origin: 'Origem', c_dest: 'Destino', c_comb: 'Combinada', cat_inferred: 'categoria inferida pela cinemática (ADR-0022)', local: 'Método local', pairs: 'Pares', combined: 'Conclusão combinada',
    none_yet: 'Nenhuma aeronave processada ainda.', adsb_off: 'Processamento ADS-B desligado: rode a interface com --adsb.',
    deploy_lead: 'BASIC_INTERMEDIATE aciona o burn-wire por 5 s, uma vez por entrada no modo, e o OBC volta sozinho a PRE_TEST. Exige armar.',
    ssh_title: 'Plano B: pela Pi, por SSH', ssh_lead: 'Se o rádio não alcançar o satélite: o mesmo pulso de 5 s, disparado na própria Pi pelo SSH (gama-deploy). Só a partir de PRE_TEST; o resultado vem do log do OBC, não do rádio. Exige armar.',
    ssh_host: 'Pi', ssh_start: 'Implantar pelo SSH', ssh_busy: 'acionando… (~6 s)', ssh_arm: 'Arme antes de implantar pelo SSH.',
    ssh_off: 'Desligado: rode a página com --pi-ssh HOST (make gs-web e ./gama mission já passam o PI_HOST do local.mk).',
    ssh_s_signal: 'Sinal entregue ao obcd', ssh_s_entered: 'OBC em BASIC_INTERMEDIATE', ssh_s_burn: 'GPIO do burn-wire acionado',
    start_deploy: 'Acionar implantação', aocs_lead: 'ADVANCED_AOCS empresta o I²C ao ESP32 do AOCS para a tarefa dele (ADR-0017); o OBC volta a PRE_TEST com o resultado, ou de imediato sem ESP32. Exige armar.',
    start_aocs: 'Iniciar tarefa do AOCS', attitude: 'Atitude (HK)', roll: 'Rolagem', pitch: 'Arfagem', yaw: 'Guinada',
    aocs_slot: 'A interface da equipe do AOCS entra aqui: os arquivos dela em ground/host/web/aocs/.',
    surv_lead: 'ENV_SURVIVAL deixa só o termostato. Sai pelo comando de fim de tarefa (task_done).',
    start_surv: 'Entrar em sobrevivência', end_surv: 'Sair (task_done)', temps: 'Temperaturas', power: 'Energia e temperatura',
    link: 'Enlace', rssi: 'RSSI', snr: 'SNR', acks: 'ACKs ok / falhos', latency: 'latência média',
    failed: 'comandos sem ACK', badcrc: 'quadros com CRC ruim', ground_radio: 'rádio da ground', rate: 'Taxa',
    ping_res: 'Último ping', ok: 'ok', fail: 'falhou', waiting: 'aguardando',
  },
  es: {
    title: 'GCD26 · Estación terrena', lang: 'ES', theme_dark: 'tema oscuro', theme_light: 'tema claro', side_hide: 'ocultar eventos y utilidades', side_show: 'mostrar eventos y utilidades', big: 'proyectar: letras más grandes',
    contact: 'EN CONTACTO', nocontact: 'SIN CONTACTO', wait: 'ESPERANDO', offline: 'SIN SERVIDOR',
    mode_none: 'modo desconocido', for: 'hace', last_frame: 'último cuadro',
    battery: 'Batería', current: 'Corriente', tbat: 'T. batería', text: 'T. externa', tsoc: 'T. procesador',
    uptime: 'Encendido', time_sync: 'Hora', synced: 'sincronizada', unsynced: 'sin hora', nodata: 'sin dato',
    demo: 'MODO REPLAY: los comandos no llegan a ningún satélite.',
    tab_ops: 'Operación', tab_deploy: 'Despliegue', tab_surv: 'Supervivencia', tab_link: 'Enlace',
    utilities: 'Utilidades', g_link: 'Enlace y telemetría', g_rate: 'Velocidad del enlace', g_stream: 'Instantáneas ADS-B',
    b_time: 'sincronizar hora', b_stop: 'detener', arm: 'Armar: burn-wire y AOCS (se desarma en 15 s)', sent: 'enviado',
    events: 'Eventos', f_all: 'todos', f_cmd: 'comandos', f_bad: 'fallas', f_mode: 'modos', f_tm: 'telemetría',
    modes: 'Modos del OBC (Figura 44)', in_mode: 'en este modo hace', runs: 'Ejecuciones',
    start_adsb: 'Iniciar misión ADS-B', end_adsb: 'Terminar ahora (downlink)', mission: 'Misión', elapsed: 'transcurrido', remaining: 'restante', starting: 'iniciando la misión…', step: 'paso',
    stats: 'Estadísticas', aircraft: 'Aeronaves', messages: 'Mensajes', map: 'Mapa', detail: 'Detalle de la aeronave', legend_ac: 'aeronave', legend_ref: 'aeropuerto citado por el estimador', legend_apt: 'aeropuerto', legend_city: 'área urbana', fit: 'encuadrar',
    c_cs: 'Indicativo', c_phase: 'Fase', c_origin: 'Origen', c_dest: 'Destino', c_comb: 'Combinada', cat_inferred: 'categoría inferida por la cinemática (ADR-0022)',
    ssh_title: 'Plan B: desde la Pi, por SSH', ssh_start: 'Desplegar por SSH', ssh_busy: 'activando… (~6 s)',
    start_deploy: 'Activar despliegue', start_aocs: 'Iniciar tarea del AOCS', start_surv: 'Entrar en supervivencia',
    attitude: 'Actitud (HK)', temps: 'Temperaturas', power: 'Energía y temperatura', link: 'Enlace',
  },
  en: {
    title: 'GCD26 · Ground station', lang: 'EN', theme_dark: 'dark theme', theme_light: 'light theme', side_hide: 'hide events and utilities', side_show: 'show events and utilities', big: 'projector: larger text',
    contact: 'IN CONTACT', nocontact: 'NO CONTACT', wait: 'WAITING', offline: 'NO SERVER',
    mode_none: 'unknown mode', for: 'for', last_frame: 'last frame',
    battery: 'Battery', current: 'Current', tbat: 'Battery T', text: 'External T', tsoc: 'Processor T',
    uptime: 'Uptime', time_sync: 'Time', synced: 'synced', unsynced: 'no time', nodata: 'no data',
    demo: 'REPLAY MODE: commands reach no satellite.',
    tab_ops: 'Operations', tab_deploy: 'Deployment', tab_surv: 'Survival', tab_link: 'Link',
    utilities: 'Utilities', g_link: 'Link and telemetry', g_rate: 'Link rate', g_stream: 'ADS-B snapshots',
    b_time: 'sync time', b_stop: 'stop', arm: 'Arm: burn-wire and AOCS (disarms in 15 s)', sent: 'sent',
    events: 'Events', f_all: 'all', f_cmd: 'commands', f_bad: 'failures', f_mode: 'modes', f_tm: 'telemetry',
    modes: 'OBC modes (Figure 44)', in_mode: 'in this mode for', runs: 'Runs',
    start_adsb: 'Start ADS-B mission', end_adsb: 'End now (downlink)', mission: 'Mission', elapsed: 'elapsed', remaining: 'remaining', starting: 'starting the mission…', step: 'step',
    stats: 'Statistics', aircraft: 'Aircraft', messages: 'Messages', map: 'Map', detail: 'Aircraft detail', legend_ac: 'aircraft', legend_ref: 'airport the estimator cites', legend_apt: 'airport', legend_city: 'urban area', fit: 'fit', map_hint: 'drag to pan, wheel to zoom, double-click fits',
    c_cs: 'Callsign', c_phase: 'Phase', c_origin: 'Origin', c_dest: 'Destination', c_comb: 'Combined', cat_inferred: 'category inferred from kinematics (ADR-0022)',
    ssh_title: 'Plan B: from the Pi, over SSH', ssh_lead: 'If the radio cannot reach the satellite: the same 5 s pulse, fired on the Pi itself over SSH (gama-deploy). Only from PRE_TEST; the result comes from the OBC log, not the radio. Needs arming.',
    ssh_host: 'Pi', ssh_start: 'Deploy over SSH', ssh_busy: 'firing… (~6 s)', ssh_arm: 'Arm before deploying over SSH.',
    ssh_off: 'Off: start the page with --pi-ssh HOST (make gs-web and ./gama mission pass local.mk\'s PI_HOST).',
    ssh_s_signal: 'Signal delivered to obcd', ssh_s_entered: 'OBC in BASIC_INTERMEDIATE', ssh_s_burn: 'Burn-wire GPIO driven',
    start_deploy: 'Fire deployment', start_aocs: 'Start AOCS task', start_surv: 'Enter survival',
    attitude: 'Attitude (HK)', temps: 'Temperatures', power: 'Power and temperature', link: 'Link',
  },
};

export const LANGS = ['pt', 'es', 'en'];
let lang = store('gs-lang') || 'pt';
if (!LANGS.includes(lang)) lang = 'pt';

export function t(key) {
  return (T[lang] && T[lang][key]) || T.pt[key] || key;
}

export function currentLang() { return lang; }

export function setLang(l) {
  if (!LANGS.includes(l)) return lang;
  lang = l;
  store('gs-lang', lang);
  document.documentElement.lang = lang === 'pt' ? 'pt-BR' : lang;
  return lang;
}

export function nextLang() {
  lang = LANGS[(LANGS.indexOf(lang) + 1) % LANGS.length];
  store('gs-lang', lang);
  document.documentElement.lang = lang === 'pt' ? 'pt-BR' : lang;
  return lang;
}

// Fills every [data-t] element with its word.
export function labels(root = document) {
  for (const el of root.querySelectorAll('[data-t]')) el.textContent = t(el.dataset.t);
}
