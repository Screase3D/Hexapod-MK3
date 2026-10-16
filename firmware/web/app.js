'use strict';
// Hexapod MK3 – Weboberfläche (ohne externe Abhängigkeiten, läuft auch im AP-Modus offline)

const LEGS = ['vorne links', 'Mitte links', 'hinten links', 'vorne rechts', 'Mitte rechts', 'hinten rechts'];
const JOINTS = ['Coxa', 'Femur', 'Tibia'];
const NUM_SERVOS = 18;
const DEFAULT_SLOPE = 2000 / 180;
const LOCAL_PRIORITY_MS = 900;  // so lange hat eine lokale Eingabe Vorrang vor der Telemetrie
// Kennung dieser Oberfläche, beim Build eingesetzt (scripts/embed_web.py)
const UI_VERSION = '__HEXAPOD_UI_VERSION__';

const $ = (id) => document.getElementById(id);

const S = {
  ws: null,
  wsOpen: false,
  wsOpenedAt: 0,
  wsDownSince: Date.now(),  // seit wann keine Verbindung (Banner erst nach WS_BANNER_DELAY_MS)
  wsRetries: 0,
  wsStale: false,           // selbst geschlossen, weil nichts mehr kam
  wsLastClose: null,        // Beschreibung des letzten Abbruchs (geht nach dem Neuverbinden ins Protokoll)
  lastMsg: 0,
  tel: null,
  cal: new Array(NUM_SERVOS).fill(null),
  sel: 0,
  localUs: null,
  localTime: 0,
  tab: 'control',
  calView: 'cal',  // Kalibrieren (einzeln) oder Justieren (alle Gelenke)
  camWanted: false,
  hud: false,
  status: null,  // letzte Antwort von /api/status
  wifi: null,
  camera: null,
  torqueHintShown: false,
  statusTimer: null,
};

// ---------------------------------------------------------------- Hilfen ----

let toastTimer = null;
function toast(msg, isError = false) {
  const el = $('toast');
  el.textContent = msg;
  el.classList.toggle('error', isError);
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), isError ? 4000 : 2200);
}

function throttle(fn, ms) {
  let last = 0;
  let timer = null;
  let pending = null;
  return (...args) => {
    pending = args;
    const wait = ms - (Date.now() - last);
    if (wait <= 0) {
      last = Date.now();
      fn(...pending);
      pending = null;
    } else if (!timer) {
      timer = setTimeout(() => {
        timer = null;
        last = Date.now();
        if (pending) fn(...pending);
        pending = null;
      }, wait);
    }
  };
}

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const fmtDeg = (d) => (d > 0 ? '+' : '') + Number(d).toFixed(1);
const legLabel = (leg) => `Bein ${leg + 1} · ${LEGS[leg]}`;

function servoLabel(id) {
  const leg = Math.floor(id / 3);
  return `${legLabel(leg)} – ${JOINTS[id % 3]}`;
}

function isCalibrated(c) {
  return !!c && (c.center_us !== 1500 || Math.abs(Math.abs(c.us_per_deg) - DEFAULT_SLOPE) > 0.01);
}

// Kalibrierung: puls = center_us + (winkel - ref_deg) * us_per_deg
// (ref_deg = Gelenkwinkel der Montagestellung)
const refDeg = (c) => (c && typeof c.ref_deg === 'number' ? c.ref_deg : 0);

function usToDeg(id, us) {
  const c = S.cal[id];
  return c ? refDeg(c) + (us - c.center_us) / c.us_per_deg : 0;
}

function degToUs(c, deg) {
  return c.center_us + (deg - refDeg(c)) * c.us_per_deg;
}

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  };
  const res = await fetch(path, opts);
  let data = {};
  try { data = await res.json(); } catch (e) { /* leer */ }
  if (!res.ok || data.ok === false) throw new Error(data.msg || `HTTP ${res.status}`);
  return data;
}

// Umschalter (Segmente): aktiven Wert anzeigen bzw. Auswahl melden
function setSeg(id, value) {
  $(id).querySelectorAll('button').forEach((b) => {
    const on = b.dataset.value === value;
    b.classList.toggle('active', on);
    b.setAttribute(b.getAttribute('role') === 'tab' ? 'aria-selected' : 'aria-checked', on);
  });
}

function bindSeg(id, onPick) {
  $(id).querySelectorAll('button').forEach((b) => {
    b.onclick = () => {
      setSeg(id, b.dataset.value);
      onPick(b.dataset.value);
    };
  });
}

// Zustand immer mit Symbol und Text, nie nur als Farbe
const STATUS_ICON = { ok: 'i-check', warn: 'i-alert', bad: 'i-x' };
function statusEl(cls, text) {
  const span = document.createElement('span');
  span.className = `status ${cls}`;
  span.innerHTML = `<svg class="ic"><use href="#${STATUS_ICON[cls]}"/></svg><span></span>`;
  span.lastChild.textContent = text;
  return span;
}

// ------------------------------------------------------------- WebSocket ----

const WS_RETRY_FIRST_MS = 300;    // nach einem Abbruch sofort neu verbinden ...
const WS_RETRY_MS = 1500;         // ... danach in Abständen
const WS_BANNER_DELAY_MS = 2500;  // kurze Aussetzer nicht als „Keine Verbindung“ melden
const WS_STALE_MS = 5000;         // so lange nichts empfangen -> Verbindung hängt

// Keine Verbindung (länger als ein kurzer Aussetzer)
const wsDown = () => !S.wsOpen && Date.now() - S.wsDownSince >= WS_BANNER_DELAY_MS;

function describeClose(ev) {
  const now = Date.now();
  const open = ((now - S.wsOpenedAt) / 1000).toFixed(1);
  const idle = ((now - S.lastMsg) / 1000).toFixed(1);
  const codes = { 1000: 'normal', 1001: 'Seite verlassen', 1002: 'Protokollfehler', 1006: 'abgerissen', 1009: 'zu groß', 1011: 'Serverfehler' };
  const why = S.wsStale || !ev ? `${WS_STALE_MS / 1000} s keine Daten`
    : `${codes[ev.code] || 'Code'} ${ev.code}${ev.reason ? ' ' + ev.reason : ''}`;
  return `Verbindung getrennt (${why}) nach ${open} s, letzte Nachricht vor ${idle} s`;
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const ws = new WebSocket(proto + location.host + '/ws');
  S.ws = ws;
  ws.onopen = () => {
    S.wsOpen = true;
    S.wsOpenedAt = S.lastMsg = Date.now();
    S.wsDownSince = 0;
    S.wsRetries = 0;
    S.wsStale = false;
    renderConnection();
    if (S.wsLastClose) {  // Grund des letzten Abbruchs ins Ereignisprotokoll des Roboters
      send('client_log', { text: S.wsLastClose });
      S.wsLastClose = null;
    }
    send('cal_get');
    checkVersion();
    v3Tick();
    if (OTA.rebooting) location.reload();  // neue Firmware = neue Oberfläche
  };
  ws.onclose = (ev) => wsClosed(ws, ev);
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    S.lastMsg = Date.now();
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    onMessage(m);
  };
}

// Verbindung weg (oder hängt): Zustand zurücksetzen und neu verbinden
function wsClosed(ws, ev) {
  if (S.ws !== ws) return;
  ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;  // spätere Ereignisse der alten Verbindung ignorieren
  S.ws = null;
  if (S.wsOpen && !document.hidden) S.wsLastClose = describeClose(ev);  // Handy gesperrt: kein Fehler
  S.wsOpen = false;
  funStop();  // Choreografie nicht blind weiterlaufen lassen
  if (!S.wsDownSince) {
    S.wsDownSince = Date.now();
    setTimeout(renderConnection, WS_BANNER_DELAY_MS + 50);
  }
  renderConnection();
  setTimeout(connect, S.wsRetries++ ? WS_RETRY_MS : WS_RETRY_FIRST_MS);
}

const wsReady = () => S.wsOpen && S.ws && S.ws.readyState === WebSocket.OPEN;

function send(cmd, params = {}) {
  if (!wsReady()) {
    toast('Keine Verbindung zum Hexapod', true);
    return false;
  }
  S.ws.send(JSON.stringify({ cmd, ...params }));
  return true;
}

setInterval(() => {
  if (!S.wsOpen || !S.ws) return;
  if (Date.now() - S.lastMsg > WS_STALE_MS) {
    // Verbindung hängt -> sofort neu aufbauen: das Schließen einer hängenden Verbindung
    // (Abschluss-Handshake) kann sehr lange dauern, so lange liefen Befehle ins Leere
    S.wsStale = true;
    const ws = S.ws;
    wsClosed(ws, null);
    try { ws.close(); } catch (e) { /* egal */ }
    return;
  }
  if (S.ws.readyState !== WebSocket.OPEN) return;
  S.pingAt = performance.now();
  S.ws.send('{"cmd":"ping"}');
}, 1500);

function onMessage(m) {
  if (m.type === 'pong' && S.pingAt) {
    // Antwortzeit (geglättet): zeigt, wie flüssig sich der Roboter steuern lässt
    const rtt = performance.now() - S.pingAt;
    S.pingAt = 0;
    S.rtt = S.rtt ? S.rtt * 0.7 + rtt * 0.3 : rtt;
    renderConnection();
    return;
  }
  if (m.type === 'live') {
    v3Live(m);
    return;
  }
  if (m.type === 'telemetry') {
    S.tel = m;
    renderTelemetry();
    return;
  }
  if (m.type !== 'ack') return;
  if (m.calibration) {
    for (const c of m.calibration.servos) S.cal[c.id] = c;
    if (S.cal[1]) document.querySelectorAll('.asm-femur').forEach((e) => { e.textContent = refDeg(S.cal[1]); });
    renderLegs();
    renderEditorCal();
    renderManualLimits();
  }
  if (m.cal) {
    S.cal[m.cal.id] = m.cal;
    renderLegs();
    renderManualLimits();
    if (m.cal.id === S.sel) renderEditorCal();
  }
  if (m.cmd === 'pose') {  // kommt beim Ziehen im 60-ms-Takt
    if (m.msg) showPoseNote(m.msg);
    return;
  }
  if (m.ok === false) toast(m.msg || 'Fehler', true);
  else if (m.msg) toast(m.msg);
  if (['cal_save', 'cal_reload', 'cal_reset'].includes(m.cmd)) send('cal_get');
}

// --------------------------------------------------------------- Kopfzeile --

// short: Kurzform für schmale Bildschirme (Handy), sonst wie text
function setPill(id, text, cls = '', short = '') {
  const pill = $(id);
  const t = pill.querySelector('.ptext');
  t.textContent = text;
  if (short) t.dataset.short = short;
  else delete t.dataset.short;
  pill.className = 'pill ' + cls;
}

const RTT_SLOW_MS = 250;

// Vom Roboter ausgelassene Nachrichten (WLAN kam nicht nach), Verlauf der letzten Minute
function noteWsDrops(n) {
  if (typeof n !== 'number') return;
  const now = Date.now();
  const h = S.wsDrops || (S.wsDrops = []);
  if (h.length && n < h[h.length - 1][1]) h.length = 0;  // Neustart des Roboters
  h.push([now, n]);
  while (h.length > 2 && now - h[0][0] > 60000) h.shift();
}

function wsDropsPerMin() {
  const h = S.wsDrops;
  return S.wsOpen && h && h.length > 1 ? h[h.length - 1][1] - h[0][1] : 0;
}

function renderConnection() {
  if (!S.wsOpen) S.rtt = 0;
  const rtt = S.rtt ? Math.round(S.rtt) : 0;
  const drops = wsDropsPerMin();  // für alle Geräte zusammen – nur ein Hinweis, kein Urteil über diese Verbindung
  const slow = S.wsOpen && rtt > RTT_SLOW_MS;
  const down = wsDown();
  setPill('pill-conn', S.wsOpen ? (slow ? 'Verbindung langsam' : 'Verbunden') : (down ? 'Getrennt' : 'Verbinde …'),
    S.wsOpen ? (slow ? 'pill-warn' : 'pill-ok') : (down ? 'pill-bad' : 'pill-warn'));
  const rssi = S.tel && S.tel.wifi && S.tel.wifi.rssi;
  $('pill-conn').title = S.wsOpen
    ? `Antwortzeit ${rtt || '–'} ms${rssi ? ` · WLAN ${rssi} dBm (${signalBars(rssi)})` : ''}` +
      `${drops ? ` · Roboter hat in der letzten Minute ${drops} Nachrichten ausgelassen (WLAN eines Geräts kam nicht nach)` : ''}` +
      `${slow ? ' – Steuerung reagiert verzögert (WLAN-Empfang?)' : ''}`
    : '';
  $('sb-conn').classList.toggle('on', S.wsOpen);
  $('sb-conn-text').textContent = S.wsOpen ? `Verbunden · ${location.hostname}${rtt ? ` · ${rtt} ms` : ''}` : (down ? 'Getrennt' : 'Verbinde …');
  renderBanner();
}

function renderBanner() {
  const banner = $('banner');
  const text = $('banner-text');
  const action = $('banner-action');
  const t = S.tel;
  let msg = '';
  let cls = '';
  let act = null;

  if (wsDown()) {
    msg = 'Keine Verbindung zum Hexapod – verbinde neu …';
    cls = 'info';
  } else if (!S.wsOpen) {
    // kurzer Aussetzer: gleich wieder da, kein Banner
  } else if (t && t.servo.estop) {
    msg = 'NOT-AUS aktiv – alle Servos stromlos';
    cls = 'danger';
    act = { label: 'Freigeben', fn: () => send('estop_clear') };
  } else if (t && t.servo.inhibit) {
    msg = `Servos gesperrt: ${t.servo.inhibit}`;
    cls = 'danger';
  } else if (t && t.bat.state === 'critical') {
    msg = `Akku leer (${t.bat.v.toFixed(2)} V) – sofort laden!`;
    cls = 'danger';
  } else if (t && t.bat.state === 'warn') {
    msg = `Akku schwach (${t.bat.v.toFixed(2)} V) – bald laden`;
  } else if (t && t.wifi && t.wifi.mode === 'ap' && S.tab !== 'settings') {
    msg = 'Access-Point-Modus – noch nicht mit dem Heimnetz verbunden';
    cls = 'info';
    act = { label: 'WLAN einrichten', fn: () => showTab('settings') };
  }

  banner.hidden = !msg;
  banner.className = 'banner ' + cls;
  text.textContent = msg;
  action.hidden = !act;
  if (act) {
    action.textContent = act.label;
    action.onclick = act.fn;
  }
}

function renderTelemetry() {
  const t = S.tel;
  const sv = t.servo;
  v3Telemetry(t);
  noteWsDrops(t.ws_drop);
  rssiTelemetry(t);
  renderPad(t.pad);
  renderLogState(t.log);
  if (FPV.open) renderFpv(t);
  if (S.tab === 'control') markProfile();

  // Akku
  $('pill-bat').title = 'Akku';
  if (t.bat.state === 'off') setPill('pill-bat', 'Akku aus', 'pill-idle');
  else if (t.bat.state === 'usb') setPill('pill-bat', 'USB', 'pill-idle');
  else {
    setPill('pill-bat', `${t.bat.v.toFixed(2)} V · ${t.bat.pct}%`,
      { ok: 'pill-ok', warn: 'pill-warn', critical: 'pill-bad' }[t.bat.state] || '', `${t.bat.pct}%`);
    $('pill-bat').title = `Akku ${t.bat.v.toFixed(2)} V · ${t.bat.pct}%`;
  }

  // Servos
  if (sv.estop) setPill('pill-torque', 'NOT-AUS', 'pill-bad');
  else if (sv.torque) setPill('pill-torque', sv.active < NUM_SERVOS ? `Servos ${sv.active}/${NUM_SERVOS}` : 'Servos an', 'pill-on');
  else setPill('pill-torque', 'Servos aus');
  $('ctl-torque').checked = sv.torque;
  $('cal-torque').checked = sv.torque;

  const slew = $('ctl-slew');
  if (document.activeElement !== slew) {
    slew.value = sv.slew;
    $('ctl-slew-val').textContent = `${Math.round(sv.slew)}°/s`;
  }

  // IMU
  renderHud(t.imu);
  if (t.imu.ok) {
    $('imu-pitch').textContent = fmtDeg(t.imu.pitch) + '°';
    $('imu-roll').textContent = fmtDeg(t.imu.roll) + '°';
    $('horizon-line').style.transform =
      `translateY(${clamp(t.imu.pitch, -45, 45) * 0.9}px) rotate(${-t.imu.roll}deg)`;
    // Rohwerte zur Kontrolle (Körperachsen): waagerecht ist z ≈ +1 g, Nase hoch -> x > 0
    if (t.imu.acc) {
      const a = t.imu.acc.map((v) => (v >= 0 ? '+' : '') + v.toFixed(2)).join(' ');
      $('imu-raw').textContent = `a = ${a} g · ${t.imu.hz} Hz · ${t.imu.temp} °C · Lesefehler ${t.imu.errors}`;
    }
  } else {
    $('imu-pitch').textContent = 'kein Sensor';
    $('imu-roll').textContent = '–';
    $('imu-raw').textContent = t.imu.errors ? `Lesefehler ${t.imu.errors}` : '';
  }
  renderImuSettings(t.imu);
  renderCamStats(t);

  // Kalibrierung
  $('cal-dirty').hidden = !sv.cal_dirty;
  $('nav-cal-dot').hidden = !sv.cal_dirty;
  $('cal-save').classList.toggle('attention', sv.cal_dirty);
  if (S.tab === 'system') renderTiles();

  renderLegValues();
  renderEditorLive();
  renderManualValues();
  renderPose(t.motion);
  renderWalk(t.motion);
  renderBanner();
}

// ----------------------------------------------------------- Lagesensor -----

function renderImuSettings(imu) {
  // Kalibrierung: Fortschritt bzw. Ergebnis (in der Lage-Karte und den Einstellungen)
  let text = '';
  let cls = '';
  if (imu.cal === 'running') text = `Messe … ${imu.cal_pct || 0} % – Roboter ruhig halten`;
  else if (imu.cal) {
    text = imu.cal_msg || '';
    cls = imu.cal;
  }
  for (const id of ['imu-cal', 'imu-cal2']) {
    const el = $(id);
    if (!el) continue;
    el.hidden = !text;
    el.textContent = text;
    el.className = `small imu-cal ${cls}`;
  }
  const busy = imu.cal === 'running';
  for (const id of ['imu-level', 'imu-level2', 'imu-gyro']) if ($(id)) $(id).disabled = busy || !imu.ok;

  // Einbaulage (nicht überschreiben, während ausgewählt wird)
  if (imu.mount && document.activeElement !== $('imu-xdir')) $('imu-xdir').value = imu.mount;
  if (imu.flip !== undefined && document.activeElement !== $('imu-flip')) $('imu-flip').value = imu.flip ? '1' : '0';
  if (S.tab === 'camera' && imu.level) {
    const [r, p] = imu.level;
    fillKv($('imu-info'), [
      ['Nullpunkt', r || p ? `Sensor um Roll ${fmtDeg(r)}°, Nick ${fmtDeg(p)}° geneigt` : 'nicht gesetzt'],
      ['Gyro-Nullpunkt', imu.bias_src || '–'],
    ]);
  }
}

function bindImu() {
  const level = () => send('imu_cal', { level: true });
  if ($('imu-level')) $('imu-level').onclick = level;
  $('imu-level2').onclick = level;
  $('imu-gyro').onclick = () => send('imu_cal', { level: false });
  $('imu-level-reset').onclick = () => {
    if (confirm('Nullpunkt des Lagesensors löschen?')) send('imu_level_reset');
  };
  const mount = () => send('imu_mount', { x: $('imu-xdir').value, upside_down: $('imu-flip').value === '1' });
  $('imu-xdir').onchange = mount;
  $('imu-flip').onchange = mount;
}

// -------------------------------------------------------------- Beine -------

function buildLegs() {
  for (let leg = 0; leg < 6; ++leg) {
    const col = $(leg < 3 ? 'legs-left' : 'legs-right');
    const card = document.createElement('div');
    card.className = 'leg';
    card.innerHTML = `<div class="leg-title">Bein ${leg + 1} <small>${LEGS[leg]}</small></div>`;
    for (let j = 0; j < 3; ++j) {
      const id = leg * 3 + j;
      const b = document.createElement('button');
      b.className = 'joint';
      b.id = `joint-${id}`;
      b.innerHTML = `<span class="dot"></span><span>${JOINTS[j]}</span><span class="val">–</span>`;
      b.onclick = () => selectServo(id);
      card.appendChild(b);
    }
    col.appendChild(card);
  }
}

function renderLegs() {
  for (let id = 0; id < NUM_SERVOS; ++id) {
    const el = $(`joint-${id}`);
    el.classList.toggle('selected', id === S.sel);
    el.querySelector('.dot').classList.toggle('done', isCalibrated(S.cal[id]));
  }
  renderLegValues();
}

function renderLegValues() {
  if (!S.tel) return;
  for (let id = 0; id < NUM_SERVOS; ++id) {
    const val = $(`joint-${id}`).querySelector('.val');
    val.textContent = `${fmtDeg(S.tel.servo.deg[id])}°`;
    val.title = `${S.tel.servo.us[id]} µs`;
  }
}

// ------------------------------------------------------ Körperhaltung -------

const POSE_FIELDS = [
  // key, Beschriftung, Einheit, Grenzen aus /api/kinematics
  ['z', 'Höhe', 'mm', (l) => [l.height_min, l.height_max]],
  ['stance', 'Fußabstand', 'mm', (l) => [l.stance_min, l.stance_max]],
  ['x', 'Vor / zurück', 'mm', (l) => [-l.shift_max, l.shift_max]],
  ['y', 'Links / rechts', 'mm', (l) => [-l.shift_max, l.shift_max]],
  ['roll', 'Rollen', '°', (l) => [-l.tilt_max, l.tilt_max]],
  ['pitch', 'Nicken', '°', (l) => [-l.tilt_max, l.tilt_max]],
  ['yaw', 'Gieren', '°', (l) => [-l.yaw_max, l.yaw_max]],
];
const poseTouched = {};
const poseRange = {};  // erreichbarer Bereich je Größe bei den übrigen Werten (Telemetrie motion.range)
const poseSend = throttle((patch) => { send('pose', patch); posePending = {}; }, 60);
let posePending = {};

function buildPose(kinem) {
  setGaitLimits(kinem.gait_limits);
  const box = $('pose-sliders');
  box.innerHTML = '';
  $('pose-placeholder').hidden = !kinem.placeholder;
  for (const [key, label, unit, range] of POSE_FIELDS) {
    const [lo, hi] = range(kinem.limits);
    const row = document.createElement('label');
    row.className = 'slider-row';
    row.innerHTML = `<span>${label}</span><span class="pose-track"><input type="range" id="pose-${key}" step="1">` +
      `<i class="pose-band" id="pose-${key}-band" hidden></i></span><span class="mono" id="pose-${key}-val"></span>`;
    const input = row.querySelector('input');
    input.min = Math.floor(lo);
    input.max = Math.ceil(hi);
    input.oninput = () => {
      poseTouched[key] = Date.now();
      // nur bis zum Wert, den die Beine bei den übrigen Einstellungen noch erreichen
      const r = poseReach(key);
      if (r) input.value = clamp(+input.value, r[0], r[1]);
      $(`pose-${key}-val`).textContent = `${input.value} ${unit}`;
      markPoseLimit(key);
      posePending[key] = +input.value;
      poseSend(posePending);
    };
    box.appendChild(row);
  }
}

// Erreichbarer Bereich als ganze Reglerwerte, null = keine Einschränkung bekannt
function poseReach(key) {
  const r = poseRange[key];
  const input = $(`pose-${key}`);
  if (!r || !input) return null;
  const lo = Math.max(+input.min, Math.ceil(r[0] - 1e-6));
  const hi = Math.min(+input.max, Math.floor(r[1] + 1e-6));
  return lo <= hi ? [lo, hi] : null;
}

// Band unter dem Regler: erreichbarer Teil der Skala; Wert am Rand hervorheben
function markPoseLimit(key) {
  const input = $(`pose-${key}`);
  const band = $(`pose-${key}-band`);
  if (!input || !band) return;
  const r = poseReach(key);
  const min = +input.min, max = +input.max;
  const narrowed = r && (r[0] > min || r[1] < max);
  band.hidden = !narrowed;
  if (narrowed) {
    band.style.setProperty('--a', (r[0] - min) / (max - min));
    band.style.setProperty('--b', (r[1] - min) / (max - min));
  }
  const v = +input.value;
  const atEdge = narrowed && ((v >= r[1] && r[1] < max) || (v <= r[0] && r[0] > min));
  $(`pose-${key}-val`).classList.toggle('at-limit', !!atEdge);
  input.title = narrowed ? `erreichbar: ${r[0]} bis ${r[1]} (bei den übrigen Werten)` : '';
}

function renderPose(m) {
  if (!m) return;
  $('pose-mode').checked = m.mode === 'pose';
  for (const [key, , unit] of POSE_FIELDS) {
    const input = $(`pose-${key}`);
    if (m.range && Array.isArray(m.range[key])) poseRange[key] = m.range[key];
    else delete poseRange[key];
    if (!input) continue;
    if (Date.now() - (poseTouched[key] || 0) < LOCAL_PRIORITY_MS || document.activeElement === input) {
      markPoseLimit(key);
      continue;
    }
    input.value = Math.round(m.pose[key]);
    $(`pose-${key}-val`).textContent = `${Math.round(m.pose[key])} ${unit}`;
    markPoseLimit(key);
  }
  renderLevel(m.level);
  const warn = [];
  if (Date.now() < poseNote.until) warn.push(poseNote.text);
  const legs = (a) => `Bein${a.length > 1 ? 'e' : ''} ${a.map((l) => l + 1).join(', ')}`;
  if (m.unreachable && m.unreachable.length) {
    warn.push(`${legs(m.unreachable)} ${m.unreachable.length > 1 ? 'erreichen' : 'erreicht'} den Boden nicht`);
  }
  if (m.clamped && m.clamped.length) warn.push(`${legs(m.clamped)} an der Gelenkgrenze`);
  $('pose-warn').hidden = !warn.length;
  $('pose-warn').textContent = warn.join(' · ');
}

// Firmware hat die gewünschte Haltung begrenzt: Hinweis im Kasten, Toast höchstens alle 4 s
const poseNote = { text: '', until: 0, toastAt: 0 };
function showPoseNote(text) {
  const now = Date.now();
  if (now - poseNote.toastAt > 4000) {
    toast(text, true);
    poseNote.toastAt = now;
  }
  poseNote.text = text;
  poseNote.until = now + 6000;
  if (S.tel) renderPose(S.tel.motion);
}

let levelTouched = 0;
let levelFaultShown = false;

function renderLevel(l) {
  if (!l) return;
  if (Date.now() - levelTouched > LOCAL_PRIORITY_MS) $('level-on').checked = l.on;
  let info = '';
  if (l.active) info = `Roll ${fmtDeg(l.roll)}° · Nick ${fmtDeg(l.pitch)}°`;
  else if (l.on) info = 'wirkt im Stehen';
  $('level-info').textContent = info;
  $('level-fault').hidden = !l.fault;
  if (l.fault && !levelFaultShown) toast('Lageausgleich abgeschaltet – Einbaulage des Lagesensors prüfen', true);
  levelFaultShown = !!l.fault;
}

async function loadKinematics(attempt = 0) {
  try {
    const k = await api('/api/kinematics');
    if (k.leg && Array.isArray(k.mounts) && k.mounts.length === 6) V3.geo = { leg: k.leg, mounts: k.mounts };
    buildPose(k);
    if (S.tel) renderPose(S.tel.motion);
  } catch (e) {
    // beim Laden der Seite hat der ESP nur wenige Verbindungen frei -> später nochmal
    if (attempt < 20) setTimeout(() => loadKinematics(attempt + 1), 3000);
  }
}

function bindPose() {
  $('pose-mode').onchange = (e) => {
    const on = e.target.checked;
    if (!send('mode', { mode: on ? 'pose' : 'manual' })) e.target.checked = !on;
    else if (on) torqueHint();
  };
  $('level-on').onchange = (e) => {
    levelTouched = Date.now();
    const on = e.target.checked;
    if (!send('leveling', { on })) e.target.checked = !on;
  };
  $('pose-reset').onclick = () => {
    for (const k of Object.keys(poseTouched)) delete poseTouched[k];
    posePending = {};
    send('pose_reset');
  };
}

// ---------------------------------------------------------------- Laufen ----

const WALK_SEND_MS = 100;  // Fahrbefehl alle 100 ms (Firmware hält nach 600 ms ohne Befehl an)
const WALK_KEYS = {
  KeyW: [1, 0, 0], KeyS: [-1, 0, 0], KeyA: [0, 1, 0], KeyD: [0, -1, 0], KeyQ: [0, 0, 1], KeyE: [0, 0, -1],
};
const W = {
  joyMove: [0, 0],  // vx, vy aus dem Joystick (normiert)
  joyTurn: 0,
  keys: new Set(),
  timer: null,
  last: [0, 0, 0],
  scale: 0.8,
  touched: {},
};

function sendQuiet(cmd, params = {}) {
  if (!wsReady()) return false;
  S.ws.send(JSON.stringify({ cmd, ...params }));
  return true;
}

function walkVector() {
  let [vx, vy] = W.joyMove;
  let turn = W.joyTurn;
  for (const code of W.keys) {
    const k = WALK_KEYS[code];
    vx += k[0];
    vy += k[1];
    turn += k[2];
  }
  const s = W.scale;
  return [clamp(vx, -1, 1) * s, clamp(vy, -1, 1) * s, clamp(turn, -1, 1) * s];
}

function walkTick() {
  const v = walkVector();
  const moving = v.some((x) => Math.abs(x) > 1e-3);
  sendQuiet('walk', { vx: +v[0].toFixed(3), vy: +v[1].toFixed(3), turn: +v[2].toFixed(3) });
  W.last = v;
  if (!moving) {
    clearInterval(W.timer);
    W.timer = null;
  }
}

// Bei jeder Änderung sofort senden, danach regelmäßig, solange gefahren wird
function walkChanged() {
  walkTick();
  if (!W.timer && W.last.some((x) => Math.abs(x) > 1e-3)) {
    W.timer = setInterval(walkTick, WALK_SEND_MS);
    torqueHint();
    const m = S.tel && S.tel.motion;
    if (m && m.mode === 'pose' && m.posture && m.posture !== 'standing') {
      toast('Erst aufstehen – im Sitzen läuft der Roboter nicht.');
    } else if (m && m.arms && m.arms !== 'down') {
      toast('Vorderbeine sind oben – erst absetzen, dann laufen.');
    }
  }
}

function walkStopAll() {
  W.joyMove = [0, 0];
  W.joyTurn = 0;
  W.keys.clear();
  document.querySelectorAll('.joy').forEach((j) => {
    j.classList.remove('active');
    j.querySelector('.joy-knob').style.transform = '';
  });
  sendQuiet('stop');
  walkChanged();
}

// Totzone und Kennlinie: kleine Ausschläge fein, voller Ausschlag = 1
function joyShape(v) {
  const a = Math.abs(v);
  const dead = 0.1;
  if (a < dead) return 0;
  return Math.sign(v) * Math.pow(Math.min(1, (a - dead) / (1 - dead)), 1.5);
}

// Form zur Laufzeit: .joy-h = waagerechter Schieber (nur x), sonst rund (x, y)
function makeJoystick(el, onChange) {
  const knob = el.querySelector('.joy-knob');
  let pid = null;
  const move = (e) => {
    const horizontal = el.classList.contains('joy-h');
    const r = el.getBoundingClientRect();
    const k = knob.offsetWidth / 2;
    const maxX = r.width / 2 - k - 4;
    let dx = e.clientX - (r.left + r.width / 2);
    let dy = horizontal ? 0 : e.clientY - (r.top + r.height / 2);
    if (horizontal) {
      dx = clamp(dx, -maxX, maxX);
    } else {
      const d = Math.hypot(dx, dy);
      if (d > maxX) {
        dx *= maxX / d;
        dy *= maxX / d;
      }
    }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    onChange(joyShape(dx / maxX), horizontal ? 0 : joyShape(dy / maxX));
  };
  const end = (e) => {
    if (e.pointerId !== pid) return;
    pid = null;
    el.classList.remove('active');
    knob.style.transform = '';
    onChange(0, 0);
  };
  el.addEventListener('pointerdown', (e) => {
    if (pid !== null) return;
    if (e.pointerType === 'touch' && navigator.vibrate) navigator.vibrate(8);  // kurzes Antippen spürbar
    pid = e.pointerId;
    el.setPointerCapture(pid);
    el.classList.add('active');
    move(e);
    e.preventDefault();
  });
  el.addEventListener('pointermove', (e) => { if (e.pointerId === pid) move(e); });
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
  el.addEventListener('lostpointercapture', end);
}

// R/F: Körper in 5-mm-Schritten höher/tiefer (wie der Schieber „Höhe“)
function nudgeHeight(dir) {
  const input = $('pose-z');
  if (!input) return;
  const z = clamp(+input.value + 5 * dir, +input.min, +input.max);
  input.value = z;
  input.dispatchEvent(new Event('input'));
}

// ------------------------------------------------- Körper / Vorderbeine -----

const MOVE = {
  mode: 'walk',                 // walk | body | arms
  stickL: [0, 0], stickR: [0, 0],  // Joysticks (x rechts, y runter), normiert
  keys: new Set(),
  timer: null,
  sent: '',                     // zuletzt gesendeter Befehl (für das Null-Telegramm)
  mirror: false,
};
const BODY_KEYS = { KeyW: [1, 0, 0], KeyS: [-1, 0, 0], KeyA: [0, 1, 0], KeyD: [0, -1, 0], KeyQ: [0, 0, 1], KeyE: [0, 0, -1] };
const MOVE_LABELS = {
  walk: ['Fahren', '↶ Drehen ↷'],
  body: ['Verschieben', 'Neigen'],
  arms: ['linkes Bein', 'rechtes Bein'],
  armsMirror: ['beide Beine', '↕ ausstrecken'],
};

function setMoveLabels() {
  const [l, r] = MOVE_LABELS[MOVE.mode === 'arms' && MOVE.mirror ? 'armsMirror' : MOVE.mode];
  $('joy-move-label').textContent = l;
  $('joy-turn-label').textContent = r;
  $('joy-move').setAttribute('aria-label', l);
  $('joy-turn').setAttribute('aria-label', r);
}

// aktueller Befehl im Körper- bzw. Vorderbein-Modus
function moveCommand() {
  const [lx, ly] = MOVE.stickL;
  const [rx, ry] = MOVE.stickR;
  if (MOVE.mode === 'body') {
    let x = -ly, y = -lx, yaw = 0;
    for (const code of MOVE.keys) {
      const k = BODY_KEYS[code];
      x += k[0];
      y += k[1];
      yaw += k[2];
    }
    // rechts: nach rechts = rechte Seite runter (Rollen +), nach oben = Nase hoch
    return ['body', { x: clamp(x, -1, 1), y: clamp(y, -1, 1), roll: rx, pitch: -ry, yaw: clamp(yaw, -1, 1) }];
  }
  // Vorderbeine: Der Fuß folgt der Joystick-Richtung (von hinten gesehen): links/rechts
  // schwenkt, hoch/runter hebt. Firmware: x + = nach innen/vorne, also beim rechten
  // Bein umgekehrt. Gespiegelt: links bewegt beide, rechts ↑/↓ = ausstrecken/heranziehen.
  const l = [lx, -ly];
  let r = [-rx, -ry];
  let reach = 0;
  if (MOVE.mirror) {
    r = l;
    reach = -ry;
  }
  return ['arm', { lx: l[0], lz: l[1], rx: r[0], rz: r[1], reach }];
}

function moveTick() {
  const [cmd, params] = moveCommand();
  const active = Object.values(params).some((v) => Math.abs(v) > 1e-3);
  for (const k of Object.keys(params)) params[k] = +params[k].toFixed(3);
  sendQuiet(cmd, params);
  MOVE.sent = cmd;
  if (!active) {
    clearInterval(MOVE.timer);
    MOVE.timer = null;
  }
}

function moveChanged() {
  moveTick();
  if (!MOVE.timer && Object.values(moveCommand()[1]).some((v) => Math.abs(v) > 1e-3)) {
    MOVE.timer = setInterval(moveTick, WALK_SEND_MS);
  }
}

function moveStopAll() {
  MOVE.stickL = [0, 0];
  MOVE.stickR = [0, 0];
  MOVE.keys.clear();
  clearInterval(MOVE.timer);
  MOVE.timer = null;
  if (MOVE.sent) sendQuiet(MOVE.sent, MOVE.sent === 'body' ? { x: 0, y: 0, roll: 0, pitch: 0, yaw: 0 } : { lx: 0, lz: 0, rx: 0, rz: 0, reach: 0 });
  MOVE.sent = '';
}

function onStick(which, x, y) {
  if (MOVE.mode === 'walk') {
    if (which === 'L') W.joyMove = [-y, -x];  // hoch = vor, links = +y
    else W.joyTurn = -x;                      // links = Drehen gegen den Uhrzeigersinn
    walkChanged();
    return;
  }
  if (which === 'L') MOVE.stickL = [x, y];
  else MOVE.stickR = [x, y];
  moveChanged();
}

function setMoveMode(mode) {
  if (!['walk', 'body', 'arms'].includes(mode)) mode = 'walk';
  if (W.timer || W.keys.size) walkStopAll();
  moveStopAll();
  document.querySelectorAll('.joy').forEach((j) => {
    j.classList.remove('active');
    j.querySelector('.joy-knob').style.transform = '';
  });
  MOVE.mode = mode;
  setSeg('move-mode', mode);
  setSeg('fpv-mode', mode);
  $('joy-turn').classList.toggle('joy-h', mode === 'walk');
  $('fpv-joy-r').classList.toggle('joy-h', mode === 'walk');
  setMoveLabels();
  $('walk-settings').hidden = mode !== 'walk';
  $('body-settings').hidden = mode !== 'body';
  $('arms-settings').hidden = mode !== 'arms';
  $('arms-buttons').hidden = mode !== 'arms';
  $('posture-buttons').hidden = mode === 'arms';
  try { localStorage.setItem('moveMode', mode); } catch (e) { /* egal */ }
  if (S.tel) renderWalk(S.tel.motion);
  if (FPV.open && S.tel) renderFpv(S.tel);
}

function bindMove() {
  bindSeg('move-mode', setMoveMode);
  makeJoystick($('joy-move'), (x, y) => onStick('L', x, y));
  makeJoystick($('joy-turn'), (x, y) => onStick('R', x, y));
  $('arms-toggle').onclick = () => {
    const m = S.tel && S.tel.motion;
    const up = !(m && m.arms && m.arms !== 'down');
    send('arms', { up });
  };
  $('arms-wave').onclick = () => send('wave');
  $('alive-on').onchange = (e) => {
    aliveTouched = Date.now();
    if (!send('alive', { on: e.target.checked })) e.target.checked = !e.target.checked;
  };
  $('arms-mirror').onchange = (e) => {
    MOVE.mirror = e.target.checked;
    setMoveLabels();
    moveChanged();
  };
  document.querySelectorAll('[data-gesture]').forEach((b) => {
    b.onclick = () => send('gesture', { name: b.dataset.gesture });
  });
  let mode = 'walk';
  try { mode = localStorage.getItem('moveMode') || 'walk'; } catch (e) { /* kein Speicher */ }
  setMoveMode(mode);
}

function walkKey(e, down) {
  const height = e.code === 'KeyR' || e.code === 'KeyF';
  if (S.tab !== 'control' || !(e.code in WALK_KEYS || e.code === 'Space' || height)) return false;
  const el = document.activeElement;
  const tag = (el && el.tagName) || '';
  if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return false;
  // Leertaste auf einem per Tastatur (Tab) gewählten Knopf: Knopf auslösen statt Stopp.
  // Nach einem Mausklick behält der Knopf zwar den Fokus, dann bleibt Leertaste = Stopp.
  if (e.code === 'Space' && el && el.closest && el.closest('button, a, summary, [role="button"], [role="radio"], [role="tab"]') &&
      el.matches(':focus-visible')) return false;
  if (height) {
    if (down) nudgeHeight(e.code === 'KeyR' ? 1 : -1);
    return true;
  }
  if (MOVE.mode === 'body') {
    if (!(e.code in BODY_KEYS)) return false;
    if (down) {
      if (MOVE.keys.has(e.code)) return true;
      MOVE.keys.add(e.code);
    } else {
      MOVE.keys.delete(e.code);
    }
    moveChanged();
    return true;
  }
  if (MOVE.mode === 'arms') return false;
  if (e.code === 'Space') {
    if (down) walkStopAll();
  } else if (down) {
    if (W.keys.has(e.code)) return true;  // Tastenwiederholung
    W.keys.add(e.code);
    walkChanged();
  } else {
    W.keys.delete(e.code);
    walkChanged();
  }
  return true;
}

const GAIT_TOUCH_MS = 1500;
const gaitSend = throttle((patch) => send('gait', patch), 150);

function setGaitLimits(l) {
  if (!l) return;
  const sw = $('gait-swing');
  sw.min = l.swing_min;
  sw.max = l.swing_max;
  const h = $('gait-height');
  h.min = l.height_min;
  h.max = l.height_max;
}

function renderGaitLabels() {
  $('gait-swing-val').textContent = `${(+$('gait-swing').value).toFixed(2)} s`;
  $('gait-height-val').textContent = `${$('gait-height').value} mm`;
  $('walk-scale-val').textContent = `${Math.round(W.scale * 100)} %`;
}

const ARMS_TEXT = {
  shift_back: 'verlagert Gewicht …', raise: 'hebt Vorderbeine …', up: 'Vorderbeine oben',
  lower: 'senkt Vorderbeine …', shift_forward: 'verlagert Gewicht …',
};
const GESTURE_TEXT = { circle: 'kreist', nod: 'nickt', shake: 'schüttelt', pushup: 'Liegestütz', dance: 'tanzt' };
const ALIVE_TEXT = {
  look: 'schaut sich um', shift: 'verlagert das Gewicht', tilt: 'legt den Kopf schief',
  lift: 'hebt ein Bein', tap: 'tippt mit dem Fuß', shuffle: 'setzt die Füße neu',
  yawn: 'gähnt', sigh: 'seufzt', stretch: 'streckt sich',
};
let aliveTouched = 0;
const MOOD_TEXT = { awake: 'wach', bored: 'gelangweilt', asleep: 'schläft – Steuern weckt ihn' };
const LIFE = { touched: 0 };

// Einstellungen „Lebendig wirken“ und Stimmung
function renderLife(l) {
  if (!l) return;
  if (Date.now() - LIFE.touched > LOCAL_PRIORITY_MS) {
    document.querySelectorAll('[data-life]').forEach((i) => { i.checked = !!l[i.dataset.life]; });
    $('life-sleep').value = String(l.sleep_min);
    if ($('life-sleep').value !== String(l.sleep_min)) {  // eigener Wert (per API)
      $('life-sleep').add(new Option(`${l.sleep_min} min`, String(l.sleep_min)));
      $('life-sleep').value = String(l.sleep_min);
    }
  }
  $('life-sleep').disabled = !l.bored;
  if (Date.now() - LIFE.touched > LOCAL_PRIORITY_MS && l.lift_sens) $('life-sens').value = String(l.lift_sens);
  $('life-sens').disabled = !l.lift;
  $('life-mood').textContent = l.flail ? 'Stimmung: zappelt (hochgehoben?)' : `Stimmung: ${MOOD_TEXT[l.mood] || l.mood}`;
}

function bindLife() {
  document.querySelectorAll('[data-life]').forEach((i) => {
    i.onchange = () => {
      LIFE.touched = Date.now();
      if (!send('life', { [i.dataset.life]: i.checked })) i.checked = !i.checked;
    };
  });
  $('life-sens').onchange = (e) => {
    LIFE.touched = Date.now();
    send('life', { lift_sens: +e.target.value });
  };
  $('life-sleep').onchange = (e) => {
    LIFE.touched = Date.now();
    send('life', { sleep_min: +e.target.value });
  };
}

function renderAlive(m) {
  const a = m.alive;
  if (!a) return;
  if (Date.now() - aliveTouched > LOCAL_PRIORITY_MS) $('alive-on').checked = a.on;
  // was er gerade tut (die Stimmung steht davor)
  let info = '';
  if (a.act) info = ALIVE_TEXT[a.act] || a.act;
  else if (a.on && !(m.mode === 'pose' && m.posture === 'standing')) info = '„Lebendig“ wirkt im Stand';
  $('alive-info').textContent = info;
  renderLife(m.life);
}

function renderWalk(m) {
  if (!m) return;
  renderAlive(m);
  const g = m.gait || {};
  const st = $('walk-state');
  const posture = m.mode === 'pose' ? m.posture : null;
  const busy = posture === 'standing_up' || posture === 'sitting_down';
  const POSTURE_TEXT = { sitting: 'sitzt', standing_up: 'steht auf …', sitting_down: 'setzt sich …' };
  const arms = m.mode === 'pose' && m.arms ? m.arms : 'down';
  const life = m.life || {};
  st.title = '';
  if (m.mode !== 'pose') {
    st.textContent = 'IK aus';
    st.className = 'badge badge-muted';
  } else if (life.flail) {
    st.textContent = 'zappelt';
    st.className = 'badge badge-warn';
  } else if (life.mood === 'asleep' && (posture === 'sitting' || posture === 'sitting_down')) {
    st.textContent = posture === 'sitting' ? 'schläft' : 'schläft ein …';
    st.className = 'badge badge-muted';
    st.title = 'Fahren, Körper oder Gesten wecken ihn';
  } else if (arms !== 'down') {
    st.textContent = ARMS_TEXT[arms] || arms;
    st.className = arms === 'up' ? 'badge badge-on' : 'badge badge-warn';
  } else if (posture && posture !== 'standing') {
    st.textContent = POSTURE_TEXT[posture] || posture;
    st.className = busy ? 'badge badge-warn' : 'badge badge-muted';
  } else if (m.gesture) {
    st.textContent = GESTURE_TEXT[m.gesture] || m.gesture;
    st.className = 'badge badge-on';
  } else if (g.walking) {
    st.textContent = 'läuft';
    st.className = 'badge badge-on';
  } else {
    st.textContent = 'steht';
    st.className = 'badge badge-muted';
  }
  const powered = S.tel && S.tel.servo.torque && !S.tel.servo.estop;
  $('walk-stand').disabled = !powered || busy || posture === 'standing';
  $('walk-stand').title = powered ? '' : 'Erst Torque einschalten';
  $('walk-sit').disabled = !powered || busy || m.mode !== 'pose' || posture === 'sitting';
  const standing = powered && m.mode === 'pose' && posture === 'standing';

  // Vorderbeine: heben nur im Stand ohne Laufen, absetzen jederzeit
  const raising = arms === 'shift_back' || arms === 'raise' || arms === 'up';
  $('arms-toggle').textContent = raising ? 'Vorderbeine absetzen' : 'Vorderbeine heben';
  $('arms-toggle').disabled = raising ? false : (arms !== 'down' || !standing || !!g.walking);
  $('arms-toggle').title = standing ? '' : 'Erst aufstehen';
  $('arms-wave').disabled = arms !== 'up';

  // Gesten: nur im Stand, Vorderbeine unten
  document.querySelectorAll('[data-gesture]').forEach((b) => {
    const name = b.dataset.gesture;
    b.classList.toggle('running', m.gesture === name);
    b.disabled = name === 'stop' ? !m.gesture : (!standing || arms !== 'down');
  });
  $('body-info').textContent = m.offset_limited && arms === 'down' ? 'An der Reichweitengrenze – Körper lässt sich nicht weiter bewegen' : '';
  $('arms-info').textContent = m.offset_limited && arms === 'up' ? 'So weit reicht das Bein nicht – es bleibt am Rand stehen' : '';

  // Tempo durch die Beinreichweite begrenzt?
  let limit = '';
  if (m.mode === 'pose' && posture === 'standing' && MOVE.mode === 'walk') {
    if (arms !== 'down') {
      limit = 'Vorderbeine sind oben – Laufen ist gesperrt.';
    } else if (g.nominal > 0) {
      const fwd = (g.max_speed || 0) / g.nominal;
      const side = (g.max_side || 0) / g.nominal;
      if (g.max_speed === 0 && g.max_side === 0) {
        limit = 'In dieser Haltung kann er nicht laufen – die Beine haben keinen Spielraum für Schritte. Höhe oder Standbreite näher an die Grundstellung bringen.';
      } else if (fwd < 0.6 || side < 0.6) {
        limit = `Beinreichweite begrenzt das Tempo: vorwärts ${Math.round(fwd * 100)} %, seitwärts ${Math.round(side * 100)} % – `
          + 'Höhe oder Standbreite näher an die Grundstellung bringen.';
      }
    }
  }
  $('walk-limit').hidden = !limit;
  $('walk-limit').textContent = limit;

  const swing = g.swing || 0;
  document.querySelectorAll('#walk-legs [data-leg]').forEach((d) => {
    d.classList.toggle('up', !!(swing & (1 << +d.dataset.leg)));
  });

  const now = Date.now();
  const sync = (id, key, val) => {
    const el = $(id);
    if (val === undefined || document.activeElement === el || now - (W.touched[key] || 0) < GAIT_TOUCH_MS) return;
    el.value = val;
  };
  if (g.type !== undefined && now - (W.touched.type || 0) >= GAIT_TOUCH_MS) setSeg('gait-type', g.type);
  sync('gait-swing', 'swing_s', g.swing_s);
  sync('gait-height', 'step_height', g.step_height);
  renderGaitLabels();

  const info = [];
  if (m.mode === 'pose' && g.max_speed !== undefined && MOVE.mode === 'walk') {
    if (g.walking) {
      info.push(`${g.vx} / ${g.vy} mm/s · ${g.wz} °/s`);
      if (g.cadence) info.push(`Schritt ${(g.swing_s * g.cadence).toFixed(2).replace('.', ',')} s`);
    }
    info.push(`max. ${g.max_speed} mm/s · ${g.max_turn} °/s`);
    if (g.climb_height && (g.type === 'climb' || g.active === 'climb')) info.push(`Fußhub ${g.climb_height} mm`);
    if (g.switching) {
      const climb = g.active !== g.type && (g.type === 'climb' || g.active === 'climb');
      info.push(climb ? 'Gangart wechselt – hält kurz an'
        : g.active !== g.type ? 'Gangart wechselt – passt das Tempo an' : 'Gangart wechselt – Beine finden in den Takt');
    }
  }
  $('walk-info').textContent = info.join(' · ');
}

function bindWalk() {
  try {
    const s = parseFloat(localStorage.getItem('walkScale'));
    if (s >= 0.1 && s <= 1) W.scale = s;
  } catch (e) { /* kein Speicher */ }
  $('walk-scale').value = Math.round(W.scale * 100);

  $('walk-stand').onclick = () => send('stand');
  $('walk-sit').onclick = () => {
    walkStopAll();
    send('sit');
  };
  $('walk-scale').oninput = (e) => {
    W.scale = +e.target.value / 100;
    renderGaitLabels();
    try { localStorage.setItem('walkScale', W.scale); } catch (err) { /* egal */ }
    if (W.timer) walkTick();
  };
  bindSeg('gait-type', (type) => {
    W.touched.type = Date.now();
    send('gait', { type });
  });
  $('gait-swing').oninput = (e) => {
    W.touched.swing_s = Date.now();
    renderGaitLabels();
    gaitSend({ swing_s: +e.target.value });
  };
  $('gait-height').oninput = (e) => {
    W.touched.step_height = Date.now();
    renderGaitLabels();
    gaitSend({ step_height: +e.target.value });
  };
  document.addEventListener('keyup', (e) => { if (walkKey(e, false)) e.preventDefault(); });
  // Fenster verliert den Fokus / Tab im Hintergrund: sofort anhalten
  window.addEventListener('blur', () => {
    if (W.timer || W.keys.size) walkStopAll();
    if (MOVE.timer || MOVE.keys.size) moveStopAll();
  });
  document.addEventListener('visibilitychange', () => { if (document.hidden && W.timer) walkStopAll(); });
  renderGaitLabels();
}

// ------------------------------------------------- Manuelle Steuerung -------

const manualTouched = new Array(NUM_SERVOS).fill(0);  // Zeit der letzten Eingabe
const manualSend = Array.from({ length: NUM_SERVOS },
  (_, id) => throttle((deg) => send('servo_deg', { id, deg }), 50));

function buildManual() {
  const grid = $('manual-grid');
  for (let leg = 0; leg < 6; ++leg) {
    const box = document.createElement('div');
    box.className = 'manual-leg';
    box.style.gridColumn = leg < 3 ? 1 : 2;
    box.style.gridRow = (leg % 3) + 2;  // Zeile 1: „VORNE“
    box.innerHTML = `<div class="leg-title">Bein ${leg + 1} <small>${LEGS[leg]}</small></div>`;
    for (let j = 0; j < 3; ++j) {
      const id = leg * 3 + j;
      const row = document.createElement('label');
      row.className = 'manual-row';
      row.innerHTML = `<span>${JOINTS[j]}</span>` +
        `<input type="range" id="man-${id}" min="-90" max="90" step="1" value="0">` +
        `<span class="mono" id="man-val-${id}">–</span>`;
      const input = row.querySelector('input');
      input.oninput = () => {
        manualTouched[id] = Date.now();
        $(`man-val-${id}`).textContent = `${fmtDeg(+input.value)}°`;
        manualSend[id](+input.value);
        torqueHint();
      };
      box.appendChild(row);
    }
    grid.appendChild(box);
  }
  $('manual-assembly').onclick = () => send('all_assembly');
}

function renderManualLimits() {
  for (let id = 0; id < NUM_SERVOS; ++id) {
    const c = S.cal[id];
    if (!c) continue;
    const input = $(`man-${id}`);
    input.min = Math.ceil(c.min_deg);
    input.max = Math.floor(c.max_deg);
  }
}

function renderManualValues() {
  if (!S.tel || S.tab !== 'cal' || S.calView !== 'adjust') return;
  for (let id = 0; id < NUM_SERVOS; ++id) {
    if (Date.now() - manualTouched[id] < LOCAL_PRIORITY_MS) continue;
    const deg = S.tel.servo.deg[id];
    const input = $(`man-${id}`);
    if (document.activeElement !== input) input.value = Math.round(deg);
    $(`man-val-${id}`).textContent = `${fmtDeg(deg)}°`;
  }
}

// ------------------------------------------------------------- Editor -------

function currentUs() {
  if (S.localUs !== null && Date.now() - S.localTime < LOCAL_PRIORITY_MS) return S.localUs;
  if (S.tel) return S.tel.servo.us[S.sel];
  const c = S.cal[S.sel];
  return c ? c.center_us : 1500;
}

// Vorschlag für den zweiten Bezugswinkel: Coxa ±45°, Femur/Tibia 0°
const REF_SUGGEST = [45, 0, 0];

function selectServo(id) {
  S.sel = (id + NUM_SERVOS) % NUM_SERVOS;
  S.localUs = null;
  $('ed-ref-deg').value = REF_SUGGEST[S.sel % 3];
  const leg = Math.floor(S.sel / 3);
  $('ed-title').textContent = `Bein ${leg + 1} – ${JOINTS[S.sel % 3]}`;
  $('ed-sub').textContent = `${LEGS[leg]} · ID ${S.sel}`;
  renderLegs();
  renderEditorCal();
  renderEditorLive();
}

function renderEditorCal() {
  const c = S.cal[S.sel];
  if (!c) return;
  const leg = Math.floor(S.sel / 3);
  $('ed-sub').textContent = `${LEGS[leg]} · PCA #${c.board + 1} · CH${c.channel} · ID ${S.sel}`;
  for (const [id, key] of [['ed-center', 'center_us'], ['ed-slope', 'us_per_deg'], ['ed-min', 'min_deg'], ['ed-max', 'max_deg']]) {
    if (document.activeElement !== $(id)) $(id).value = c[key];
  }
  const test = $('ed-test');
  test.min = Math.ceil(c.min_deg);
  test.max = Math.floor(c.max_deg);
  $('ed-assembly').textContent = `Aktuelle Stellung = Montagestellung (${fmtDeg(refDeg(c)).replace('.0', '')}°)`;
}

function renderEditorLive() {
  const us = Math.round(currentUs());
  $('ed-us').textContent = us;
  $('ed-deg').textContent = fmtDeg(usToDeg(S.sel, us));
  const slider = $('ed-us-slider');
  if (document.activeElement !== slider) slider.value = us;
  const test = $('ed-test');
  if (document.activeElement !== test && Date.now() - S.localTime > LOCAL_PRIORITY_MS) {
    const deg = usToDeg(S.sel, us);
    test.value = Math.round(deg);
    $('ed-test-val').textContent = fmtDeg(deg);
  }
}

function torqueHint() {
  if (S.tel && !S.tel.servo.torque && !S.torqueHintShown) {
    S.torqueHintShown = true;
    toast('Hinweis: Torque ist aus – der Servo bewegt sich erst nach dem Einschalten.');
  }
}

const sendUs = throttle((id, us) => send('servo_us', { id, us }), 40);
const sendDeg = throttle((id, deg) => send('servo_deg', { id, deg }), 40);

function setUs(us) {
  us = clamp(Math.round(us), 500, 2500);
  S.localUs = us;
  S.localTime = Date.now();
  sendUs(S.sel, us);
  torqueHint();
  renderEditorLive();
}

function setTestDeg(deg) {
  const c = S.cal[S.sel];
  if (c) deg = clamp(deg, c.min_deg, c.max_deg);
  S.localTime = Date.now();
  S.localUs = c ? degToUs(c, deg) : null;
  $('ed-test').value = Math.round(deg);
  $('ed-test-val').textContent = fmtDeg(deg);
  sendDeg(S.sel, deg);
  torqueHint();
  renderEditorLive();
}

function bindEditor() {
  $('ed-prev').onclick = () => selectServo(S.sel - 1);
  $('ed-next').onclick = () => selectServo(S.sel + 1);
  $('ed-us-slider').oninput = (e) => setUs(+e.target.value);
  document.querySelectorAll('[data-jog]').forEach((b) => {
    b.onclick = () => setUs(currentUs() + +b.dataset.jog);
  });
  $('ed-assembly').onclick = () => send('cal_center', { id: S.sel });
  $('ed-ref').onclick = () => {
    const deg = parseFloat($('ed-ref-deg').value);
    if (!isFinite(deg)) return toast('Referenzwinkel eingeben', true);
    send('cal_ref', { id: S.sel, deg });
  };
  $('ed-invert').onclick = () => send('cal_invert', { id: S.sel });
  $('ed-apply').onclick = () => {
    const v = (id) => parseFloat($(id).value);
    send('cal_set', {
      id: S.sel,
      center_us: v('ed-center'),
      us_per_deg: v('ed-slope'),
      min_deg: v('ed-min'),
      max_deg: v('ed-max'),
    });
  };
  $('ed-test').oninput = (e) => setTestDeg(+e.target.value);
  $('ed-go-min').onclick = () => S.cal[S.sel] && setTestDeg(S.cal[S.sel].min_deg);
  $('ed-go-ref').onclick = () => S.cal[S.sel] && setTestDeg(refDeg(S.cal[S.sel]));
  $('ed-go-max').onclick = () => S.cal[S.sel] && setTestDeg(S.cal[S.sel].max_deg);
}

// ------------------------------------------------- Verhalten (Spielereien) --
// Emotes und Choreografien: Abfolgen aus Gesten, Körperbewegungen, Höhe, Laufen und
// Vorderbeinen. Der Browser spielt sie mit denselben Befehlen ab wie die Handsteuerung
// (Körper/Laufen alle 100 ms erneuert); eigene Choreografien liegen im Browser.

const FUN_KEY = 'hexapod.choreos';
const FUN_TICK_MS = 100;
const FUN_GESTURES = { circle: 'Kreisen', nod: 'Ja', shake: 'Nein', pushup: 'Liegestütz', dance: 'Tanzen' };
const FUN_TYPES = {
  emote: 'Emote', gesture: 'Geste', body: 'Körper', height: 'Höhe', walk: 'Laufen', arms: 'Vorderbeine',
  wave: 'Winken', eyes: 'Augen', pause: 'Pause',
};
// Körper: x vor, y links, roll, pitch (+ Nase hoch), yaw (+ links) in % des Joystick-Ausschlags
const fb = (x, y, roll, pitch, yaw, s) => ({ t: 'body', x, y, roll, pitch, yaw, s });
const fh = (dz, s) => ({ t: 'height', dz, s });
const fe = (color, effect, s = 0) => ({ t: 'eyes', color, effect, s });  // s = 0: bis zum Ende
const fg = (name) => ({ t: 'gesture', name });
// n-mal die Schritte steps wiederholen
const frep = (n, steps) => Array.from({ length: n }, () => steps).flat();
const FUN_EMOTES = [
  { id: 'hello', name: 'Hallo', steps: [fe('#3fd17a', 'wink'), { t: 'arms', up: true }, { t: 'wave' }, { t: 'arms', up: false }] },
  { id: 'bow', name: 'Verbeugen', steps: [fe('#ffc46b', 'breathe'), fb(20, 0, 0, -100, 0, 1.4), fb(0, 0, 0, 0, 0, 0.8)] },
  { id: 'happy', name: 'Freuen', steps: [fe('#ffd400', 'rainbow'), ...frep(2, [fh(-12, 0.45), fh(0, 0.45)]),
    fb(0, 0, 0, 30, 60, 0.4), fb(0, 0, 0, 30, -60, 0.4), fb(0, 0, 0, 30, 60, 0.4), fb(0, 0, 0, 0, 0, 0.5)] },
  { id: 'look', name: 'Umschauen', steps: [fe('#3fb6ff', 'scan'), fb(0, 0, 0, 40, 100, 1.2), fb(0, 0, 0, 40, -100, 1.6),
    fb(0, 0, 0, 0, 0, 0.8)] },
  { id: 'curious', name: 'Neugierig', steps: [fe('#00e0d0', 'scan'), fb(100, 0, 70, -20, 0, 1.2), fb(100, 0, -70, -20, 0, 1.2),
    fb(100, 0, 0, -40, 0, 0.8), fb(0, 0, 0, 0, 0, 0.7)] },
  { id: 'angry', name: 'Sauer', steps: [fe('#ff2000', 'flicker'), fh(-8, 0.4), fb(40, 0, 0, -60, 0, 0.6),
    ...frep(3, [fh(-16, 0.18), fh(-6, 0.18)]), ...frep(2, [fb(40, 0, 0, -60, 35, 0.14), fb(40, 0, 0, -60, -35, 0.14)]),
    fb(40, 0, 0, -60, 0, 0.8), fb(0, 0, 0, 0, 0, 0.6), fh(0, 0.5)] },
  { id: 'attack', name: 'Angriff', steps: [fe('#ff0000', 'flicker'), fh(-15, 0.5), fb(-100, 0, 0, 40, 0, 0.7),
    fe('#ff0000', 'strobe'), fb(100, 0, 0, -50, 0, 0.35), fb(-80, 0, 0, 30, 0, 0.4), fb(100, 0, 0, -50, 0, 0.35),
    fe('#ff0000', 'solid'), fb(-40, 0, 0, 20, 0, 0.6), fb(0, 0, 0, 0, 0, 0.6), fh(0, 0.5)] },
  { id: 'scared', name: 'Ängstlich', steps: [fe('#a070ff', 'flicker'), fh(-18, 0.6), fb(-100, 0, 0, 50, 0, 0.7),
    ...frep(2, [fb(-100, 0, 25, 50, 0, 0.15), fb(-100, 0, -25, 50, 0, 0.15)]),
    fb(-100, 0, 0, 50, 0, 0.6), fb(0, 0, 0, 0, 0, 0.6), fh(0, 0.6)] },
  { id: 'surprised', name: 'Überrascht', steps: [fe('#ffffff', 'strobe'), fh(20, 0.4), fe('#ffffff', 'solid'),
    fb(-80, 0, 0, 80, 0, 1.0), fb(0, 0, 0, 0, 0, 0.6), fh(0, 0.6)] },
  { id: 'sad', name: 'Traurig', steps: [fe('#2050ff', 'breathe'), fh(-15, 1.5), fb(0, 0, 0, -100, 0, 1.4),
    ...frep(2, [fb(0, 0, 0, -100, 25, 0.8), fb(0, 0, 0, -100, -25, 0.8)]), fb(0, 0, 0, -100, 0, 1.0),
    fb(0, 0, 0, 0, 0, 0.8), fh(0, 1.0)] },
  { id: 'tired', name: 'Müde', steps: [fe('#5070ff', 'sleepy'), fh(-25, 2.5), fb(0, 0, 0, -60, 0, 1.5),
    fb(0, 0, 0, 0, 0, 0.4), fb(0, 0, 30, -80, 0, 2.0), fb(0, 0, 0, 0, 0, 0.6), fh(0, 1.2)] },
  { id: 'love', name: 'Verliebt', steps: [fe('#ff3c9a', 'pulse'), ...frep(3, [fb(0, 0, 60, 30, 0, 0.8), fb(0, 0, -60, 30, 0, 0.8)]),
    fh(-8, 0.35), fh(0, 0.35), fb(0, 0, 0, 0, 0, 0.6)] },
  { id: 'party', name: 'Party', steps: [fe('#ffffff', 'rainbow'), ...frep(2, [fb(0, 0, 100, 0, 0, 0.35), fb(0, 0, -100, 0, 0, 0.35)]),
    fg('dance'), ...frep(2, [fh(-12, 0.3), fh(0, 0.3)]), fb(0, 0, 0, 0, 0, 0.4)] },
  { id: 'stretch', name: 'Strecken', steps: [fe('#ffffff', 'sleepy'), fh(25, 1.0), fe('#ffffff', 'solid'),
    fb(0, 0, 0, 100, 0, 1.2), fb(0, 0, 0, -60, 0, 1.0), fb(0, 0, 0, 0, 0, 0.6), fh(0, 0.8)] },
  ...Object.entries(FUN_GESTURES).map(([g, name]) => ({ id: g, name, steps: [fg(g)] })),
];
const FUN_TEMPLATES = [
  { id: 'tpl-disco', name: 'Disco', builtin: true, loop: false, steps: [
    fb(0, 0, 100, 0, 0, 0.4), fb(0, 0, -100, 0, 0, 0.4), fb(0, 0, 100, 0, 0, 0.4), fb(0, 0, -100, 0, 0, 0.4),
    fh(-12, 0.3), fh(0, 0.3), fh(-12, 0.3), fh(0, 0.3), { t: 'gesture', name: 'dance' },
    fb(0, 100, 0, 0, 0, 0.5), fb(0, -100, 0, 0, 0, 0.5), fb(0, 0, 0, 0, 0, 0.4), { t: 'gesture', name: 'circle' }] },
  { id: 'tpl-greet', name: 'Begrüßung', builtin: true, loop: false, steps: [
    { t: 'emote', id: 'hello' }, { t: 'emote', id: 'bow' }, { t: 'gesture', name: 'nod' }] },
  { id: 'tpl-patrol', name: 'Wache schieben', builtin: true, loop: true, steps: [
    { t: 'walk', vx: 60, vy: 0, turn: 0, s: 3 }, { t: 'walk', vx: 0, vy: 0, turn: 70, s: 2.6 },
    { t: 'emote', id: 'look' }, { t: 'pause', s: 1 }] },
];

const FUN = { run: null, own: [], edit: null, speed: 1, last: '' };

function funLoad() {
  try {
    const raw = JSON.parse(localStorage.getItem(FUN_KEY) || '[]');
    FUN.own = (Array.isArray(raw) ? raw : []).map(funCleanChoreo).filter(Boolean);
  } catch (e) {
    FUN.own = [];
  }
}

function funStore() {
  try {
    localStorage.setItem(FUN_KEY, JSON.stringify(FUN.own));
    return true;
  } catch (e) {
    toast('Speichern im Browser nicht möglich (privater Modus?) – Export verwenden', true);
    return false;
  }
}

// Eingelesene Schritte prüfen und begrenzen (Import, Speicher)
function funCleanStep(st) {
  if (!st || !FUN_TYPES[st.t]) return null;
  const num = (v, lo, hi, d = 0) => (Number.isFinite(+v) ? clamp(Math.round(+v * 100) / 100, lo, hi) : d);
  const s = num(st.s, 0, 30, 1);
  switch (st.t) {
    case 'emote': return FUN_EMOTES.some((e) => e.id === st.id) ? { t: 'emote', id: st.id } : null;
    case 'eyes': return { t: 'eyes', color: /^#[0-9a-f]{6}$/i.test(st.color) ? st.color.toLowerCase() : '#ffffff',
      effect: LED_EFFECTS[st.effect] && st.effect !== 'off' ? st.effect : 'solid', s: num(st.s, 0, 30, 0) };
    case 'gesture': return FUN_GESTURES[st.name] ? { t: 'gesture', name: st.name } : null;
    case 'body': return { t: 'body', x: num(st.x, -100, 100), y: num(st.y, -100, 100), roll: num(st.roll, -100, 100),
      pitch: num(st.pitch, -100, 100), yaw: num(st.yaw, -100, 100), s };
    case 'height': return { t: 'height', dz: num(st.dz, -60, 60), s };
    case 'walk': return { t: 'walk', vx: num(st.vx, -100, 100), vy: num(st.vy, -100, 100), turn: num(st.turn, -100, 100), s };
    case 'arms': return { t: 'arms', up: !!st.up };
    case 'wave': return { t: 'wave' };
    default: return { t: 'pause', s };
  }
}

function funCleanChoreo(c) {
  if (!c || typeof c.name !== 'string' || !Array.isArray(c.steps)) return null;
  const steps = c.steps.slice(0, 200).map(funCleanStep).filter(Boolean);
  return { id: typeof c.id === 'string' && c.id ? c.id : funId(), name: c.name.slice(0, 40) || 'Choreografie',
    loop: !!c.loop, steps };
}

const funId = () => `c${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
const funTel = () => (S.tel && S.tel.motion) || {};
const funFind = (id) => FUN_TEMPLATES.find((c) => c.id === id) || FUN.own.find((c) => c.id === id) ||
  FUN_EMOTES.find((e) => `emote-${e.id}` === id) ||
  (FUN_TEMPLATES.draft && FUN_TEMPLATES.draft.id === id ? FUN_TEMPLATES.draft : null);

// ---- Abspielen

function funSleep(ms, run) {
  return new Promise((resolve) => {
    const end = Date.now() + ms;
    const tick = () => (run.stopped || Date.now() >= end ? resolve() : setTimeout(tick, Math.min(50, end - Date.now())));
    tick();
  });
}

async function funUntil(cond, ms, run) {
  const end = Date.now() + ms;
  while (!run.stopped && Date.now() < end) {
    if (cond()) return true;
    await funSleep(50, run);
  }
  return !run.stopped && cond();
}

// Befehl für die Dauer ms alle 100 ms wiederholen (Firmware hält sonst nach 0,6 s an)
async function funHold(ms, fn, run) {
  const end = Date.now() + ms;
  while (!run.stopped && Date.now() < end) {
    if (!fn()) {
      run.stopped = true;
      break;
    }
    await funSleep(Math.min(FUN_TICK_MS, end - Date.now()), run);
  }
}

// Steht der Roboter (IK-Betrieb, Haltung „stehen“)? Sonst aufstehen und warten.
async function funPrepare(run) {
  const t = S.tel;
  if (!wsReady() || !t) {
    toast('Keine Verbindung zum Hexapod', true);
    return false;
  }
  if (t.servo && t.servo.estop) {
    toast('Not-Aus ist aktiv', true);
    return false;
  }
  if (t.servo && !t.servo.torque) {
    toast('Servos sind aus – erst „Torque“ einschalten', true);
    return false;
  }
  if (W.timer || W.keys.size) walkStopAll();
  const standing = () => funTel().mode === 'pose' && funTel().posture === 'standing';
  if (!standing()) {
    if (funTel().mode !== 'pose' || funTel().posture === 'sitting') send('stand');
    run.label = 'steht auf …';
    renderFun();
    if (!await funUntil(standing, 12000, run)) {
      if (!run.stopped) toast('Roboter steht nicht – Choreografie abgebrochen', true);
      return false;
    }
  }
  run.z0 = funTel().pose ? funTel().pose.z : 0;
  return true;
}

async function funArms(up, run) {
  const want = up ? 'up' : 'down';
  if (funTel().arms === want) return true;
  if (up) await funUntil(() => !funTel().gait || !funTel().gait.walking, 3000, run);
  send('arms', { up });
  run.used.arms = true;
  return funUntil(() => funTel().arms === want, 8000, run);
}

async function funStep(st, next, run) {
  const ms = (Math.max(0, +st.s || 0) * 1000) / FUN.speed;
  switch (st.t) {
    case 'emote': {
      const e = FUN_EMOTES.find((x) => x.id === st.id);
      for (let i = 0; e && i < e.steps.length && !run.stopped; ++i) await funStep(e.steps[i], e.steps[i + 1] || next, run);
      return;
    }
    case 'gesture':
      if (!await funArms(false, run)) return;
      send('gesture', { name: st.name });
      run.used.gesture = true;
      await funUntil(() => funTel().gesture === st.name, 1500, run);
      await funUntil(() => !funTel().gesture, 20000, run);
      return;
    case 'body':
      if (!await funArms(false, run)) return;
      run.used.body = true;
      await funHold(ms, () => send('body', { x: st.x / 100, y: st.y / 100, roll: st.roll / 100, pitch: st.pitch / 100,
        yaw: st.yaw / 100 }), run);
      return;
    case 'walk':
      if (!await funArms(false, run)) return;
      run.used.walk = true;
      await funHold(ms, () => send('walk', { vx: st.vx / 100, vy: st.vy / 100, turn: st.turn / 100 }), run);
      if (!next || next.t !== 'walk') send('stop');  // sonst liefe er noch 0,6 s weiter
      return;
    case 'height': {
      const r = (funTel().range && funTel().range.z) || [-Infinity, Infinity];
      const z = clamp(run.z0 + st.dz, r[0], r[1]);
      const from = funTel().pose ? funTel().pose.z : run.z0;
      send('pose', { z: Math.round(z) });
      run.used.height = true;
      // ohne Dauer: so lange, wie die Bewegung braucht (80 mm/s)
      await funSleep(ms || Math.abs(z - from) / 80 * 1000 + 300, run);
      return;
    }
    case 'arms':
      await funArms(st.up, run);
      return;
    case 'wave':
      if (!await funArms(true, run)) return;
      send('wave');
      await funSleep(3200, run);
      return;
    case 'eyes':
      send('eyes', { color: st.color, effect: st.effect, ms: Math.round(ms || 120000) });
      run.used.eyes = true;
      return;
    case 'note':  // nur in Spielen: Text anzeigen
      $(st.el).textContent = st.text;
      return;
    default:
      await funSleep(ms, run);
  }
}

// id einer Choreografie/eines Emotes, oder eine Abfolge {id, name, steps} (Spiele); done(): fertig gespielt
async function funPlay(id, done) {
  const c = typeof id === 'object' ? id : funFind(id);
  if (!c) return;
  if (typeof id === 'object') id = c.id;
  if (FUN.run) {
    const same = FUN.run.id === id;
    funStop();
    if (same) return;  // zweiter Tipp = anhalten
    await new Promise((r) => setTimeout(r, 150));
  }
  const run = { id, name: c.name, steps: c.steps, loop: !!c.loop, step: -1, stopped: false, used: {}, label: '' };
  FUN.run = run;
  renderFun();
  try {
    if (!await funPrepare(run)) return;
    do {
      for (let i = 0; i < run.steps.length && !run.stopped; ++i) {
        run.step = i;
        run.label = '';
        renderFun();
        await funStep(run.steps[i], run.steps[i + 1] || (run.loop ? run.steps[0] : null), run);
      }
    } while (run.loop && !run.stopped && run.steps.length);
    if (!run.stopped && done) done();
  } finally {
    funFinish(run);
  }
}

// Aufräumen: Körper zurück, anhalten, Vorderbeine ab, Höhe wie vorher
function funFinish(run) {
  if (run.done) return;
  run.done = true;
  if (FUN.run === run) FUN.run = null;
  if (wsReady()) {
    const tel = funTel();
    if (run.used.body) send('body', { x: 0, y: 0, roll: 0, pitch: 0, yaw: 0 });
    if (run.used.walk) send('stop');
    if (run.stopped && tel.gesture) send('gesture', { name: 'stop' });
    if (run.used.arms && tel.arms && tel.arms !== 'down') send('arms', { up: false });
    if (run.used.height && run.z0) send('pose', { z: Math.round(run.z0) });
    if (run.used.eyes) send('eyes', { reset: true });
  }
  renderFun();
}

function funStop() {
  const run = FUN.run;
  if (!run) return;
  run.stopped = true;
  funFinish(run);
}

// ---- Anzeige

function renderFun() {
  const run = FUN.run;
  $('fun-now').hidden = !run;
  if (run) {
    const pos = run.step >= 0 ? ` – Schritt ${run.step + 1}/${run.steps.length}` : '';
    $('fun-now-text').textContent = `${run.name}${run.label ? ` ${run.label}` : pos}${run.loop ? ' (Endlosschleife)' : ''}`;
  }
  document.querySelectorAll('[data-fun-play]').forEach((b) => {
    const on = !!run && run.id === b.dataset.funPlay;
    b.classList.toggle('running', on);
    if (b.dataset.toggleLabel) {
      b.innerHTML = on ? '<svg class="ic"><use href="#i-stop"/></svg>Stopp' : '<svg class="ic"><use href="#i-play"/></svg>Abspielen';
    }
  });
  document.querySelectorAll('[data-game]').forEach((b) => {
    b.classList.toggle('running', !!run && run.id === `game-${b.dataset.game}`);
  });
  document.querySelectorAll('#fun-steps > li').forEach((li, i) => {
    li.classList.toggle('running', !!run && FUN.edit && run.id === `edit-${FUN.edit.id}` && run.step === i);
  });
}

function renderFunEmotes() {
  $('fun-emotes').innerHTML = FUN_EMOTES.map((e) =>
    `<button type="button" class="btn" data-fun-play="emote-${e.id}">${escapeHtml(e.name)}</button>`).join('');
}

function renderFunList() {
  const row = (c, own) => `<div class="fun-row">
      <div class="fun-row-name"><b>${escapeHtml(c.name)}</b>
        <span class="muted small">${c.steps.length} Schritt${c.steps.length === 1 ? '' : 'e'}${c.loop ? ' · Schleife' : ''}${own ? '' : ' · Vorlage'}</span></div>
      <button type="button" class="btn btn-small" data-fun-play="${c.id}" data-toggle-label="1"></button>
      <button type="button" class="btn btn-small btn-ghost" data-fun-edit="${c.id}" title="${own ? 'Bearbeiten' : 'Als Kopie bearbeiten'}">${own ? 'Bearbeiten' : 'Kopie'}</button>
      ${own ? `<button type="button" class="btn btn-small btn-ghost" data-fun-del="${c.id}" title="Löschen" aria-label="${escapeHtml(c.name)} löschen"><svg class="ic"><use href="#i-x"/></svg></button>` : ''}
    </div>`;
  $('fun-list').innerHTML = FUN_TEMPLATES.map((c) => row(c, false)).join('') + FUN.own.map((c) => row(c, true)).join('');
  renderFun();
}

// ---- Editor

const FUN_FIELDS = {
  body: [['x', 'Vor'], ['y', 'Links'], ['roll', 'Rollen'], ['pitch', 'Nicken'], ['yaw', 'Drehen'], ['s', 's']],
  walk: [['vx', 'Vor'], ['vy', 'Links'], ['turn', 'Drehen'], ['s', 's']],
  height: [['dz', 'mm'], ['s', 's']],
  pause: [['s', 's']],
};

function funNewStep(t) {
  switch (t) {
    case 'emote': return { t, id: 'hello' };
    case 'gesture': return { t, name: 'nod' };
    case 'body': return { t, x: 0, y: 0, roll: 0, pitch: 0, yaw: 0, s: 1 };
    case 'walk': return { t, vx: 50, vy: 0, turn: 0, s: 2 };
    case 'height': return { t, dz: -10, s: 0.5 };
    case 'arms': return { t, up: true };
    case 'wave': return { t };
    case 'eyes': return { t, color: '#ff2000', effect: 'flicker', s: 0 };
    default: return { t: 'pause', s: 1 };
  }
}

function renderFunEditor() {
  const ed = FUN.edit;
  $('fun-editor').hidden = !ed;
  if (!ed) return;
  const opts = (map, cur) => Object.entries(map).map(([k, v]) =>
    `<option value="${k}"${k === cur ? ' selected' : ''}>${escapeHtml(v)}</option>`).join('');
  $('fun-steps').innerHTML = ed.steps.map((st, i) => {
    let params = '';
    if (st.t === 'emote') params = `<select data-k="id" aria-label="Emote">${opts(Object.fromEntries(FUN_EMOTES.map((e) => [e.id, e.name])), st.id)}</select>`;
    else if (st.t === 'gesture') params = `<select data-k="name" aria-label="Geste">${opts(FUN_GESTURES, st.name)}</select>`;
    else if (st.t === 'arms') params = `<select data-k="up" aria-label="Vorderbeine">${opts({ 1: 'heben', 0: 'absetzen' }, st.up ? '1' : '0')}</select>`;
    else if (st.t === 'eyes') {
      const fx = Object.fromEntries(Object.entries(LED_EFFECTS).filter(([k]) => k !== 'off'));
      params = `<input type="color" data-k="color" value="${st.color}" aria-label="Farbe">` +
        `<select data-k="effect" aria-label="Effekt">${opts(fx, st.effect)}</select>` +
        `<label class="fun-num" title="0 = bis zum Ende"><span>s</span><input type="number" inputmode="decimal" data-k="s" min="0" max="30" step="0.5" value="${st.s}"></label>`;
    }
    else if (FUN_FIELDS[st.t]) {
      params = FUN_FIELDS[st.t].map(([k, label]) => {
        const lim = k === 's' ? 'min="0" max="30" step="0.1"' : (k === 'dz' ? 'min="-60" max="60" step="1"' : 'min="-100" max="100" step="5"');
        return `<label class="fun-num"><span>${label}</span><input type="number" inputmode="decimal" data-k="${k}" ${lim} value="${st[k]}"></label>`;
      }).join('');
    }
    return `<li data-i="${i}"><div class="fun-step-head"><select data-type aria-label="Art">${opts(FUN_TYPES, st.t)}</select>
      <span class="fun-step-tools">
        <button type="button" class="btn btn-small btn-ghost" data-move="-1" title="nach oben" aria-label="nach oben"${i ? '' : ' disabled'}>↑</button>
        <button type="button" class="btn btn-small btn-ghost" data-move="1" title="nach unten" aria-label="nach unten"${i < ed.steps.length - 1 ? '' : ' disabled'}>↓</button>
        <button type="button" class="btn btn-small btn-ghost" data-remove title="entfernen" aria-label="Schritt entfernen"><svg class="ic"><use href="#i-x"/></svg></button>
      </span></div>${params ? `<div class="fun-params">${params}</div>` : ''}</li>`;
  }).join('') || '<li class="muted small">Noch keine Schritte</li>';
  renderFun();
}

function funOpenEditor(id) {
  const src = id ? funFind(id) : null;
  if (src && src.builtin) {
    FUN.edit = { id: null, name: `${src.name} (Kopie)`, loop: src.loop, steps: JSON.parse(JSON.stringify(src.steps)) };
  } else if (src) {
    FUN.edit = JSON.parse(JSON.stringify(src));
  } else {
    FUN.edit = { id: null, name: 'Meine Choreografie', loop: false, steps: [{ t: 'emote', id: 'hello' }] };
  }
  $('fun-name').value = FUN.edit.name;
  $('fun-loop').checked = FUN.edit.loop;
  $('fun-ed-title').textContent = FUN.edit.id ? 'Choreografie bearbeiten' : 'Neue Choreografie';
  renderFunEditor();
  $('fun-editor').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

// ---- Spiele

const fnote = (el, text) => ({ t: 'note', el, text });
const ORACLE = [
  ['Ja!', [fe('#3fd17a', 'solid'), fg('nod')]],
  ['Nein.', [fe('#ff2000', 'solid'), fg('shake')]],
  ['Vielleicht …', [fe('#ffd400', 'scan'), fg('circle')]],
  ['Ganz sicher!', [{ t: 'emote', id: 'happy' }]],
  ['Frag später nochmal.', [{ t: 'emote', id: 'tired' }]],
  ['Lieber nicht.', [{ t: 'emote', id: 'scared' }]],
];
const DICE = ['⚀', '⚁', '⚂', '⚃', '⚄', '⚅'];

// Spiel starten bzw. (zweiter Tipp) anhalten
function funGame(name, steps, label) {
  const id = `game-${name}`;
  if (FUN.run && FUN.run.id === id) {
    funStop();
    return;
  }
  funPlay({ id, name: label, steps });
}

function funOracle() {
  const [answer, steps] = ORACLE[Math.floor(Math.random() * ORACLE.length)];
  funGame('oracle', [fnote('fun-oracle-out', 'Das Orakel denkt nach …'), fe('#ffffff', 'scan'),
    fb(0, 0, 0, 40, 80, 0.9), fb(0, 0, 0, 40, -80, 0.9), fb(0, 0, 0, 0, 0, 0.4),
    fnote('fun-oracle-out', `„${answer}“`), ...steps], 'Orakel');
}

function funDice() {
  const n = 1 + Math.floor(Math.random() * 6);
  const tap = [fe('#ffffff', 'solid'), fb(0, 0, 0, -80, 0, 0.3), fe('#303030', 'solid'), fb(0, 0, 0, 0, 0, 0.4)];
  funGame('dice', [fnote('fun-dice-out', 'würfelt …'), fe('#ffffff', 'flicker'),
    ...frep(3, [fb(0, 0, 50, 0, 40, 0.15), fb(0, 0, -50, 0, -40, 0.15)]), fb(0, 0, 0, 0, 0, 0.5),
    fnote('fun-dice-out', `${DICE[n - 1]} ${n}`), ...frep(n, tap), fe('#3fd17a', 'solid', 1)], 'Würfeln');
}

function funRandom() {
  const pool = FUN_EMOTES.filter((e) => e.id !== FUN.last);
  const e = pool[Math.floor(Math.random() * pool.length)];
  FUN.last = e.id;
  $('fun-random-out').textContent = e.name;
  funGame('random', [{ t: 'emote', id: e.id }], `Überrasch mich: ${e.name}`);
}

// ---- Augen

const EYE_SWATCHES = [
  ['Wut', '#ff2000', 'flicker'], ['Ruhig', '#2050ff', 'breathe'], ['Gut', '#3fd17a', 'solid'], ['Herz', '#ff3c9a', 'pulse'],
  ['Achtung', '#ffb000', 'blink'], ['Blitz', '#ffffff', 'strobe'], ['Suchen', '#00e0d0', 'scan'], ['Müde', '#5070ff', 'sleepy'],
  ['Party', '#ffffff', 'rainbow'],
];
const funEyeStyle = () => ({ color: $('fun-eye-color').value, effect: $('fun-eye-fx').value });

function funEyeFrame() {
  if (S.tab !== 'fun' || document.hidden) {
    FUN.raf = 0;
    return;
  }
  const live = S.tel && S.tel.eyes;  // was der Roboter gerade vorgegeben zeigt
  const style = live || funEyeStyle();
  const c = ledRender(style, performance.now());
  for (let i = 0; i < 2; ++i) {
    const [r, g, b] = c[i];
    const el = $(`fun-eye-${i}`);
    el.style.background = `rgb(${r}, ${g}, ${b})`;
    el.style.boxShadow = `0 0 ${6 + (r + g + b) / 30}px rgb(${r}, ${g}, ${b})`;
  }
  $('fun-eye-now').textContent = live ? `Roboter zeigt: ${LED_EFFECTS[live.effect] || live.effect}` : 'Vorschau';
  FUN.raf = requestAnimationFrame(funEyeFrame);
}

function funEyeTick() {
  if (S.tab === 'fun' && !FUN.raf && !document.hidden) FUN.raf = requestAnimationFrame(funEyeFrame);
}

function funEyeShow(style) {
  send('eyes', { ...style, ms: +$('fun-eye-dur').value });
}

function bindFun() {
  funLoad();
  renderFunEmotes();
  renderFunList();
  $('fun-add-type').innerHTML = Object.entries(FUN_TYPES).map(([k, v]) => `<option value="${k}">${v}</option>`).join('');
  $('page-fun').addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.funPlay) funPlay(b.dataset.funPlay);
    else if (b.dataset.funEdit) funOpenEditor(b.dataset.funEdit);
    else if (b.dataset.funDel) {
      const c = FUN.own.find((x) => x.id === b.dataset.funDel);
      if (!c || !confirm(`„${c.name}“ löschen?`)) return;
      if (FUN.run && FUN.run.id === c.id) funStop();
      FUN.own = FUN.own.filter((x) => x !== c);
      funStore();
      if (FUN.edit && FUN.edit.id === c.id) FUN.edit = null;
      renderFunList();
      renderFunEditor();
    }
  });
  $('fun-now-stop').onclick = funStop;
  $('fun-oracle').onclick = funOracle;
  $('fun-dice').onclick = funDice;
  $('fun-random').onclick = funRandom;
  try {
    FUN.speed = clamp(+localStorage.getItem('hexapod.funSpeed') || 1, 0.5, 2);
  } catch (e) { /* ohne Speicher: 100 % */ }
  $('fun-speed').value = Math.round(FUN.speed * 100);
  $('fun-speed-val').textContent = `${Math.round(FUN.speed * 100)} %`;
  $('fun-speed').oninput = (e) => {
    FUN.speed = +e.target.value / 100;
    $('fun-speed-val').textContent = `${e.target.value} %`;
    try { localStorage.setItem('hexapod.funSpeed', String(FUN.speed)); } catch (err) { /* egal */ }
  };
  $('fun-eye-fx').innerHTML = Object.entries(LED_EFFECTS).filter(([k]) => k !== 'off')
    .map(([k, v]) => `<option value="${k}"${k === 'flicker' ? ' selected' : ''}>${v}</option>`).join('');
  $('fun-swatches').innerHTML = EYE_SWATCHES.map(([name, color, fx]) =>
    `<button type="button" class="swatch" data-color="${color}" data-fx="${fx}" title="${LED_EFFECTS[fx]}"` +
    ` style="--sw:${fx === 'rainbow' ? 'conic-gradient(red, yellow, lime, cyan, blue, magenta, red)' : color}">` +
    `<i></i>${name}</button>`).join('');
  $('fun-swatches').addEventListener('click', (e) => {
    const b = e.target.closest('.swatch');
    if (!b) return;
    $('fun-eye-color').value = b.dataset.color;
    $('fun-eye-fx').value = b.dataset.fx;
    funEyeShow(funEyeStyle());
  });
  $('fun-eye-show').onclick = () => funEyeShow(funEyeStyle());
  $('fun-eye-reset').onclick = () => send('eyes', { reset: true });
  $('fun-new').onclick = () => funOpenEditor(null);
  $('fun-close').onclick = () => {
    if (FUN.run && FUN.edit && FUN.run.id === `edit-${FUN.edit.id}`) funStop();
    FUN.edit = null;
    renderFunEditor();
  };
  $('fun-name').oninput = (e) => { if (FUN.edit) FUN.edit.name = e.target.value; };
  $('fun-loop').onchange = (e) => { if (FUN.edit) FUN.edit.loop = e.target.checked; };
  $('fun-add').onclick = () => {
    if (!FUN.edit) return;
    FUN.edit.steps.push(funNewStep($('fun-add-type').value));
    renderFunEditor();
  };
  const steps = $('fun-steps');
  steps.addEventListener('change', (e) => {
    const li = e.target.closest('li[data-i]');
    if (!li || !FUN.edit) return;
    const i = +li.dataset.i;
    const st = FUN.edit.steps[i];
    if (e.target.hasAttribute('data-type')) {
      FUN.edit.steps[i] = funNewStep(e.target.value);
      renderFunEditor();
      return;
    }
    const k = e.target.dataset.k;
    if (!k) return;
    if (k === 'up') st.up = e.target.value === '1';
    else if (k === 'id' || k === 'name' || k === 'color' || k === 'effect') st[k] = e.target.value;
    else st[k] = +e.target.value;
    FUN.edit.steps[i] = funCleanStep(st) || st;
    if (e.target.type === 'number') e.target.value = FUN.edit.steps[i][k];
  });
  steps.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    const li = e.target.closest('li[data-i]');
    if (!b || !li || !FUN.edit) return;
    const i = +li.dataset.i;
    const list = FUN.edit.steps;
    if (b.hasAttribute('data-remove')) list.splice(i, 1);
    else if (b.dataset.move) {
      const j = i + +b.dataset.move;
      if (j < 0 || j >= list.length) return;
      [list[i], list[j]] = [list[j], list[i]];
    }
    renderFunEditor();
  });
  $('fun-try').onclick = () => {
    if (!FUN.edit) return;
    const id = `edit-${FUN.edit.id}`;
    if (FUN.run && FUN.run.id === id) {
      funStop();
      return;
    }
    // Entwurf abspielen (auch ungespeichert)
    FUN_TEMPLATES.draft = { id, name: FUN.edit.name || 'Entwurf', loop: FUN.edit.loop,
      steps: FUN.edit.steps.map(funCleanStep).filter(Boolean) };
    funPlay(id);
  };
  $('fun-save').onclick = () => {
    const ed = FUN.edit;
    if (!ed) return;
    const clean = funCleanChoreo({ ...ed, name: ed.name.trim() || 'Choreografie', id: ed.id || funId() });
    const i = FUN.own.findIndex((c) => c.id === clean.id);
    if (i >= 0) FUN.own[i] = clean;
    else FUN.own.push(clean);
    if (funStore()) toast(`„${clean.name}“ gespeichert`);
    FUN.edit = JSON.parse(JSON.stringify(clean));
    $('fun-ed-title').textContent = 'Choreografie bearbeiten';
    renderFunList();
    renderFunEditor();
  };
  $('fun-export').onclick = () => {
    if (!FUN.own.length) {
      toast('Noch keine eigenen Choreografien', true);
      return;
    }
    const blob = new Blob([JSON.stringify({ version: 1, choreos: FUN.own }, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `hexapod-choreografien-${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
  };
  $('fun-import').onchange = async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    try {
      const data = JSON.parse(await file.text());
      const list = (Array.isArray(data) ? data : data.choreos || []).map(funCleanChoreo).filter(Boolean);
      if (!list.length) throw new Error('keine Choreografien in der Datei');
      for (const c of list) {
        c.id = funId();  // nie eine vorhandene überschreiben
        FUN.own.push(c);
      }
      funStore();
      renderFunList();
      toast(`${list.length} Choreografie${list.length === 1 ? '' : 'n'} importiert`);
    } catch (err) {
      toast(`Import fehlgeschlagen: ${err.message}`, true);
    }
  };
  document.addEventListener('keydown', (e) => {
    if (S.tab !== 'fun' || e.code !== 'Space' || e.defaultPrevented) return;
    const el = e.target;
    if (el && el.closest && el.closest('input, select, textarea, button')) return;
    if (FUN.run) {
      funStop();
      e.preventDefault();
    }
  });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) funStop();
    else funEyeTick();
  });
}

// ------------------------------------------------- Globale Bedienelemente ---

function bindGlobal() {
  const estop = () => {
    funStop();
    walkStopAll();
    if (!send('estop')) api('/api/estop', {}).catch(() => {});
  };
  $('btn-estop').onclick = estop;

  for (const id of ['ctl-torque', 'cal-torque']) {
    $(id).onchange = (e) => {
      const on = e.target.checked;
      if (!send('torque', { on })) e.target.checked = !on;
    };
  }
  $('ctl-assembly').onclick = () => send('all_assembly');
  $('ctl-slew').oninput = (e) => {
    $('ctl-slew-val').textContent = `${e.target.value}°/s`;
  };
  $('ctl-slew').onchange = (e) => send('slew', { deg_per_s: +e.target.value });

  $('cal-all-1500').onclick = () => send('all_us', { us: 1500 });
  $('cal-all-assembly').onclick = () => send('all_assembly');
  $('cal-save').onclick = () => send('cal_save');
  $('cal-reload').onclick = () => {
    if (confirm('Ungespeicherte Änderungen verwerfen und gespeicherte Kalibrierung laden?')) send('cal_reload');
  };
  $('cal-defaults').onclick = () => {
    if (confirm('Alle 18 Servos auf Standardwerte zurücksetzen?\n(Wird erst mit „Speichern“ dauerhaft.)')) send('cal_reset');
  };

  document.addEventListener('keydown', (e) => {
    if (e.defaultPrevented) return;  // schon von einem Dialog behandelt
    if (e.key === 'Escape') {
      walkStopAll();
      estop();
      e.preventDefault();
      return;
    }
    if (walkKey(e, true)) {
      e.preventDefault();
      return;
    }
    if (S.tab !== 'cal' || S.calView !== 'cal') return;
    const el = document.activeElement;
    const tag = (el && el.tagName) || '';
    // Pfeiltasten = Puls fein verstellen; nicht, wenn ein anderes Eingabefeld den Fokus hat
    // (z. B. der kalibrierte Testregler – sonst ginge der Rohpuls an den Gelenkgrenzen vorbei)
    if (tag === 'INPUT' && el.id !== 'ed-us-slider') return;
    if (tag === 'SELECT' || tag === 'TEXTAREA') return;
    const step = e.shiftKey ? 10 : 1;
    if (e.key === 'ArrowRight') setUs(currentUs() + step);
    else if (e.key === 'ArrowLeft') setUs(currentUs() - step);
    else if (e.key === 'ArrowUp') selectServo(S.sel - 1);
    else if (e.key === 'ArrowDown') selectServo(S.sel + 1);
    else return;
    e.preventDefault();
  });
}

// -------------------------------------------------------- Seiten & Menü -----

// Seite -> Adresse (#steuerung …), damit Neuladen und Zurück-Taste funktionieren
const PAGES = {
  control: 'steuerung', fun: 'verhalten', camera: 'kamera', cal: 'kalibrierung', system: 'system', leds: 'leds',
  pad: 'controller', settings: 'einstellungen',
};
const PAGE_ALIASES = { '#spielereien': 'fun' };  // alte Adressen (Lesezeichen)
const pageFromHash = () => Object.keys(PAGES).find((k) => location.hash === `#${PAGES[k]}`) ||
  PAGE_ALIASES[location.hash] || 'control';

function showTab(name) {
  if (!PAGES[name]) name = 'control';
  if (name !== 'control') closeFpv();
  if (name !== 'control' && (W.timer || W.keys.size)) walkStopAll();
  if (name !== 'fun') funStop();
  S.tab = name;
  if (location.hash !== `#${PAGES[name]}`) history.replaceState(null, '', `#${PAGES[name]}`);
  document.querySelectorAll('.nav-item[data-page]').forEach((a) => {
    const on = a.dataset.page === name;
    a.classList.toggle('active', on);
    if (on) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  let page = null;
  document.querySelectorAll('.page').forEach((p) => {
    const on = p.id === `page-${name}`;
    p.classList.toggle('active', on);
    if (on) page = p;
  });
  // langer Titel, auf dem Handy die Kurzform
  const full = document.createElement('span');
  full.className = 't-full';
  full.textContent = page.dataset.title;
  const short = document.createElement('span');
  short.className = 't-short';
  short.textContent = page.dataset.short || page.dataset.title;
  $('page-title').replaceChildren(full, short);
  document.title = `${page.dataset.title} · Hexapod MK3`;
  setNavOpen(false);
  updateStream();
  updatePreview();
  v3Tick();
  funEyeTick();
  clearInterval(S.statusTimer);
  renderBanner();
  if (name === 'system' || name === 'settings') {
    loadStatus();
    let tick = 0;
    S.statusTimer = setInterval(() => {
      loadStatus();
      if (S.tab === 'system' && ++tick % 2 === 0) loadHistory();  // neue Werte alle 10 s
    }, 5000);
  }
  if (name === 'system') {
    renderTiles();
    loadLog();
    loadHistory();
  }
  if (name === 'cal') renderManualValues();
  if (name === 'leds') loadLeds();
  if (name === 'pad') loadPad();
}

function showCalView(view) {
  S.calView = view;
  setSeg('cal-view', view);
  $('cal-panel-cal').hidden = view !== 'cal';
  $('cal-panel-adjust').hidden = view !== 'adjust';
  renderManualValues();
}

const setNavOpen = (open) => document.documentElement.classList.toggle('nav-open', open);

function bindNav() {
  $('menu-btn').onclick = () => setNavOpen(!document.documentElement.classList.contains('nav-open'));
  $('sb-backdrop').onclick = () => setNavOpen(false);
  const collapseTitle = () => {
    $('sb-collapse').title = document.documentElement.classList.contains('nav-collapsed')
      ? 'Menü ausklappen' : 'Menü einklappen';
  };
  $('sb-collapse').onclick = () => {
    const c = document.documentElement.classList.toggle('nav-collapsed');
    try { localStorage.setItem('navCollapsed', c ? '1' : '0'); } catch (e) { /* egal */ }
    collapseTitle();
  };
  collapseTitle();
  window.addEventListener('hashchange', () => showTab(pageFromHash()));
  bindSeg('cal-view', showCalView);
}

// Farbschema: automatisch (System), dunkel oder hell
function applyTheme(pref) {
  const root = document.documentElement;
  if (pref === 'light' || pref === 'dark') root.dataset.theme = pref;
  else delete root.dataset.theme;
  setSeg('theme-seg', pref === 'light' || pref === 'dark' ? pref : 'auto');
  const dark = pref === 'dark' || (pref !== 'light' && !matchMedia('(prefers-color-scheme: light)').matches);
  document.querySelector('meta[name="theme-color"]').content = dark ? '#0d1217' : '#f2f4f7';
  V3.colors = null;
  V3.dirty = true;
}

function bindTheme() {
  let pref = 'auto';
  try { pref = localStorage.getItem('theme') || 'auto'; } catch (e) { /* kein Speicher */ }
  applyTheme(pref);
  bindSeg('theme-seg', (v) => {
    try { localStorage.setItem('theme', v); } catch (e) { /* egal */ }
    applyTheme(v);
  });
}

// -------------------------------------------------------------- Kamera ------

const camBase = () => `${location.protocol}//${location.hostname}:81`;
let camRetryTimer = null;
let camRetries = 0;

// Vorschau auf der Seite "Kamera & Lagesensor": eigener Stream, nur solange die Seite offen ist
function updatePreview() {
  const img = $('cam-prev');
  const view = $('cam-prev-view');
  const on = S.tab === 'camera' && !document.hidden;
  if (on && !view.classList.contains('live')) {
    $('cam-prev-text').textContent = 'Vorschau startet …';
    img.src = `${camBase()}/stream?t=${Date.now()}`;
    view.classList.add('live');
  } else if (!on && view.classList.contains('live')) {
    img.src = 'data:,';  // Verbindung schließen
    view.classList.remove('live');
  }
}

// Kopfzeile: Kamera-Symbol, solange ein Stream läuft (auch von einem anderen Gerät)
function renderCamPill(c) {
  const pill = $('pill-cam');
  const n = c && c.clients ? c.clients : 0;
  pill.hidden = !n;
  if (!n) return;
  setPill('pill-cam', n > 1 ? `Kamera · ${n}` : 'Kamera', 'pill-on', '\u00a0');
  pill.title = `Kamerastream läuft${n > 1 ? ` (${n} Zuschauer)` : ''}${c.fps !== undefined ? ` · ${c.fps.toFixed(1)} fps` : ''}`;
}

function renderCamStats(t) {
  renderCamPill(t.cam);
  if (S.tab === 'camera') {
    const c = t.cam || {};
    $('cam-prev-info').textContent = c.fps !== undefined && c.clients
      ? `${c.fps.toFixed(1)} fps · ${c.kb || 0} KB` : '';
  }
  const el = $('cam-stats');
  const c = t.cam || {};
  const live = $('cam-img').parentElement.classList.contains('live');
  $('cam-live').hidden = !live || !c.clients;
  if (!live || !c.clients) {
    // Ohne Stream schläft die Kamera nach kurzer Zeit (spart Strom und Wärme)
    el.textContent = c.awake === undefined ? '' : (c.awake ? 'Kamera aktiv – schläft gleich ohne Stream' : 'Kamera schläft – wacht mit dem Stream auf');
    return;
  }
  if (c.fps !== undefined) camRetries = 0;  // Stream läuft wieder
  const parts = [`${(c.fps || 0).toFixed(1)} fps`, `${c.kb || 0} KB/Bild`, `${((c.kbps || 0) / 1000).toFixed(1)} Mbit/s`];
  if (c.bt_fs) {  // Bluetooth an: kleinere Auflösung
    const f = ((S.camera && S.camera.framesizes) || []).find((x) => x.id === c.bt_fs);
    parts.push(`Bluetooth: ${f ? f.name.split(' ')[0] : 'kleiner'}`);
  }
  if (c.wake_ms) parts.push(`Start ${c.wake_ms} ms`);
  if (t.wifi && t.wifi.rssi !== undefined) parts.push(`WLAN ${t.wifi.rssi} dBm`);
  if (c.err) parts.push(`Kamera-Aussetzer ${c.err}`);
  el.textContent = parts.join(' · ');
}

function scheduleCamRetry() {
  clearTimeout(camRetryTimer);
  camRetries += 1;
  $('cam-ph-text').textContent = `Stream unterbrochen – verbinde neu (Versuch ${camRetries}) …`;
  camRetryTimer = setTimeout(() => {
    camRetryTimer = null;
    const img = $('cam-img');
    if (S.camWanted && S.tab === 'control' && !document.hidden) {
      img.src = `${camBase()}/stream?t=${Date.now()}`;
      img.parentElement.classList.add('live');
    }
  }, Math.min(1000 * camRetries, 5000));
}

function updateStream() {
  const img = $('cam-img');
  const live = S.camWanted && S.tab === 'control' && !document.hidden;
  const view = img.parentElement;
  if (live && !view.classList.contains('live') && !camRetryTimer) {
    img.src = `${camBase()}/stream?t=${Date.now()}`;
    view.classList.add('live');
  } else if (!live) {
    stopRecording();
    clearTimeout(camRetryTimer);
    camRetryTimer = null;
    camRetries = 0;
    $('cam-ph-text').textContent = 'Stream gestoppt';
    if (view.classList.contains('live')) {
      img.src = 'data:,';  // Verbindung schließen
      view.classList.remove('live');
    }
  }
  if (!live) $('cam-live').hidden = true;
  $('cam-toggle-text').textContent = S.camWanted ? 'Stream stoppen' : 'Stream starten';
  $('cam-toggle-ic').setAttribute('href', S.camWanted ? '#i-stop' : '#i-play');
  $('cam-toggle').classList.toggle('btn-primary', !S.camWanted);
}

// Künstlicher Horizont über dem Kamerabild (Lage aus dem MPU6050)
function renderHud(imu) {
  const hud = $('hud');
  hud.hidden = !S.hud || !imu || !imu.ok;
  if (hud.hidden) return;
  // + Nick = Nase hoch -> Horizont wandert im Bild nach unten (ca. 1,5 % der Bildhöhe je Grad)
  $('hud-sky').style.transform = `translateY(${clamp(imu.pitch, -30, 30) * 1.5}cqh) rotate(${-imu.roll}deg)`;
  $('hud-pitch').textContent = fmtDeg(imu.pitch) + '°';
  $('hud-roll').textContent = fmtDeg(imu.roll) + '°';
}

// Rand abschneiden (Kamera sieht z. B. die Platine vor sich)
function renderCrop(c) {
  const r = $('cam-crop');
  if (document.activeElement !== r) r.value = c.crop || 0;
  $('cam-crop-val').textContent = `${r.value} %`;
  $('cam-crop-edge').value = c.crop_edge || 'bottom';
  const lim = c.crop_active !== undefined && c.crop_active < (c.crop || 0)
    ? ` Bei dieser Auflösung höchstens ${c.crop_active} %.` : '';
  $('cam-crop-note').textContent = c.zoom_hw
    ? `Der Sensor liest diesen Streifen gar nicht erst aus; das Bild behält sein Format, links und rechts fällt je die Hälfte so viel weg.${lim}`
    : 'Die Oberfläche blendet den Streifen aus (Bild wird entsprechend vergrößert).';
}

function renderCamera() {
  const c = S.camera;
  if (!c) return;
  const sel = $('cam-fs');
  if (!sel.options.length) {
    for (const f of c.framesizes || []) sel.add(new Option(f.name, f.id));
  }
  sel.value = c.framesize;
  const bt = $('cam-btfs');
  if (!bt.options.length) {
    bt.add(new Option('wie oben', 0));
    for (const f of c.framesizes || []) bt.add(new Option(`höchstens ${f.name}`, f.id));
  }
  bt.value = c.bt_framesize || 0;
  $('cam-q').value = c.quality;
  $('cam-q-val').textContent = c.quality;
  $('cam-fps').value = c.max_fps;
  $('cam-fps-val').textContent = `${c.max_fps} fps`;
  $('cam-rot').value = c.rotation || 0;
  $('cam-xclk').value = c.xclk_mhz || 20;
  $('cam-xclk-note').hidden = !c.xclk_active_mhz || c.xclk_mhz === c.xclk_active_mhz;
  // 90°/270°: Sensor liefert 0°/180°, die Oberfläche dreht um weitere 90°
  $('cam-img').parentElement.classList.toggle('rot90', c.rotation === 90 || c.rotation === 270);
  $('cam-prev-view').classList.toggle('rot90', c.rotation === 90 || c.rotation === 270);
  $('cam-vflip').checked = c.vflip;
  $('cam-hmirror').checked = c.hmirror;
  $('cam-info').textContent = c.ok ? (c.sensor || '') : 'Keine Kamera erkannt';
  // Zoom vom Roboter übernehmen (z. B. kleiner nach einem Auflösungswechsel), außer er wird gerade verstellt
  if (c.zoom_hw && !ZOOM.pending && !ZOOM.busy && c.zoom !== undefined) ZOOM.z = ZOOM.raw = c.zoom;
  else if (!c.zoom_hw) ZOOM.z = ZOOM.raw = Math.min(ZOOM.z, zoomMax());
  renderZoom();
  renderCrop(c);
  renderCamImage(c);
  $('cam-toggle').disabled = !c.ok;
}

async function loadCamera() {
  try {
    S.camera = await api('/api/camera');
    renderCamera();
  } catch (e) {
    $('cam-info').textContent = 'Kamera-Status nicht abrufbar';
  }
}

// Bild-Vorgaben (Werte für den Sensor; was nicht passt, zeigt „Eigene Einstellung“)
const CAM_SCENES = [
  { id: 'auto', name: 'Automatisch', hint: 'Werkseinstellung des Sensors',
    v: { brightness: 0, contrast: 0, saturation: 0, sharpness: 0, ae_level: 0, gainceiling: 2, wb_mode: 0, aec2: false } },
  { id: 'motion', name: 'Bewegung', hint: 'kürzere Belichtung: weniger Unschärfe beim Laufen, etwas mehr Rauschen',
    v: { brightness: 1, contrast: 1, saturation: 0, sharpness: 1, ae_level: -1, gainceiling: 5, wb_mode: 0, aec2: false } },
  { id: 'indoor', name: 'Innen', hint: 'heller bei Raumlicht',
    v: { brightness: 1, contrast: 0, saturation: 0, sharpness: 0, ae_level: 1, gainceiling: 4, wb_mode: 0, aec2: true } },
  { id: 'outdoor', name: 'Draußen', hint: 'kräftige Farben, kurze Belichtung bei Tageslicht',
    v: { brightness: 0, contrast: 1, saturation: 1, sharpness: 1, ae_level: -1, gainceiling: 1, wb_mode: 0, aec2: false } },
  { id: 'night', name: 'Nacht', hint: 'so hell wie möglich – verrauscht, Farben gedämpft',
    v: { brightness: 2, contrast: 0, saturation: -1, sharpness: 0, ae_level: 2, gainceiling: 6, wb_mode: 0, aec2: true } },
];
const CAM_SLIDERS = { 'cam-br': 'brightness', 'cam-ae': 'ae_level', 'cam-gc': 'gainceiling', 'cam-ct': 'contrast',
  'cam-sa': 'saturation', 'cam-sh': 'sharpness' };
const fmtLevel = (v) => (v > 0 ? `+${v}` : `${v}`);
const fmtGain = (v) => `${2 ** (v + 1)}×`;

function camScene(img, sharpOk) {
  return CAM_SCENES.find((sc) => Object.entries(sc.v).every(([k, v]) => (k === 'sharpness' && !sharpOk) || img[k] === v));
}

function renderCamImage(c) {
  const img = c.image;
  if (!img) return;
  const box = $('cam-scenes');
  if (!box.children.length) {
    for (const sc of CAM_SCENES) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'profile';
      b.dataset.scene = sc.id;
      b.textContent = sc.name;
      b.title = sc.hint;
      b.onclick = () => setCamera(sc.v);
      box.appendChild(b);
    }
  }
  const active = camScene(img, c.sharpness_ok);
  box.querySelectorAll('.profile').forEach((b) => b.classList.toggle('active', !!active && b.dataset.scene === active.id));
  $('cam-scene-name').textContent = active ? active.hint : 'eigene Einstellung';
  for (const [id, key] of Object.entries(CAM_SLIDERS)) {
    if (document.activeElement !== $(id)) $(id).value = img[key];
    $(`${id}-val`).textContent = key === 'gainceiling' ? fmtGain(+$(id).value) : fmtLevel(+$(id).value);
  }
  document.querySelectorAll('.cam-sharp').forEach((el) => { el.hidden = !c.sharpness_ok; });
  $('cam-wb').value = img.wb_mode;
  $('cam-aec2').checked = img.aec2;
  $('cam-lenc').checked = img.lenc;
  $('cam-bpc').checked = img.bpc;
}

// ---- Zoom: der OV5640 liest nur einen Ausschnitt des Sensors (echte Details, zoom_hw), sonst
// vergrößert die Oberfläche das Bild. Sensor-Zoom wird nicht gespeichert (nach dem Neustart 1×).
const ZOOM = { z: 1, raw: 1, timer: 0, busy: false, pending: false, pinch: null };
const zoomMax = () => (S.camera && S.camera.zoom_max) || 4;
const fmtZoom = (z) => `${(Math.round(z * 10) / 10).toLocaleString('de-DE')}×`;

function renderZoom() {
  const c = S.camera || {};
  const txt = fmtZoom(ZOOM.z);
  for (const id of ['cam-zoom', 'fpv-zoom']) {
    $(id).textContent = txt;
    $(id).classList.toggle('active', ZOOM.z > 1);
    $(id).disabled = !c.ok;
  }
  // ohne Sensor-Zoom: Bild in der Oberfläche vergrößern und den Rand abschneiden (gleiche Wirkung:
  // sichtbar bleibt der Teil ohne den Streifen, im gleichen Seitenverhältnis, dazu der Zoom)
  const crop = c.zoom_hw ? 0 : (c.crop || 0) / 100;
  const scale = c.zoom_hw ? 1 : ZOOM.z / (1 - crop);
  // Mitte des sichtbaren Teils im Bild (0..1); 90°/270°: unten im Bild = rechts im Stream
  const near = (1 - crop) / 2, far = 1 - near;
  const side = c.crop_edge === 'top' ? near : far;
  const rot90 = c.rotation === 90 || c.rotation === 270;
  const cx = rot90 ? 1 - side : 0.5, cy = rot90 ? 0.5 : 1 - side;
  for (const id of ['cam-view', 'cam-prev-view']) {
    const st = $(id).style;
    st.setProperty('--zoom', scale);
    st.setProperty('--cam-tx', `${(scale * (0.5 - cx) * 100).toFixed(2)}%`);
    st.setProperty('--cam-ty', `${(scale * (0.5 - cy) * 100).toFixed(2)}%`);
  }
  const r = $('cam-zoom-r');
  r.max = zoomMax();
  if (document.activeElement !== r) r.value = ZOOM.z;
  $('cam-zoom-val').textContent = txt;
  $('cam-zoom-note').textContent = c.zoom_hw
    ? `Sensor-Zoom: Ausschnitt mit voller Sensor-Auflösung, bei dieser Auflösung bis ${fmtZoom(zoomMax())} (kleinere Auflösung = mehr Zoom). Wird nicht gespeichert.`
    : 'Digitaler Zoom: das Bild wird nur vergrößert.';
}

function setZoom(z, raw) {
  ZOOM.raw = clamp(raw === undefined ? z : raw, 1, zoomMax());
  z = clamp(Math.round(ZOOM.raw * 20) / 20, 1, zoomMax());
  if (z === ZOOM.z) return;
  ZOOM.z = z;
  renderZoom();
  if (!S.camera || !S.camera.zoom_hw) return;  // digital: nur die Anzeige
  ZOOM.pending = true;
  if (!ZOOM.timer) ZOOM.timer = setTimeout(sendZoom, 120);  // Mausrad/Finger: höchstens ~8 Anfragen/s
}

async function sendZoom() {
  ZOOM.timer = 0;
  if (ZOOM.busy) { ZOOM.timer = setTimeout(sendZoom, 120); return; }
  ZOOM.busy = true;
  ZOOM.pending = false;
  try {
    S.camera = await api('/api/camera', { zoom: ZOOM.z });
  } catch (e) {
    toast(e.message, true);
  }
  ZOOM.busy = false;
  if (!ZOOM.pending && S.camera) renderCamera();
}

// 1× → 2× → 3× → 4× (bzw. was die Auflösung erlaubt) → 1×
function cycleZoom() {
  const m = zoomMax();
  const steps = [...[2, 3, 4].filter((v) => v < m - 0.01), m];
  setZoom(steps.find((v) => v > ZOOM.z + 0.01) || 1);
}

function bindZoomGestures(view) {
  view.addEventListener('wheel', (e) => {
    if (!view.classList.contains('live') || !(S.camera && S.camera.ok)) return;
    e.preventDefault();
    const d = e.deltaY * (e.deltaMode === 1 ? 16 : 1);
    setZoom(0, ZOOM.raw * Math.exp(-clamp(d, -200, 200) * 0.0025));
  }, { passive: false });
  const dist = (t) => Math.hypot(t[0].clientX - t[1].clientX, t[0].clientY - t[1].clientY);
  view.addEventListener('touchstart', (e) => {
    if (e.touches.length === 2) ZOOM.pinch = { d: dist(e.touches), z: ZOOM.raw };
  }, { passive: true });
  view.addEventListener('touchmove', (e) => {
    if (e.touches.length !== 2 || !ZOOM.pinch || !view.classList.contains('live')) return;
    e.preventDefault();
    setZoom(0, ZOOM.pinch.z * dist(e.touches) / Math.max(ZOOM.pinch.d, 1));
  }, { passive: false });
  view.addEventListener('touchend', (e) => { if (e.touches.length < 2) ZOOM.pinch = null; });
  view.addEventListener('dblclick', () => { if (view.classList.contains('live')) setZoom(ZOOM.z > 1 ? 1 : 2); });
}

async function setCamera(patch) {
  try {
    S.camera = await api('/api/camera', patch);
    renderCamera();
  } catch (e) {
    toast(e.message, true);
  }
}

// ---- Video aufnehmen: Stream in ein Canvas zeichnen und im Browser aufnehmen (MediaRecorder)
const REC = { rec: null, chunks: [], raf: 0, timer: 0, start: 0, ext: 'webm' };

function recMime() {
  if (!window.MediaRecorder) return '';
  const types = ['video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm', 'video/mp4'];
  return types.find((t) => MediaRecorder.isTypeSupported(t)) || '';
}

async function startRecording() {
  const img = $('cam-img');
  const view = img.parentElement;
  const mime = recMime();
  if (!mime || !HTMLCanvasElement.prototype.captureStream) {
    toast('Videoaufnahme wird von diesem Browser nicht unterstützt', true);
    return;
  }
  if (!view.classList.contains('live')) {
    toast('Erst den Stream starten', true);
    return;
  }
  // Das Bild kommt von Port 81: ohne CORS-Freigabe dürfte das Canvas nicht aufgenommen werden
  if (img.crossOrigin !== 'anonymous') {
    img.crossOrigin = 'anonymous';
    img.src = `${camBase()}/stream?t=${Date.now()}`;
    const t0 = Date.now();
    while (!img.naturalWidth && Date.now() - t0 < 4000) await sleep(100);
  }
  if (!img.naturalWidth) {
    toast('Kein Kamerabild für die Aufnahme', true);
    return;
  }
  const rot = (S.camera && S.camera.rotation) || 0;
  const turn = rot === 90 || rot === 270;  // Sensor liefert 0°/180°, die Oberfläche dreht um 90°
  const canvas = document.createElement('canvas');
  const w = img.naturalWidth, h = img.naturalHeight;
  canvas.width = turn ? h : w;
  canvas.height = turn ? w : h;
  const ctx = canvas.getContext('2d');
  const draw = () => {
    if (img.naturalWidth) {
      ctx.save();
      if (turn) {
        ctx.translate(canvas.width / 2, canvas.height / 2);
        ctx.rotate(Math.PI / 2);
        ctx.drawImage(img, -w / 2, -h / 2, w, h);
      } else {
        ctx.drawImage(img, 0, 0, w, h);
      }
      ctx.restore();
    }
    REC.raf = requestAnimationFrame(draw);
  };
  draw();
  let stream;
  try {
    stream = canvas.captureStream(25);
    REC.rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: 2500000 });
  } catch (e) {
    cancelAnimationFrame(REC.raf);
    toast(`Aufnahme nicht möglich: ${e.message}`, true);
    return;
  }
  REC.ext = mime.includes('mp4') ? 'mp4' : 'webm';
  REC.chunks = [];
  REC.rec.ondataavailable = (e) => { if (e.data.size) REC.chunks.push(e.data); };
  REC.rec.onstop = () => {
    cancelAnimationFrame(REC.raf);
    clearInterval(REC.timer);
    const blob = new Blob(REC.chunks, { type: mime });
    const a = document.createElement('a');
    const ts = new Date().toISOString().slice(0, 19).replace(/[-:]/g, '').replace('T', '-');
    a.href = URL.createObjectURL(blob);
    a.download = `hexapod-${ts}.${REC.ext}`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    toast(`Video gespeichert (${(blob.size / 1048576).toFixed(1)} MB)`);
    REC.rec = null;
    renderRecording();
  };
  REC.rec.start(1000);
  REC.start = Date.now();
  REC.timer = setInterval(renderRecording, 500);
  renderRecording();
}

function stopRecording() {
  if (REC.rec && REC.rec.state !== 'inactive') REC.rec.stop();
}

function renderRecording() {
  const on = !!REC.rec;
  const btn = $('cam-rec');
  btn.classList.toggle('recording', on);
  btn.title = on ? 'Aufnahme beenden und speichern' : 'Video aufnehmen (im Browser, als Datei speichern)';
  $('cam-img').parentElement.classList.toggle('recording', on);
  const t = $('cam-rec-time');
  t.hidden = !on;
  if (on) {
    const s = Math.floor((Date.now() - REC.start) / 1000);
    t.textContent = `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
  }
}

function bindCamera() {
  $('cam-rec').onclick = () => (REC.rec ? stopRecording() : startRecording());
  $('cam-snap').href = `${camBase()}/capture`;
  $('cam-toggle').onclick = () => {
    S.camWanted = !S.camWanted;
    updateStream();
  };
  try { S.hud = localStorage.getItem('camHud') === '1'; } catch (e) { /* kein Speicher */ }
  $('cam-hud').setAttribute('aria-pressed', S.hud);
  $('cam-hud').onclick = () => {
    S.hud = !S.hud;
    $('cam-hud').setAttribute('aria-pressed', S.hud);
    try { localStorage.setItem('camHud', S.hud ? '1' : '0'); } catch (e) { /* egal */ }
    if (S.tel) renderHud(S.tel.imu);
  };
  const view = $('cam-view');
  $('cam-full').hidden = !document.fullscreenEnabled || !view.requestFullscreen;
  $('cam-full').onclick = () => {
    if (document.fullscreenElement) document.exitFullscreen();
    else view.requestFullscreen().catch(() => toast('Vollbild nicht möglich', true));
  };
  // Bricht der Stream ab (WLAN-Aussetzer), automatisch neu verbinden
  $('cam-img').onerror = () => {
    const view = $('cam-img').parentElement;
    if (!view.classList.contains('live') || !S.camWanted) return;
    view.classList.remove('live');
    scheduleCamRetry();
  };
  $('cam-xclk').onchange = (e) => setCamera({ xclk_mhz: +e.target.value });
  $('cam-fs').onchange = (e) => setCamera({ framesize: +e.target.value });
  $('cam-btfs').onchange = (e) => setCamera({ bt_framesize: +e.target.value });
  for (const [id, key] of Object.entries(CAM_SLIDERS)) {
    $(id).oninput = (e) => {
      const v = +e.target.value;
      $(`${id}-val`).textContent = key === 'gainceiling' ? fmtGain(v) : fmtLevel(v);
    };
    $(id).onchange = (e) => setCamera({ [key]: +e.target.value });
  }
  $('cam-wb').onchange = (e) => setCamera({ wb_mode: +e.target.value });
  for (const id of ['cam-aec2', 'cam-lenc', 'cam-bpc']) {
    $(id).onchange = (e) => setCamera({ [e.target.dataset.key]: e.target.checked });
  }
  $('cam-q').oninput = (e) => { $('cam-q-val').textContent = e.target.value; };
  $('cam-q').onchange = (e) => setCamera({ quality: +e.target.value });
  $('cam-fps').oninput = (e) => { $('cam-fps-val').textContent = `${e.target.value} fps`; };
  $('cam-fps').onchange = (e) => setCamera({ max_fps: +e.target.value });
  $('cam-rot').onchange = (e) => setCamera({ rotation: +e.target.value });
  $('cam-zoom').onclick = cycleZoom;
  $('fpv-zoom').onclick = cycleZoom;
  $('cam-zoom-r').oninput = (e) => setZoom(+e.target.value);
  $('cam-crop').oninput = (e) => { $('cam-crop-val').textContent = `${e.target.value} %`; };
  $('cam-crop').onchange = (e) => setCamera({ crop: +e.target.value });
  $('cam-crop-edge').onchange = (e) => setCamera({ crop_edge: e.target.value });
  bindZoomGestures($('cam-view'));
  bindZoomGestures($('cam-prev-view'));
  $('cam-vflip').onchange = (e) => setCamera({ vflip: e.target.checked });
  $('cam-hmirror').onchange = (e) => setCamera({ hmirror: e.target.checked });
  document.addEventListener('visibilitychange', updateStream);
  document.addEventListener('visibilitychange', updatePreview);
  $('cam-prev').onerror = () => {
    const view = $('cam-prev-view');
    if (!view.classList.contains('live')) return;
    view.classList.remove('live');
    $('cam-prev-text').textContent = 'Kamera nicht erreichbar – neuer Versuch …';
    setTimeout(updatePreview, 3000);
  };
}

// -------------------------------------------------------------- System ------

function fmtBytes(b) {
  if (b >= 1048576) return `${(b / 1048576).toFixed(1)} MB`;
  return `${Math.round(b / 1024)} KB`;
}

function fmtUptime(s) {
  if (s < 60) return `${s} s`;
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return (d ? `${d} d ` : '') + `${h} h ${m} min`;
}

function fillKv(el, rows) {
  el.innerHTML = '';
  for (const [k, v] of rows) {
    const dt = document.createElement('dt');
    dt.textContent = k;
    const dd = document.createElement('dd');
    dd.textContent = v;
    el.append(dt, dd);
  }
}

const BAT_TEXT = { off: 'Überwachung aus', usb: 'kein Akku (USB)', ok: 'OK', warn: 'schwach', critical: 'leer!' };
const BAT_CLS = { ok: 'ok', warn: 'warn', critical: 'bad' };

function wifiQuality(rssi) {
  if (rssi >= -55) return ['ok', 'sehr gut'];
  if (rssi >= -67) return ['ok', 'gut'];
  if (rssi >= -75) return ['warn', 'mäßig'];
  return ['bad', 'schwach'];
}

function setTile(key, value, sub) {
  $(`tile-${key}-val`).textContent = value;
  const el = $(`tile-${key}-sub`);
  if (sub instanceof Node) el.replaceChildren(sub);
  else el.textContent = sub || '';
}

// Kennzahlen der Systemübersicht (Telemetrie live, Rest aus /api/status)
function renderTiles() {
  const t = S.tel;
  const s = S.status;

  const b = (t && t.bat) || (s && s.bat);
  const meter = $('tile-bat-meter');
  if (!b) setTile('bat', '–');
  else if (b.state === 'off' || b.state === 'usb') {
    meter.hidden = true;
    setTile('bat', b.state === 'usb' ? 'USB' : 'aus', b.state === 'usb' ? 'kein Akku erkannt' : 'Überwachung aus');
  } else {
    meter.hidden = false;
    meter.className = 'meter ' + (BAT_CLS[b.state] || '');
    meter.firstElementChild.style.width = `${clamp(b.pct, 0, 100)}%`;
    setTile('bat', `${b.v.toFixed(2)} V`, statusEl(BAT_CLS[b.state] || 'warn', `${b.pct} % · ${BAT_TEXT[b.state] || b.state}`));
  }

  const w = s && s.wifi;
  let rssi = t && t.wifi ? t.wifi.rssi : undefined;
  if (rssi === undefined && w && w.sta.state === 'connected') rssi = w.sta.rssi;
  if (rssi !== undefined) {
    const [cls, text] = wifiQuality(rssi);
    setTile('wifi', `${rssi} dBm`, statusEl(cls, text + (w && w.sta.ssid ? ` · ${w.sta.ssid}` : '') + (S.rtt ? ` · ${Math.round(S.rtt)} ms` : '')));
  } else if (w && w.ap.active) {
    setTile('wifi', 'Access Point', `„${w.ap.ssid}“ · ${w.ap.clients} Gerät(e)`);
  } else setTile('wifi', '–');

  if (t && t.ctl) {
    const ok = t.ctl.hz >= 45;
    setTile('ctl', `${Math.round(t.ctl.hz)} Hz`, statusEl(ok ? 'ok' : 'warn',
      `Soll 50 · max. ${(t.ctl.max_us / 1000).toFixed(1)} ms`));
  }

  const c = t && t.cam;
  if (s && s.camera && !s.camera.ok) setTile('cam', 'fehlt', statusEl('bad', 'keine Kamera erkannt'));
  else if (c && c.clients && c.fps !== undefined) {
    setTile('cam', `${c.fps.toFixed(1)} fps`, `${c.kb} KB/Bild · ${(c.kbps / 1000).toFixed(1)} Mbit/s`);
  } else if (c && c.awake !== undefined) {
    setTile('cam', c.awake ? 'aktiv' : 'schläft', c.awake ? 'ohne Stream gleich im Schlaf' : 'wacht mit dem Stream auf');
  }

  const heap = t && t.heap !== undefined ? t.heap : s && s.mem.heap_free;
  if (heap != null) {
    setTile('mem', fmtBytes(heap), s ? `Heap · min. ${fmtBytes(s.mem.heap_min)}` : 'Heap');
  }

  const up = t && t.uptime !== undefined ? t.uptime : s && s.uptime;
  if (up != null) setTile('up', fmtUptime(up), s ? `Firmware ${s.fw}` : '');
}

// Version: Sidebar, Einstellungen; nach einem Update mit noch offener alter Seite -> neu laden
async function checkVersion() {
  try {
    renderVersion(await api('/api/status'));
  } catch (e) { /* später erneut */ }
}

function renderVersion(s) {
  if (!s || !s.fw) return;
  const stale = s.ui && !UI_VERSION.startsWith('__') && s.ui !== UI_VERSION;
  $('sb-ver').textContent = `v${s.fw}`;
  $('sb-ver').title = `Firmware ${s.fw}${s.git ? ' · ' + s.git : ''}${s.build ? ' · gebaut ' + s.build : ''}`;
  fillKv($('about-info'), [
    ['Firmware', s.fw],
    ['Stand (Git)', s.git ? s.git.replace(/\+$/, ' (mit lokalen Änderungen)') : '–'],
    ['Gebaut', s.build || '–'],
    ['Oberfläche', UI_VERSION.startsWith('__') ? 'Entwicklung (nicht eingebettet)' : UI_VERSION],
    ['Chip', `${s.chip.model} rev ${s.chip.rev}`],
  ]);
  $('about-stale').hidden = !stale;
  if (stale && !S.staleShown) {
    S.staleShown = true;
    toast(`Neue Firmware ${s.fw} – Seite neu laden`, true);
  }
  $('sb-ver').classList.toggle('stale', !!stale);
}

// --------------------------------------------------------------- Profile ----

// Haltung + Gang auf einen Klick (alle in der Firmware erreichbar, Tempo bei 100 %)
const PROFILES = [
  { id: 'normal', name: 'Normal', hint: 'Standard, 175 mm/s', z: 52, stance: 100, type: 'tripod', swing_s: 0.4, step_height: 15 },
  { id: 'stable', name: 'Stabil', hint: 'tief und breit, Ripple (immer 4 Beine am Boden)', z: 42, stance: 115, type: 'ripple', swing_s: 0.45, step_height: 12 },
  { id: 'terrain', name: 'Gelände', hint: 'hoch, Fuß 25 mm anheben', z: 70, stance: 100, type: 'tripod', swing_s: 0.5, step_height: 25 },
  { id: 'sport', name: 'Sport', hint: 'schnelle Schritte, bis 216 mm/s', z: 55, stance: 95, type: 'tripod', swing_s: 0.3, step_height: 15 },
  { id: 'sneak', name: 'Schleichen', hint: 'Welle, langsam und leise', z: 45, stance: 105, type: 'wave', swing_s: 0.5, step_height: 10 },
  { id: 'climb', name: 'Klettern', hint: 'über Kanten: Körper höher, Füße bis 50 mm hoch', z: 62, stance: 105, type: 'climb', swing_s: 0.4, step_height: 15 },
];
const PROFILE_MAX_OWN = 4;

function ownProfiles() {
  try { return JSON.parse(localStorage.getItem('profiles') || '[]'); } catch (e) { return []; }
}

function saveOwnProfiles(list) {
  try { localStorage.setItem('profiles', JSON.stringify(list)); } catch (e) { toast('Speichern im Browser nicht möglich', true); }
}

function applyProfile(p) {
  const pose = { z: p.z, stance: p.stance, x: p.x || 0, y: p.y || 0, roll: p.roll || 0, pitch: p.pitch || 0, yaw: p.yaw || 0 };
  if (!send('pose', pose)) return;
  send('gait', { type: p.type, swing_s: p.swing_s, step_height: p.step_height });
  const m = S.tel && S.tel.motion;
  toast(`Profil „${p.name}“${m && m.gait && m.gait.walking && m.gait.active !== p.type ? ' – Gangart wechselt im Lauf' : ''}`);
}

// aktives Profil: Haltung und Gang stimmen (fast) überein
function profileMatches(p, m) {
  const ps = m.pose || {};
  const g = m.gait || {};
  const near = (a, b, tol) => Math.abs((a || 0) - (b || 0)) <= tol;
  return near(ps.z, p.z, 1) && near(ps.stance, p.stance, 1) && near(ps.x, p.x, 1) && near(ps.y, p.y, 1) &&
    near(ps.roll, p.roll, 1) && near(ps.pitch, p.pitch, 1) && near(ps.yaw, p.yaw, 1) &&
    g.type === p.type && near(g.swing_s, p.swing_s, 0.01) && near(g.step_height, p.step_height, 0.5);
}

function renderProfiles() {
  const box = $('profiles');
  const own = ownProfiles();
  box.innerHTML = '';
  const add = (p, ownIdx) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'profile';
    b.dataset.profile = p.id;
    b.title = p.hint || `eigenes Profil: Höhe ${p.z} mm, Fußabstand ${p.stance} mm, ${p.type}`;
    b.textContent = p.name;
    b.onclick = () => applyProfile(p);
    if (ownIdx === undefined) {
      box.appendChild(b);
      return;
    }
    // eigenes Profil: Löschen als eigener Knopf daneben (nicht im Anwenden-Knopf –
    // ein knapp verfehltes × hätte sonst das Profil angewendet und den Roboter bewegt)
    const group = document.createElement('span');
    group.className = 'profile-own';
    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'profile-del';
    del.textContent = '×';
    del.title = 'Profil löschen';
    del.setAttribute('aria-label', `Profil „${p.name}“ löschen`);
    del.onclick = () => {
      if (!confirm(`Profil „${p.name}“ löschen?`)) return;
      const list = ownProfiles();
      list.splice(ownIdx, 1);
      saveOwnProfiles(list);
      renderProfiles();
    };
    group.append(b, del);
    box.appendChild(group);
  };
  PROFILES.forEach((p) => add(p));
  own.forEach((p, i) => add(p, i));
  if (own.length < PROFILE_MAX_OWN) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'profile profile-add';
    b.textContent = '+ Merken';
    b.title = 'Aktuelle Haltung und Gangart als eigenes Profil speichern (in diesem Browser)';
    b.onclick = saveProfile;
    box.appendChild(b);
  }
  markProfile();
}

function saveProfile() {
  const m = S.tel && S.tel.motion;
  if (!m || !m.pose || !m.gait) {
    toast('Keine Verbindung zum Hexapod', true);
    return;
  }
  const name = (prompt('Name für das Profil:', 'Mein Profil') || '').trim().slice(0, 20);
  if (!name) return;
  const ps = m.pose;
  const g = m.gait;
  const list = ownProfiles();
  list.push({
    id: `own-${Date.now()}`, name, z: Math.round(ps.z), stance: Math.round(ps.stance), x: Math.round(ps.x), y: Math.round(ps.y),
    roll: Math.round(ps.roll), pitch: Math.round(ps.pitch), yaw: Math.round(ps.yaw),
    type: g.type, swing_s: g.swing_s, step_height: g.step_height,
  });
  saveOwnProfiles(list);
  renderProfiles();
  toast(`Profil „${name}“ gespeichert`);
}

function markProfile() {
  const m = S.tel && S.tel.motion;
  if (!m || !m.pose) return;
  const all = PROFILES.concat(ownProfiles());
  document.querySelectorAll('#profiles .profile[data-profile]').forEach((b) => {
    const p = all.find((x) => x.id === b.dataset.profile);
    b.classList.toggle('active', !!p && profileMatches(p, m));
  });
}

// -------------------------------------------------------- Fahrer-Ansicht ----

const FPV = { open: false, home: null, next: null, gesture: 0 };
const FPV_GESTURES = [['circle', 'Kreisen'], ['nod', 'Ja'], ['shake', 'Nein'], ['pushup', 'Liegestütz'], ['dance', 'Tanzen']];

// Kameraansicht ins Vollbild-Overlay verschieben (kein zweiter Stream nötig)
function openFpv() {
  if (FPV.open) return;
  FPV.open = true;
  v3Tick();  // 3D-Ansicht liegt jetzt verdeckt: keine Live-Daten
  const view = $('cam-view');
  FPV.home = view.parentElement;
  FPV.next = view.nextSibling;
  $('fpv-video').appendChild(view);
  $('fpv').hidden = false;
  document.documentElement.classList.add('fpv-on');
  setMoveMode(MOVE.mode);
  if (!S.camWanted) {
    S.camWanted = true;
    updateStream();
  }
  const el = $('fpv');
  if (el.requestFullscreen) {
    el.requestFullscreen({ navigationUI: 'hide' }).then(() => {
      // Handy quer (geht nur im Vollbild und nicht überall)
      if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(() => {});
    }).catch(() => {});
  }
  if (S.tel) renderFpv(S.tel);
}

function closeFpv() {
  if (!FPV.open) return;
  FPV.open = false;
  setTimeout(v3Tick, 0);  // 3D-Ansicht wieder sichtbar
  if (W.timer || W.keys.size || W.joyMove.some((v) => v) || W.joyTurn) walkStopAll();
  moveStopAll();
  FPV.home.insertBefore($('cam-view'), FPV.next);
  $('fpv').hidden = true;
  document.documentElement.classList.remove('fpv-on');
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
}

function renderFpv(t) {
  const m = t.motion || {};
  const st = $('walk-state');
  $('fpv-state').textContent = st.textContent;
  $('fpv-state').className = `fpv-chip${st.classList.contains('badge-on') ? ' on' : ''}${st.classList.contains('badge-warn') ? ' warn' : ''}`;
  const b = t.bat || {};
  $('fpv-bat').textContent = b.state === 'ok' || b.state === 'warn' || b.state === 'critical' ? `🔋 ${b.v.toFixed(1)} V` : '';
  $('fpv-bat').className = `fpv-chip${b.state === 'warn' || b.state === 'critical' ? ' warn' : ''}`;

  const posture = m.mode === 'pose' ? m.posture : null;
  const standing = posture === 'standing';
  const busy = posture === 'standing_up' || posture === 'sitting_down';
  const arms = m.mode === 'pose' && m.arms ? m.arms : 'down';
  const p = $('fpv-posture');
  p.hidden = MOVE.mode === 'arms';
  p.textContent = standing ? 'Hinsetzen' : 'Aufstehen';
  p.disabled = busy;
  $('fpv-arms').hidden = MOVE.mode !== 'arms';
  $('fpv-arms').textContent = $('arms-toggle').textContent.replace('Vorderbeine ', '');
  $('fpv-arms').disabled = $('arms-toggle').disabled;
  $('fpv-wave').hidden = MOVE.mode !== 'arms';
  $('fpv-wave').disabled = arms !== 'up';
  $('fpv-gesture').hidden = MOVE.mode !== 'body';
  $('fpv-gesture').textContent = m.gesture ? 'Stopp' : `Geste: ${FPV_GESTURES[FPV.gesture][1]}`;
  $('fpv-hud').setAttribute('aria-pressed', S.hud);

  // Infozeile: Tempo, Hinweise (Reichweite, gesperrt, Controller)
  const g = m.gait || {};
  const info = [];
  if (g.walking) info.push(`${g.vx} / ${g.vy} mm/s · ${g.wz} °/s`);
  if (!$('walk-limit').hidden) info.push($('walk-limit').textContent);
  if (t.pad && t.pad.src) info.push(`🎮 ${PAD_MODES[t.pad.mode] || ''}`);
  $('fpv-info').textContent = info.join(' · ');
}

function bindFpv() {
  $('cam-fpv').onclick = openFpv;
  $('fpv-exit').onclick = closeFpv;
  bindSeg('fpv-mode', setMoveMode);
  makeJoystick($('fpv-joy-l'), (x, y) => onStick('L', x, y));
  makeJoystick($('fpv-joy-r'), (x, y) => onStick('R', x, y));
  $('fpv-stop').onclick = () => $('btn-estop').click();
  $('fpv-hud').onclick = () => {
    $('cam-hud').click();
    $('fpv-hud').setAttribute('aria-pressed', S.hud);
  };
  $('fpv-posture').onclick = () => {
    const m = S.tel && S.tel.motion;
    send(m && m.mode === 'pose' && m.posture === 'standing' ? 'sit' : 'stand');
  };
  $('fpv-arms').onclick = () => $('arms-toggle').click();
  $('fpv-wave').onclick = () => send('wave');
  $('fpv-gesture').onclick = () => {
    const m = S.tel && S.tel.motion;
    if (m && m.gesture) {
      send('gesture', { name: 'stop' });
      return;
    }
    send('gesture', { name: FPV_GESTURES[FPV.gesture][0] });
    FPV.gesture = (FPV.gesture + 1) % FPV_GESTURES.length;
  };
}

// --------------------------------------------------------- Firmware-Update ----

const OTA = { busy: false, rebooting: false };

function otaStatus(text, cls = '') {
  $('ota-status').textContent = text;
  $('ota-status').className = `small ${cls}`;
}

function otaUpload() {
  const f = $('ota-file').files[0];
  if (!f || OTA.busy) return;
  if (!f.name.endsWith('.bin')) {
    otaStatus('Bitte eine .bin-Datei wählen', 'warn-text');
    return;
  }
  if (f.name.includes('factory')) {
    otaStatus('firmware.factory.bin ist für den Browser-Flasher – hier firmware.bin wählen', 'warn-text');
    return;
  }
  if (!confirm(`Firmware „${f.name}“ (${fmtBytes(f.size)}) aufspielen? Die Servos werden stromlos.`)) return;
  OTA.busy = true;
  $('ota-start').disabled = true;
  $('ota-bar').hidden = false;
  $('ota-progress').style.width = '0';
  otaStatus('Übertrage …');
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    const pct = Math.round((e.loaded / e.total) * 100);
    $('ota-progress').style.width = `${pct}%`;
    otaStatus(pct < 100 ? `Übertrage … ${pct} %` : 'Prüfe und schreibe …');
  };
  xhr.onload = () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* leer */ }
    OTA.busy = false;
    if (xhr.status === 200 && r.ok !== false) {
      OTA.rebooting = true;
      otaStatus('Erfolgreich – ESP32 startet neu, die Seite lädt danach neu …', 'ok-text');
      toast('Firmware übertragen – Neustart');
    } else {
      otaStatus(r.msg || `Fehler (HTTP ${xhr.status})`, 'warn-text');
      $('ota-start').disabled = false;
    }
  };
  xhr.onerror = () => {
    OTA.busy = false;
    otaStatus('Verbindung abgebrochen – Update nicht übernommen', 'warn-text');
    $('ota-start').disabled = false;
  };
  xhr.send(f);
}

function bindOta() {
  $('ota-file').onchange = () => {
    $('ota-start').disabled = !$('ota-file').files.length;
    otaStatus('');
  };
  $('ota-start').onclick = otaUpload;
}

// ------------------------------------------------------------ Ereignisse ----

const EV = { seq: 0, entries: [], seenWarn: 0, loading: false, uptime: null };

async function loadLog() {
  if (EV.loading) return;
  EV.loading = true;
  try {
    const r = await api(`/api/log?since=${EV.seq}`);
    if (r.seq < EV.seq) {  // ESP32 neu gestartet
      EV.entries = [];
      EV.seq = 0;
      EV.loading = false;
      return loadLog();
    }
    const now = Date.now();
    for (const e of r.entries) e.at = now - (r.uptime_ms - e.ms);
    EV.entries = EV.entries.concat(r.entries).slice(-100);
    EV.seq = r.seq;
    renderLog();
  } catch (e) { /* später erneut */ }
  EV.loading = false;
}

const fmtTime = (t) => new Date(t).toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit', second: '2-digit' });

function renderLog() {
  const list = $('evlog');
  list.innerHTML = '';
  for (const e of EV.entries.slice().reverse()) {  // neueste oben
    const li = document.createElement('li');
    li.className = e.lvl;
    li.innerHTML = '<time></time><span class="lvl"></span><span></span>';
    li.firstChild.textContent = fmtTime(e.at);
    li.firstChild.title = `${Math.round(e.ms / 1000)} s nach dem Start`;
    li.lastChild.textContent = e.text;
    list.appendChild(li);
  }
}

// Telemetrie: neue Einträge holen; Punkt im Menü bei ungesehenen Warnungen
function renderLogState(l) {
  if (!l) return;
  if (l.seq < EV.seq) EV.seq = Math.min(EV.seq, 0);  // Neustart: alles neu holen
  if (S.tab === 'system') {
    if (l.seq !== EV.seq) loadLog();
    EV.seenWarn = l.warn;
  }
  if (l.warn < EV.seenWarn) EV.seenWarn = 0;
  $('nav-sys-dot').hidden = !(l.warn > EV.seenWarn);
}

function logText() {
  const st = S.status;
  const head = st ? `Hexapod MK3 · Firmware ${st.fw} (${st.git}) · läuft seit ${Math.round((st.uptime || 0) / 60)} min\n` : '';
  return head + EV.entries.map((e) => `${fmtTime(e.at)} [${e.lvl}] ${e.text}`).join('\n');
}

// In die Zwischenablage: Clipboard-API gibt es nur mit HTTPS; über http (der Roboter)
// geht es mit einem versteckten Textfeld. Klappt beides nicht: als Datei anbieten.
async function copyText(text, fileName) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (e) { /* weiter mit dem Textfeld */ }
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.cssText = 'position:fixed;top:0;left:0;width:1px;height:1px;opacity:0';
  document.body.appendChild(ta);
  ta.select();
  ta.setSelectionRange(0, text.length);
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
  ta.remove();
  if (ok) return true;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  a.download = fileName;
  a.click();
  return false;
}

function bindLog() {
  $('log-copy').onclick = async () => {
    if (await copyText(logText(), 'hexapod-ereignisse.txt')) toast('Ereignisse kopiert – z. B. in den Chat einfügen');
    else toast('Als Datei gespeichert (Zwischenablage nicht verfügbar)');
  };
}

// ---- Verlauf (System): Akku, WLAN, Temperatur, Speicher der letzten 30 min
const HIST = [
  { key: 'bat', name: 'Akkuspannung', unit: 'V', fmt: (v) => v.toFixed(2), pad: 0.05 },
  { key: 'rssi', name: 'WLAN-Signal', unit: 'dBm', fmt: (v) => `${v}`, pad: 3, empty: 'nicht im Heimnetz' },
  { key: 'temp', name: 'Chip-Temperatur', unit: '°C', fmt: (v) => `${v}`, pad: 2 },
  { key: 'heap', name: 'Freier Speicher', unit: 'KB', fmt: (v) => `${v}`, pad: 4 },
];
const SVG_NS = 'http://www.w3.org/2000/svg';
const HW = 300, HH = 58;  // viewBox der Linien

function histAgo(i, n, period, ageMs) {
  const s = Math.round((n - 1 - i) * period + ageMs / 1000);
  if (s < 60) return s <= 5 ? 'gerade eben' : `vor ${s} s`;
  return `vor ${Math.floor(s / 60)} min ${s % 60 ? `${s % 60} s` : ''}`.trim();
}

function renderHistory(h) {
  const grid = $('hist-grid');
  if (!grid) return;
  grid.innerHTML = '';
  const n = (h.heap || []).length;
  for (const m of HIST) {
    const vals = h[m.key] || [];
    const box = document.createElement('div');
    box.className = 'hist';
    box.innerHTML = '<div class="hist-head"><span class="hist-name"></span><span class="hist-now"></span></div>';
    box.querySelector('.hist-name').textContent = m.name;
    const nums = vals.filter((v) => v !== null && v !== undefined);
    const now = vals.length ? vals[vals.length - 1] : null;
    const nowEl = box.querySelector('.hist-now');
    if (now !== null && now !== undefined) {
      nowEl.textContent = m.fmt(now);
      const u = document.createElement('small');
      u.textContent = m.unit;
      nowEl.appendChild(u);
    } else {
      nowEl.textContent = '–';
    }
    grid.appendChild(box);
    if (nums.length < 2) {
      const e = document.createElement('div');
      e.className = 'hist-empty';
      e.textContent = n < 2 ? 'sammelt Werte …' : (m.empty || 'keine Werte');
      box.appendChild(e);
      continue;
    }
    let lo = Math.min(...nums) - m.pad, hi = Math.max(...nums) + m.pad;
    const x = (i) => (n > 1 ? (i / (n - 1)) * HW : HW);
    const y = (v) => HH - ((v - lo) / (hi - lo)) * HH;
    // Linie mit Lücken (kein Wert = nicht verbunden o. Ä.)
    let line = '', area = '', seg = [];
    const flush = () => {
      if (seg.length > 1) {
        line += seg.map(([px, py], k) => `${k ? 'L' : 'M'}${px.toFixed(1)} ${py.toFixed(1)}`).join('');
        area += `M${seg[0][0].toFixed(1)} ${HH}` + seg.map(([px, py]) => `L${px.toFixed(1)} ${py.toFixed(1)}`).join('')
          + `L${seg[seg.length - 1][0].toFixed(1)} ${HH}Z`;
      }
      seg = [];
    };
    vals.forEach((v, i) => (v === null || v === undefined ? flush() : seg.push([x(i), y(v)])));
    flush();
    const svg = document.createElementNS(SVG_NS, 'svg');
    svg.setAttribute('viewBox', `0 0 ${HW} ${HH}`);
    svg.setAttribute('preserveAspectRatio', 'none');
    svg.setAttribute('class', 'hist-svg');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', `${m.name}: ${m.fmt(Math.min(...nums))} bis ${m.fmt(Math.max(...nums))} ${m.unit}`);
    svg.innerHTML = `<line class="hist-grid-line" x1="0" x2="${HW}" y1="${HH}" y2="${HH}"/>`
      + `<path class="hist-area" d="${area}"/><path class="hist-line" d="${line}"/>`
      + '<line class="hist-cross" y1="0" hidden/>';
    box.appendChild(svg);
    const foot = document.createElement('div');
    foot.className = 'hist-foot';
    foot.innerHTML = '<span>−30 min</span><span></span><span>jetzt</span>';
    foot.children[1].textContent = `${m.fmt(Math.min(...nums))} … ${m.fmt(Math.max(...nums))} ${m.unit}`;
    box.appendChild(foot);
    // Fadenkreuz + Tooltip (Maus, Finger, Tastatur)
    const tip = document.createElement('div');
    tip.className = 'hist-tip';
    tip.hidden = true;
    box.appendChild(tip);
    const dot = document.createElement('div');  // HTML statt SVG: die Linie ist gestreckt, ein Kreis wäre oval
    dot.className = 'hist-pt';
    dot.hidden = true;
    box.appendChild(dot);
    const cross = svg.querySelector('.hist-cross');
    let ki = n - 1;  // Tastatur: Pfeile links/rechts
    const show = (i) => {
      ki = i;
      const v = vals[i];
      cross.hidden = false;
      cross.setAttribute('x1', x(i));
      cross.setAttribute('x2', x(i));
      cross.setAttribute('y2', HH);
      const r = svg.getBoundingClientRect(), br = box.getBoundingClientRect();
      const px = r.left - br.left + (x(i) / HW) * r.width;
      dot.hidden = v === null || v === undefined;
      if (!dot.hidden) {
        dot.style.left = `${px}px`;
        dot.style.top = `${r.top - br.top + (y(v) / HH) * r.height}px`;
      }
      tip.hidden = false;
      tip.innerHTML = '<b></b><span></span>';
      tip.firstChild.textContent = v === null || v === undefined ? (m.empty || 'kein Wert') : `${m.fmt(v)} ${m.unit}`;
      tip.lastChild.textContent = histAgo(i, n, h.period_s || 10, h.age_ms || 0);
      tip.style.left = `${Math.max(60, Math.min(br.width - 60, px))}px`;
    };
    const hide = () => { cross.hidden = true; dot.hidden = true; tip.hidden = true; };
    const at = (e) => {
      const r = svg.getBoundingClientRect();
      return Math.max(0, Math.min(n - 1, Math.round(((e.clientX - r.left) / r.width) * (n - 1))));
    };
    svg.addEventListener('pointerenter', () => { S.histHover = true; });
    svg.addEventListener('pointermove', (e) => show(at(e)));
    svg.addEventListener('pointerdown', (e) => show(at(e)));
    svg.addEventListener('pointerleave', () => { S.histHover = false; hide(); });
    svg.tabIndex = 0;
    svg.addEventListener('focus', () => show(ki));
    svg.addEventListener('blur', hide);
    svg.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        ki = Math.max(0, Math.min(n - 1, ki + (e.key === 'ArrowLeft' ? -1 : 1)));
        show(ki);
        e.preventDefault();
      }
    });
  }
  // Tabelle: je Minute ein Wert (jeder 6.), neueste oben
  const t = $('hist-table');
  t.innerHTML = '';
  const head = t.insertRow();
  for (const label of ['Zeit', ...HIST.map((m) => `${m.name} (${m.unit})`)]) {
    const th = document.createElement('th');
    th.textContent = label;
    head.appendChild(th);
  }
  const step = Math.max(1, Math.round(60 / (h.period_s || 10)));
  for (let i = n - 1; i >= 0; i -= step) {
    const row = t.insertRow();
    row.insertCell().textContent = histAgo(i, n, h.period_s || 10, h.age_ms || 0);
    for (const m of HIST) {
      const v = (h[m.key] || [])[i];
      row.insertCell().textContent = v === null || v === undefined ? '–' : m.fmt(v);
    }
  }
}

async function loadHistory() {
  if (S.histHover) return;  // nicht unter dem Zeiger neu zeichnen
  try {
    renderHistory(await api('/api/history'));
  } catch (e) { /* ältere Firmware oder kurz nicht erreichbar */ }
}

async function loadStatus() {
  try {
    const s = await api('/api/status');
    S.status = s;
    renderVersion(s);
    if (S.tab === 'system') renderTiles();
    const w = s.wifi;
    fillKv($('sys-info'), [
      ['Firmware', `${s.fw}${s.git ? ' · ' + s.git : ''} (${s.build})`],
      ['Chip', `${s.chip.model} rev ${s.chip.rev}, ${s.chip.cores} Kerne, ${s.chip.mhz} MHz`],
      ['Heap frei', `${fmtBytes(s.mem.heap_free)} (min. ${fmtBytes(s.mem.heap_min)})`],
      ['PSRAM frei', s.mem.psram ? `${fmtBytes(s.mem.psram_free)} von ${fmtBytes(s.mem.psram)}` : 'kein PSRAM'],
      ['WLAN', [
        w.sta.state === 'connected' ? `${w.sta.ssid} (${w.sta.rssi} dBm)` : '',
        w.ap.active ? `Access Point „${w.ap.ssid}“ (${w.ap.clients} Geräte)` : '',
      ].filter(Boolean).join(' · ')],
      ['Adresse', `${w.ip} · ${w.host}`],
      ['Regel-Task', S.tel && S.tel.ctl
        ? `${S.tel.ctl.hz} Hz (Soll 50), max. ${(S.tel.ctl.max_us / 1000).toFixed(1)} ms Rechenzeit` : '–'],
      ['MPU6050', S.tel && S.tel.imu
        ? `WHO_AM_I ${hex(S.tel.imu.who || 0)}` + (S.tel.imu.ok ? `, ${S.tel.imu.hz} Hz` : ', antwortet nicht') +
          `, Lesefehler ${S.tel.imu.errors || 0}` : '–'],
    ]);
    const dev = $('sys-devices');
    dev.innerHTML = '';
    const names = { pca1: 'PCA9685 #1 (0x40, rechts)', pca2: 'PCA9685 #2 (0x41, links)', imu: 'MPU6050 (0x68)', camera: `Kamera${s.camera.sensor ? ' ' + s.camera.sensor : ''}` };
    for (const [key, label] of Object.entries(names)) {
      const li = document.createElement('li');
      const ok = s.devices[key];
      li.append(document.createElement('span'), statusEl(ok ? 'ok' : 'bad', ok ? 'OK' : 'fehlt'));
      li.firstChild.textContent = label;
      dev.appendChild(li);
    }
    renderI2c(s.i2c);
    renderBatterySettings(s.bat);
    renderWifi(w);
  } catch (e) {
    toast(`Status: ${e.message}`, true);
  }
}

const hex = (a) => '0x' + a.toString(16).toUpperCase().padStart(2, '0');

function renderI2c(i2c) {
  if (!i2c) return;
  $('i2c-info').textContent = `SDA GPIO ${i2c.sda} · SCL GPIO ${i2c.scl} · ${i2c.freq_khz} kHz` +
    (i2c.scanned ? ` · Scan vor ${i2c.age_s} s (${i2c.duration_ms} ms)` : ' · noch nicht gescannt');
  const list = $('i2c-list');
  list.innerHTML = '';
  const add = (addr, name, cls, status) => {
    const li = document.createElement('li');
    li.innerHTML = '<span><span class="mono"></span> <span></span></span>';
    li.querySelector('.mono').textContent = hex(addr);
    li.firstChild.lastChild.textContent = name;
    li.appendChild(statusEl(cls, status));
    list.appendChild(li);
  };
  for (const d of i2c.devices) add(d.addr, d.name, d.expected ? 'ok' : 'warn', d.expected ? 'OK' : 'unerwartet');
  for (const d of i2c.missing) add(d.addr, d.name, 'bad', 'fehlt');
  if (i2c.scanned && !i2c.devices.length && !i2c.missing.length) add(0, 'keine Geräte gefunden', 'bad', '–');
}

async function scanI2c() {
  const btn = $('i2c-scan');
  btn.disabled = true;
  try {
    const r = await api('/api/i2c/scan', {});
    renderI2c(r);
    toast(`I²C-Scan: ${r.devices.length} Gerät(e) gefunden` + (r.missing.length ? `, ${r.missing.length} fehlen` : ''));
  } catch (err) {
    toast(`I²C-Scan: ${err.message}`, true);
  } finally {
    btn.disabled = false;
  }
}

function renderBatterySettings(b) {
  $('bat-enabled').checked = b.enabled;
  $('bat-sleep').checked = b.deep_sleep;
  $('bat-sleep').disabled = !b.enabled;
  fillKv($('sys-bat'), [
    [b.enabled ? 'Spannung' : 'Messwert (ungeprüft)', `${b.v.toFixed(2)} V`],
    ['Ladestand', b.enabled ? `${b.pct} %` : '–'],
    ['Zustand', BAT_TEXT[b.state] || b.state],
  ]);
}

async function setBattery(patch) {
  try {
    const b = await api('/api/battery', patch);
    renderBatterySettings(b);
    toast(b.msg);
  } catch (err) {
    toast(err.message, true);
    loadStatus();
  }
}

// ------------------------------------------------------------------ LEDs ----

const LED_STATES = {
  ap: ['Access Point', 'Servos aus, eigenes WLAN'],
  idle: ['Servos aus', 'bereit, Servos stromlos'],
  torque: ['Servos an', 'sitzt, steht auf oder setzt sich'],
  stand: ['steht', 'steht still (auch „Lebendig“)'],
  walk: ['läuft', 'beim Laufen'],
  arms: ['Vorderbeine', 'Vorderbeine oben'],
  gesture: ['Geste', 'Kreisen, Nicken, Tanzen …'],
  boot: ['Start', 'bis das Netz steht'],
  wifi: ['WLAN', 'verbindet mit dem Heimnetz'],
  bat_warn: ['Akku schwach', 'unter 6,8 V'],
  bat_low: ['Akku leer', 'Servos abgeschaltet'],
  estop: ['Not-Aus', 'bis zur Freigabe'],
};
const LED_EFFECTS = {
  off: 'aus', solid: 'Dauerlicht', breathe: 'Atmen', blink: 'Blinken', pulse: 'Herzschlag',
  wink: 'Blinzeln', alternate: 'Wechselblinken', rainbow: 'Regenbogen',
  flicker: 'Flackern', strobe: 'Blitzen', scan: 'Hin und her', sleepy: 'Müde',
};
const L = { cfg: null, preview: null, previewUntil: 0, raf: 0, touched: 0, state: '' };

// Effekte wie in led_fx.cpp (für die Vorschau im Browser)
function ledHash(x) {
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d);
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b);
  x ^= x >>> 16;
  return x >>> 0;
}
const ledLid = (dt) => (dt < 0 || dt > 160 ? 1 : Math.abs(dt - 80) / 80);
const hexRgb = (h) => {
  const v = parseInt(h.slice(1), 16);
  return [v >> 16, (v >> 8) & 255, v & 255];
};
function hsvRgb(deg) {
  const h = (((deg % 360) + 360) % 360) / 60;
  const i = Math.floor(h);
  const f = h - i;
  const q = Math.round(255 * (1 - f));
  const t = Math.round(255 * f);
  return [[255, t, 0], [q, 255, 0], [0, 255, t], [0, q, 255], [t, 0, 255], [255, 0, q]][i % 6];
}
function ledRender(style, t) {
  t = Math.floor(t);
  let k = [1, 1];
  switch (style.effect) {
    case 'off': k = [0, 0]; break;
    case 'breathe': k = [0.12 + 0.88 * (0.5 - 0.5 * Math.cos((2 * Math.PI * (t % 3200)) / 3200))]; break;
    case 'blink': k = [t % 1000 < 500 ? 1 : 0]; break;
    case 'pulse': {
      const x = t % 1400;
      k = [0.12 + 0.88 * Math.max(Math.exp(-(((x - 100) / 60) ** 2)), 0.6 * Math.exp(-(((x - 350) / 60) ** 2)))];
      break;
    }
    case 'wink': {
      const h = ledHash(Math.floor(t / 4000) + 1);
      const dt = (t % 4000) - (500 + (h % 3000));
      let v = ledLid(dt);
      if (((h >>> 12) % 4) === 0) v = Math.min(v, ledLid(dt - 300));
      k = [v];
      break;
    }
    case 'alternate': {
      const v = t % 1000 < 500 ? 1 : 0;
      k = [v, 1 - v];
      break;
    }
    case 'rainbow': {
      const hh = ((t % 7000) * 360) / 7000;
      return [hsvRgb(hh), hsvRgb(hh + 40)];
    }
    case 'flicker': k = [0, 1].map((e) => 0.3 + (0.7 * (ledHash(Math.floor(t / 60) * 2 + e + 7) % 1000)) / 999); break;
    case 'strobe': k = [t % 160 < 60 ? 1 : 0]; break;
    case 'scan': {
      const x = 0.5 - 0.5 * Math.cos((2 * Math.PI * (t % 1600)) / 1600);
      k = [1 - 0.9 * x, 0.1 + 0.9 * x];
      break;
    }
    case 'sleepy': {
      const dt = (t % 4000) - 2500;
      k = [0.4 * (dt < 0 || dt > 1200 ? 1 : Math.abs(dt - 600) / 600)];
      break;
    }
    default: break;
  }
  if (k.length === 1) k.push(k[0]);
  const c = hexRgb(style.color);
  return k.map((v) => c.map((x) => Math.round(x * v)));
}

function ledStyleFor(cfg, id) {
  const fixed = cfg.fixed.find((s) => s.id === id);
  if (fixed) return fixed;
  if (cfg.mode === 'off') return { color: '#000000', effect: 'off' };
  if (cfg.mode === 'single') return cfg.single;
  return cfg.states.find((s) => s.id === id) || { color: '#000000', effect: 'off' };
}

function ledFrame(now) {
  if (S.tab !== 'leds' || !L.cfg) {
    L.raf = 0;
    return;
  }
  const id = (S.tel && S.tel.led) || L.cfg.state || 'idle';
  const fixed = L.cfg.fixed.some((s) => s.id === id);
  const preview = Date.now() < L.previewUntil && !fixed;
  const eyes = !fixed && S.tel && S.tel.eyes;  // Vorgabe aus "Verhalten" (Emotes, Augen)
  const style = preview ? L.preview : (eyes || ledStyleFor(L.cfg, id));
  const k = 0.3 + 0.7 * (L.cfg.brightness / 100);  // Helligkeit angedeutet
  ledRender(style, now).forEach((c, i) => {
    const [r, g, b] = c.map((v) => Math.round(v * k));
    const eye = $(`eye-${i}`);
    eye.style.backgroundColor = `rgb(${r},${g},${b})`;
    eye.style.boxShadow = r + g + b > 30 ? `0 0 18px 2px rgba(${r},${g},${b},0.55)` : 'none';
  });
  if (id !== L.state) {
    L.state = id;
    document.querySelectorAll('.led-row').forEach((row) => row.classList.toggle('active', row.dataset.state === id));
  }
  const name = LED_STATES[id] ? LED_STATES[id][0] : id;
  $('led-now').textContent = preview ? `Vorschau – ${LED_EFFECTS[style.effect] || style.effect}`
    : `Jetzt: ${name} – ${LED_EFFECTS[style.effect] || style.effect}`;
  L.raf = requestAnimationFrame(ledFrame);
}

const ledPostThrottled = throttle((patch) => ledPost(patch), 150);
async function ledPost(patch) {
  L.touched = Date.now();
  try {
    L.cfg = await api('/api/leds', patch);
    renderLeds(false);
  } catch (e) {
    toast(e.message, true);
  }
}

function ledEffectSelect(value, onChange) {
  const sel = document.createElement('select');
  for (const [v, label] of Object.entries(LED_EFFECTS)) {
    const o = document.createElement('option');
    o.value = v;
    o.textContent = label;
    sel.appendChild(o);
  }
  sel.value = value;
  sel.onchange = () => onChange(sel.value);
  return sel;
}

// Zeile: Punkt, Name, Farbe, Effekt; patchOf(style) baut den Befehl
function ledRow(list, id, style, patchOf) {
  const row = document.createElement('div');
  row.className = 'led-row';
  row.dataset.state = id;
  const dot = document.createElement('span');
  dot.className = 'led-dot';
  dot.style.background = style.effect === 'off' ? 'transparent' : style.color;
  const name = document.createElement('div');
  name.className = 'led-name';
  const [title, desc] = LED_STATES[id] || [id, ''];
  name.innerHTML = `<b></b><span class="muted small"></span>`;
  name.querySelector('b').textContent = title;
  name.querySelector('span').textContent = desc;
  row.append(dot, name);
  if (!patchOf) {
    const fx = document.createElement('span');
    fx.className = 'fixed-fx';
    fx.textContent = LED_EFFECTS[style.effect] || style.effect;
    row.append(fx);
  } else {
    const color = document.createElement('input');
    color.type = 'color';
    color.value = style.color;
    color.title = 'Farbe';
    const preview = (st) => {
      L.preview = st;
      L.previewUntil = Date.now() + 2500;
      dot.style.background = st.effect === 'off' ? 'transparent' : st.color;
    };
    color.oninput = () => {
      const st = { color: color.value, effect: sel.value };
      preview(st);
      ledPostThrottled({ ...patchOf({ color: color.value }), preview: true });
    };
    const sel = ledEffectSelect(style.effect, (v) => {
      preview({ color: color.value, effect: v });
      ledPost({ ...patchOf({ effect: v }), preview: true });
    });
    sel.title = 'Effekt';
    row.append(color, sel);
  }
  list.appendChild(row);
  return row;
}

function renderLeds(rebuild) {
  const c = L.cfg;
  if (!c) return;
  setSeg('led-mode', c.mode);
  $('led-states-card').hidden = c.mode !== 'auto';
  $('led-single-card').hidden = c.mode !== 'single';
  if (document.activeElement !== $('led-bright')) $('led-bright').value = c.brightness;
  $('led-bright-val').textContent = `${$('led-bright').value} %`;
  $('led-board').value = c.board;
  if (!rebuild) return;
  for (const id of ['led-states', 'led-single', 'led-fixed']) $(id).innerHTML = '';
  for (const st of c.states) ledRow($('led-states'), st.id, st, (p) => ({ state: st.id, ...p }));
  const single = ledRow($('led-single'), 'single', c.single, (p) => ({ single: p }));
  single.querySelector('b').textContent = 'Alle Zustände';
  single.querySelector('.muted').textContent = 'außer den festen Anzeigen';
  for (const st of c.fixed) ledRow($('led-fixed'), st.id, st, null);
  L.state = '';
}

async function loadLeds() {
  try {
    L.cfg = await api('/api/leds');
    renderLeds(true);
    if (!L.raf) L.raf = requestAnimationFrame(ledFrame);
  } catch (e) {
    toast('LED-Einstellungen nicht verfügbar', true);
  }
}

function bindLeds() {
  bindSeg('led-mode', (v) => ledPost({ mode: v }));
  $('led-bright').oninput = (e) => {
    $('led-bright-val').textContent = `${e.target.value} %`;
    if (L.cfg) L.cfg.brightness = +e.target.value;
    ledPostThrottled({ brightness: +e.target.value });
  };
  $('led-board').onchange = (e) => ledPost({ board: e.target.value });
  $('led-reset').onclick = async () => {
    if (!confirm('Alle LED-Einstellungen auf den Standard zurücksetzen?')) return;
    await ledPost({ reset: true });
    renderLeds(true);
    toast('LED-Standard wiederhergestellt');
  };
}

// ------------------------------------------------------------- Controller ----

// Reihenfolge wie pad.h (Bit = Index): Name, kurze Beschriftung, lange Beschriftung
const PAD_BUTTONS = [
  ['a', 'A', 'Taste A'], ['b', 'B', 'Taste B'], ['x', 'X', 'Taste X'], ['y', 'Y', 'Taste Y'],
  ['lb', 'LB', 'LB (Schulter links)'], ['rb', 'RB', 'RB (Schulter rechts)'],
  ['view', 'View', 'View (zwei Fenster)'], ['menu', 'Menü', 'Menü (drei Striche)'], ['xbox', 'Xbox', 'Xbox-Taste'],
  ['ls', 'LS', 'Linken Stick drücken'], ['rs', 'RS', 'Rechten Stick drücken'],
  ['up', '↑', 'Steuerkreuz ↑'], ['down', '↓', 'Steuerkreuz ↓'], ['left', '←', 'Steuerkreuz ←'], ['right', '→', 'Steuerkreuz →'],
  ['share', 'Share', 'Share (nur Series X|S)'], ['p1', 'P1', 'Paddle P1 (oben rechts)'], ['p2', 'P2', 'Paddle P2 (unten rechts)'],
  ['p3', 'P3', 'Paddle P3 (oben links)'], ['p4', 'P4', 'Paddle P4 (unten links)'],
];
const PAD_BIT = Object.fromEntries(PAD_BUTTONS.map(([id], i) => [id, i]));
const PAD_LONG = Object.fromEntries(PAD_BUTTONS.map(([id, , long]) => [id, long]));
const PAD_ACTIONS = {
  none: '– nichts –', stand_sit: 'Aufstehen / Hinsetzen', stop: 'Stopp', arms: 'Vorderbeine heben/absetzen',
  wave: 'Winken', mode_next: 'Modus wechseln', mode_walk: 'Modus Laufen', mode_body: 'Modus Körper',
  mode_arms: 'Modus Vorderbeine', gait_prev: 'Gangart zurück', gait_next: 'Gangart weiter', height_up: 'Körper höher',
  height_down: 'Körper tiefer', speed_up: 'Tempo +', speed_down: 'Tempo −', alive: 'Lebendig an/aus',
  leveling: 'Lageausgleich an/aus', gesture_next: 'Nächste Geste', circle: 'Geste Kreisen', nod: 'Geste Ja',
  shake: 'Geste Nein', pushup: 'Geste Liegestütz', dance: 'Geste Tanzen', torque: 'Servos an/aus', estop: 'Not-Aus',
  pose_reset: 'Haltung zurücksetzen',
};
// Auswahl im Dialog, nach Themen
const PAD_ACTION_GROUPS = [
  ['Bewegung', ['stand_sit', 'stop', 'arms', 'wave']],
  ['Modus', ['mode_next', 'mode_walk', 'mode_body', 'mode_arms']],
  ['Gang und Haltung', ['gait_prev', 'gait_next', 'speed_up', 'speed_down', 'height_up', 'height_down', 'pose_reset', 'leveling', 'alive']],
  ['Gesten', ['gesture_next', 'circle', 'nod', 'shake', 'pushup', 'dance']],
  ['System', ['torque', 'estop', 'none']],
];
const PAD_MODES = { walk: 'Laufen', body: 'Körper', arms: 'Vorderbeine' };
// Sticks und Trigger je Modus (fest, siehe pad_map.cpp)
const PAD_AXES = {
  walk: { ls: 'Fahren', rs: 'Drehen', lt: 'Langsamer', rt: 'Schneller (bis 100 %)' },
  body: { ls: 'Verschieben', rs: 'Neigen', lt: 'Nach links drehen', rt: 'Nach rechts drehen' },
  arms: { ls: 'Linkes Vorderbein', rs: 'Rechtes Vorderbein', lt: 'Beine heranziehen', rt: 'Beine ausstrecken' },
};
// Standard-Gamepad im Browser -> Bits wie pad.h (Index im buttons-Feld der Gamepad-API)
const GP_BUTTONS = [0, 1, 2, 3, 4, 5, 8, 9, 16, 10, 11, 12, 13, 14, 15, 17];
const PAD_SEND_MS = 50;
const PAD_LIVE_MS = 1200;  // so lange ohne neue Werte -> Anzeige zurücksetzen
const P = {
  cfg: null, web: false, timer: null, seq: null, mode: '', src: '', touched: 0, gpName: '',
  view: 'walk', liveAt: 0, picking: '',
};

function gamepad() {
  const list = navigator.getGamepads ? Array.from(navigator.getGamepads()).filter((g) => g && g.connected) : [];
  return list.find((g) => g.mapping === 'standard') || list[0] || null;
}

function gamepadState(g) {
  const pressed = (i) => !!(g.buttons[i] && g.buttons[i].pressed);
  const value = (i) => (g.buttons[i] ? g.buttons[i].value : 0);
  const axis = (i) => Math.round((g.axes[i] || 0) * 1000) / 1000;
  let b = 0;
  GP_BUTTONS.forEach((gi, bit) => { if (pressed(gi)) b |= 1 << bit; });
  return {
    lx: axis(0), ly: -axis(1) || 0, rx: axis(2), ry: -axis(3) || 0,
    lt: Math.round(value(6) * 1000) / 1000, rt: Math.round(value(7) * 1000) / 1000, b,
  };
}

// Gamepad am Handy/PC: Zustand alle 50 ms an den Roboter (der wertet ihn aus)
function padTick() {
  const g = P.web ? gamepad() : null;
  P.gpName = g ? g.id : '';
  renderPadWeb();
  if (!g) return;
  const st = gamepadState(g);
  sendQuiet('pad', st);
  if (S.tab === 'pad') renderPadLive(st, P.mode);
}

function setPadWeb(on) {
  P.web = on;
  try { localStorage.setItem('padWeb', on ? '1' : '0'); } catch (e) { /* egal */ }
  $('pad-web-on').checked = on;
  clearInterval(P.timer);
  P.timer = on ? setInterval(padTick, PAD_SEND_MS) : null;
  renderPadWeb();
}

function renderPadWeb() {
  let text = 'aus';
  if (P.web) {
    text = P.gpName ? `aktiv: ${P.gpName.replace(/\s*\(.*\)$/, '')}`
      : 'an – kein Controller erkannt (verbinden, dann eine Taste drücken)';
  }
  $('pad-web-status').textContent = text;
}

// --- Grafik: viewBox 0 0 1000 600, Controller in der Mitte, Beschriftungen links/rechts/oben/unten
const SVGNS = 'http://www.w3.org/2000/svg';
const PC_BODY = 'M375 175C430 160 570 160 625 175C680 180 712 205 726 255C744 330 760 410 752 455C745 498 705 516 676 '
  + '492C652 470 628 432 592 424L408 424C372 432 348 470 324 492C295 516 255 498 248 455C240 410 256 330 274 255C288 '
  + '205 320 180 375 175Z';
const PC_LS = [365, 262], PC_RS = [565, 345];
const PC_FACE = { y: [635, 228, '#e3b341'], x: [601, 262, '#4f8fe6'], b: [669, 262, '#e5534b'], a: [635, 296, '#3fb950'] };
// Beschriftungen: Seite, Höhe (Mitte), Ziel [x, y, Radius], Knickpunkte; kind: stick/trig = je Modus
const PC_CALLOUTS = [
  { id: 'lt', kind: 'trig', side: 'l', y: 72, to: [339, 136] },
  { id: 'lb', side: 'l', y: 122, to: [316, 171] },
  { id: 'ls', kind: 'stick', side: 'l', y: 262, to: [PC_LS[0], PC_LS[1], 36] },
  { id: 'up', side: 'l', y: 320, to: [424, 322] },
  { id: 'left', side: 'l', y: 362, to: [400, 345] },
  { id: 'right', side: 'l', y: 404, via: [[484, 404]], to: [472, 349] },
  { id: 'down', side: 'l', y: 446, via: [[435, 446]], to: [435, 380] },
  { id: 'p3', side: 'l', y: 494, to: [294, 441] },
  { id: 'p4', side: 'l', y: 540, to: [278, 479] },
  { id: 'rt', kind: 'trig', side: 'r', y: 72, to: [661, 136] },
  { id: 'rb', side: 'r', y: 122, to: [684, 171] },
  { id: 'y', side: 'r', y: 190, to: [...PC_FACE.y.slice(0, 2), 15] },
  { id: 'x', side: 'r', y: 236, via: [[655, 241]], to: [...PC_FACE.x.slice(0, 2), 15] },
  { id: 'b', side: 'r', y: 280, to: [...PC_FACE.b.slice(0, 2), 15] },
  { id: 'a', side: 'r', y: 324, to: [...PC_FACE.a.slice(0, 2), 15] },
  { id: 'rs', kind: 'stick', side: 'r', y: 392, to: [PC_RS[0], PC_RS[1], 36] },
  { id: 'p1', side: 'r', y: 486, to: [706, 441] },
  { id: 'p2', side: 'r', y: 532, to: [722, 479] },
  { id: 'view', side: 't', x: 335, via: [[335, 66], [446, 66]], to: [458, 254] },
  { id: 'xbox', side: 't', x: 500, to: [500, 214, 19] },
  { id: 'menu', side: 't', x: 665, via: [[665, 66], [554, 66]], to: [542, 254] },
  { id: 'share', side: 'b', x: 500, to: [500, 306] },
];
const PC_W = { l: 215, r: 215, t: 150, b: 200 };

function svgEl(tag, attrs, parent, text) {
  const el = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs || {})) el.setAttribute(k, v);
  if (text !== undefined) el.textContent = text;
  if (parent) parent.appendChild(el);
  return el;
}

// Zeichnung des Controllers (einmal); Tasten tragen data-btn, Sticks/Trigger eigene Klassen
function buildPadSvg() {
  const svg = svgEl('svg', { viewBox: '0 0 1000 600', class: 'pc-svg', role: 'img', 'aria-label': 'Xbox-Controller mit Belegung' });
  const defs = svgEl('defs', {}, svg);
  const leads = svgEl('g', { class: 'pc-leads' }, svg);
  const pad = svgEl('g', { class: 'pc-pad' }, svg);
  // Trigger (hinter dem Gehäuse) mit Füllstand
  for (const [id, x, rot] of [['lt', 335, -9], ['rt', 593, 9]]) {
    const g = svgEl('g', { class: 'pc-trig', 'data-axis': id, transform: `rotate(${rot} ${x + 36} 138)` }, pad);
    const clip = svgEl('clipPath', { id: `pc-clip-${id}` }, defs);
    svgEl('rect', { x, y: 118, width: 72, height: 40, rx: 12 }, clip);
    svgEl('rect', { x, y: 118, width: 72, height: 40, rx: 12, class: 'pc-trig-shape' }, g);
    svgEl('rect', { x, y: 158, width: 72, height: 0, class: 'pc-trig-fill', 'clip-path': `url(#pc-clip-${id})`, id: `pc-fill-${id}` }, g);
    svgEl('text', { x: x + 36, y: 142, class: 'pc-txt' }, g, id.toUpperCase());
  }
  // Schultertasten
  svgEl('rect', { x: 312, y: 160, width: 116, height: 18, rx: 9, transform: 'rotate(-9 370 169)', class: 'pc-btn', 'data-btn': 'lb' }, pad);
  svgEl('rect', { x: 572, y: 160, width: 116, height: 18, rx: 9, transform: 'rotate(9 630 169)', class: 'pc-btn', 'data-btn': 'rb' }, pad);
  svgEl('path', { d: PC_BODY, class: 'pc-body' }, pad);
  // Paddles (Rückseite): gestrichelt auf den Griffen
  for (const [id, x, y, rot] of [['p3', 292, 422, 18], ['p4', 276, 460, 18], ['p1', 694, 422, -18], ['p2', 710, 460, -18]]) {
    svgEl('rect', { x, y, width: 14, height: 36, rx: 7, transform: `rotate(${rot} ${x + 7} ${y + 18})`, class: 'pc-btn pc-paddle', 'data-btn': id }, pad);
  }
  // Sticks: Ring + beweglicher Kopf (Drücken = LS/RS)
  for (const [id, [cx, cy]] of [['ls', PC_LS], ['rs', PC_RS]]) {
    svgEl('circle', { cx, cy, r: 36, class: 'pc-ring' }, pad);
    const cap = svgEl('g', { class: 'pc-btn pc-cap', 'data-btn': id, id: `pc-cap-${id}` }, pad);
    svgEl('circle', { cx, cy, r: 24, class: 'pc-cap-out' }, cap);
    svgEl('circle', { cx, cy, r: 16, class: 'pc-cap-in' }, cap);
  }
  // Steuerkreuz
  const dp = svgEl('g', { class: 'pc-dpad' }, pad);
  svgEl('rect', { x: 424, y: 334, width: 22, height: 22, class: 'pc-dpad-c' }, dp);
  for (const [id, x, y, w, h, arrow] of [
    ['up', 424, 310, 22, 25, 'M435 316l-5 7h10z'], ['down', 424, 355, 22, 25, 'M435 374l-5 -7h10z'],
    ['left', 400, 334, 25, 22, 'M406 345l7 -5v10z'], ['right', 445, 334, 25, 22, 'M464 345l-7 -5v10z']]) {
    const g = svgEl('g', { class: 'pc-btn pc-dir', 'data-btn': id }, dp);
    svgEl('rect', { x, y, width: w, height: h, rx: 4 }, g);
    svgEl('path', { d: arrow, class: 'pc-glyph' }, g);
  }
  // A B X Y
  for (const [id, [cx, cy, color]] of Object.entries(PC_FACE)) {
    const g = svgEl('g', { class: 'pc-btn pc-face', 'data-btn': id }, pad);
    svgEl('circle', { cx, cy, r: 15 }, g);
    svgEl('text', { x: cx, y: cy + 5, class: 'pc-face-txt', fill: color }, g, id.toUpperCase());
  }
  // View, Menü, Xbox, Share
  let g = svgEl('g', { class: 'pc-btn pc-small', 'data-btn': 'view' }, pad);
  svgEl('rect', { x: 450, y: 254, width: 24, height: 16, rx: 5 }, g);
  svgEl('path', { d: 'M456 259h7v5h-7zM459 261v-4h7v5h-3', class: 'pc-glyph-line' }, g);
  g = svgEl('g', { class: 'pc-btn pc-small', 'data-btn': 'menu' }, pad);
  svgEl('rect', { x: 526, y: 254, width: 24, height: 16, rx: 5 }, g);
  svgEl('path', { d: 'M532 258.5h12M532 262h12M532 265.5h12', class: 'pc-glyph-line' }, g);
  g = svgEl('g', { class: 'pc-btn pc-xbox', 'data-btn': 'xbox' }, pad);
  svgEl('circle', { cx: 500, cy: 214, r: 19 }, g);
  svgEl('path', { d: 'M493 207l14 14M507 207l-14 14', class: 'pc-glyph-line pc-xbox-x' }, g);
  g = svgEl('g', { class: 'pc-btn pc-small', 'data-btn': 'share' }, pad);
  svgEl('rect', { x: 489, y: 294, width: 22, height: 13, rx: 5 }, g);
  svgEl('path', { d: 'M500 297.5v6.5M497 300.5l3 -3l3 3', class: 'pc-glyph-line' }, g);

  // Beschriftungen mit Linien
  const cos = svgEl('g', { class: 'pc-callouts' }, svg);
  for (const c of PC_CALLOUTS) {
    const h = c.kind === 'stick' ? 62 : 40;
    const w = PC_W[c.side];
    let bx, by, ax, ay;
    if (c.side === 'l' || c.side === 'r') {
      bx = c.side === 'l' ? 12 : 1000 - 12 - w;
      by = c.y - h / 2;
      ax = c.side === 'l' ? bx + w : bx;
      ay = c.y;
    } else {
      bx = c.x - w / 2;
      by = c.side === 't' ? 12 : 600 - 12 - h;
      ax = c.x;
      ay = c.side === 't' ? by + h : by;
    }
    // Linie: Anker -> Knickpunkte -> Ziel (bei Kreisen bis zum Rand)
    const pts = [[ax, ay], ...(c.via || [])];
    const [tx, ty, r] = c.to;
    let ex = tx, ey = ty;
    if (r) {
      const [px, py] = pts[pts.length - 1];
      const d = Math.hypot(px - tx, py - ty) || 1;
      ex = tx + ((px - tx) / d) * r;
      ey = ty + ((py - ty) / d) * r;
    }
    pts.push([ex, ey]);
    svgEl('polyline', { points: pts.map((q) => q.join(',')).join(' '), class: 'pc-lead', 'data-co': c.id }, leads);
    svgEl('circle', { cx: ex, cy: ey, r: 3, class: 'pc-lead-dot', 'data-co': c.id }, leads);

    const fixed = c.kind === 'trig';
    const box = svgEl('g', { class: `pc-co${fixed ? ' fixed' : ''}`, 'data-co': c.id }, cos);
    if (!fixed) {
      box.setAttribute('data-btn', c.id);
      box.setAttribute('tabindex', '0');
      box.setAttribute('role', 'button');
    }
    svgEl('rect', { x: bx, y: by, width: w, height: h, rx: 9, class: 'pc-co-box' }, box);
    const tx0 = bx + 12;
    svgEl('text', { x: tx0, y: by + 15, class: 'pc-co-title' }, box, '');
    svgEl('text', { x: tx0, y: by + 32, class: 'pc-co-fn' }, box, '');
    if (c.kind === 'stick') svgEl('text', { x: tx0, y: by + 51, class: 'pc-co-sub' }, box, '');
    box.dataset.maxw = w - 24;
  }
  return svg;
}

// Text in die Box einpassen (lange Funktionsnamen schmaler setzen)
function fitText(el, maxw) {
  el.removeAttribute('textLength');
  el.removeAttribute('lengthAdjust');
  let len = 0;
  try { len = el.getComputedTextLength(); } catch (e) { return; }
  if (len > maxw) {
    el.setAttribute('textLength', maxw);
    el.setAttribute('lengthAdjust', 'spacingAndGlyphs');
  }
}

// Belegung (Texte) in Grafik und Liste eintragen
function renderPadDiagram() {
  const c = P.cfg;
  const root = $('pad-diagram');
  if (!root.firstChild) {
    root.appendChild(buildPadSvg());
    root.addEventListener('click', (e) => {
      const t = e.target.closest('[data-btn]');
      if (t) openPadPick(t.dataset.btn);
    });
    root.addEventListener('keydown', (e) => {
      const t = e.target.closest('[data-btn]');
      if (t && (e.key === 'Enter' || e.key === ' ')) {
        e.preventDefault();
        openPadPick(t.dataset.btn);
      }
    });
    root.addEventListener('mouseover', (e) => {
      const t = e.target.closest('[data-co], [data-btn]');
      padHover(t ? t.dataset.co || t.dataset.btn : '');
    });
    root.addEventListener('mouseleave', () => padHover(''));
    if (window.ResizeObserver) new ResizeObserver(padLayout).observe(root);
  }
  padLayout();
  setSeg('pad-view-mode', P.view);
  const axes = { ...PAD_AXES[P.view] };
  if (c && c.swap_sticks) [axes.ls, axes.rs] = [axes.rs, axes.ls];
  const fn = (id) => PAD_ACTIONS[(c && c.map[id]) || 'none'] || '–';
  const title = {
    lt: 'LT (Trigger)', rt: 'RT (Trigger)', ls: 'Linker Stick', rs: 'Rechter Stick', up: 'Steuerkreuz ↑', down: 'Steuerkreuz ↓',
    left: 'Steuerkreuz ←', right: 'Steuerkreuz →', lb: 'LB', rb: 'RB', a: 'A', b: 'B', x: 'X', y: 'Y', view: 'View',
    menu: 'Menü', xbox: 'Xbox-Taste', share: 'Share', p1: 'P1 (Paddle hinten)', p2: 'P2 (Paddle hinten)',
    p3: 'P3 (Paddle hinten)', p4: 'P4 (Paddle hinten)',
  };
  root.querySelectorAll('.pc-co').forEach((box) => {
    const id = box.dataset.co;
    const [t, f, sub] = box.querySelectorAll('text');
    t.textContent = id === 'ls' || id === 'rs' || id === 'lt' || id === 'rt' ? `${title[id]} · ${PAD_MODES[P.view]}` : title[id];
    f.textContent = axes[id] || fn(id);
    if (sub) sub.textContent = `drücken: ${fn(id)}`;
    for (const el of [t, f, sub]) if (el) fitText(el, +box.dataset.maxw);
    box.classList.toggle('unset', !axes[id] && (!c || (c.map[id] || 'none') === 'none'));
  });
  // Liste (schmale Bildschirme)
  const list = $('pad-list');
  const order = ['a', 'b', 'x', 'y', 'lb', 'rb', 'up', 'down', 'left', 'right', 'view', 'menu', 'xbox', 'share', 'ls', 'rs', 'p1', 'p2', 'p3', 'p4'];
  const keyOf = Object.fromEntries(PAD_BUTTONS.map(([id, short]) => [id, short]));
  list.innerHTML = '';
  const head = (text) => {
    const h = document.createElement('div');
    h.className = 'pad-list-head';
    h.textContent = text;
    list.appendChild(h);
  };
  head(`Sticks und Trigger · ${PAD_MODES[P.view]}`);
  for (const id of ['ls', 'rs', 'lt', 'rt']) {
    const row = document.createElement('div');
    row.className = 'pad-row fixed';
    row.dataset.co = id;
    row.innerHTML = '<span class="pad-key"></span><span class="pad-fn"></span>';
    row.firstChild.textContent = { ls: 'L-Stick', rs: 'R-Stick', lt: 'LT', rt: 'RT' }[id];
    row.lastChild.textContent = axes[id];
    list.appendChild(row);
  }
  head('Tasten (antippen zum Ändern)');
  for (const id of order) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'pad-row';
    row.dataset.btn = id;
    row.dataset.co = id;
    row.innerHTML = '<span class="pad-key"></span><span class="pad-fn"></span><span class="pad-chev">›</span>';
    row.firstChild.textContent = keyOf[id];
    row.children[1].textContent = fn(id);
    row.title = PAD_LONG[id];
    row.onclick = () => openPadPick(id);
    list.appendChild(row);
  }
}

// Breite Ansicht: Controller mit Beschriftungen; schmal: nur Controller + Liste darunter
function padLayout() {
  const root = $('pad-diagram');
  const svg = root.firstChild;
  if (!svg || !root.clientWidth) return;
  const compact = root.clientWidth < 720;
  $('pad-diagram-card').classList.toggle('compact', compact);
  svg.setAttribute('viewBox', compact ? '232 92 536 432' : '0 0 1000 600');
}

// Live-Anzeige: gedrückte Tasten, bewegte Sticks und Trigger leuchten (Grafik und Liste)
function renderPadLive(i, mode) {
  const card = $('pad-diagram-card');
  if (!$('pad-diagram').firstChild) return;
  P.liveAt = Date.now();
  $('pad-live-tag').hidden = false;
  if (mode && mode !== P.liveMode) {  // Modus am Controller gewechselt -> Grafik folgt
    P.liveMode = mode;
    if (mode !== P.view) {
      P.view = mode;
      renderPadDiagram();
    }
  }
  const b = i.b || 0;
  const on = new Set(PAD_BUTTONS.filter(([, ,], bit) => b & (1 << bit)).map(([id]) => id));
  if (Math.hypot(i.lx, i.ly) > 0.2) on.add('ls');
  if (Math.hypot(i.rx, i.ry) > 0.2) on.add('rs');
  if (i.lt > 0.1) on.add('lt');
  if (i.rt > 0.1) on.add('rt');
  card.querySelectorAll('[data-btn]').forEach((el) => {
    const bit = PAD_BIT[el.dataset.btn];
    el.classList.toggle('on', bit !== undefined && !!(b & (1 << bit)) && !el.classList.contains('pc-co'));
  });
  card.querySelectorAll('.pc-lead, .pc-lead-dot, .pc-co, .pad-row').forEach((el) => {
    el.classList.toggle('on', on.has(el.dataset.co));
  });
  for (const [id, x, y] of [['ls', i.lx, i.ly], ['rs', i.rx, i.ry]]) {
    $(`pc-cap-${id}`).setAttribute('transform', `translate(${(x * 12).toFixed(1)} ${(-y * 12).toFixed(1)})`);
  }
  for (const id of ['lt', 'rt']) {
    const v = Math.max(0, Math.min(1, i[id] || 0));
    const f = $(`pc-fill-${id}`);
    f.setAttribute('y', 158 - 40 * v);
    f.setAttribute('height', 40 * v);
  }
}

// Zeigen auf Beschriftung, Linie oder Taste hebt alle drei hervor
function padHover(id) {
  $('pad-diagram').querySelectorAll('.hl').forEach((el) => el.classList.remove('hl'));
  if (!id) return;
  $('pad-diagram').querySelectorAll(`[data-co="${id}"], [data-btn="${id}"]`).forEach((el) => el.classList.add('hl'));
}

function clearPadLive() {
  if (!P.liveAt) return;
  P.liveAt = 0;
  $('pad-live-tag').hidden = true;
  renderPadLive({ lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0, b: 0 }, '');
  P.liveAt = 0;
  $('pad-live-tag').hidden = true;
}

// Auswahl-Dialog: Funktion für eine Taste
function openPadPick(id) {
  const c = P.cfg;
  if (!c || PAD_BIT[id] === undefined) return;
  P.picking = id;
  $('pad-pick-title').textContent = `${PAD_LONG[id]} belegen`;
  const list = $('pad-pick-list');
  list.innerHTML = '';
  const cur = c.map[id] || 'none';
  for (const [name, actions] of PAD_ACTION_GROUPS) {
    const h = document.createElement('div');
    h.className = 'pad-list-head';
    h.textContent = name;
    list.appendChild(h);
    const wrap = document.createElement('div');
    wrap.className = 'pad-pick-grid';
    for (const a of actions) {
      if (!c.actions.includes(a)) continue;
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'pad-opt' + (a === cur ? ' active' : '');
      btn.innerHTML = '<span></span><small></small>';
      btn.firstChild.textContent = PAD_ACTIONS[a] || a;
      const others = a === 'none' ? [] : PAD_BUTTONS.filter(([b]) => b !== id && c.map[b] === a).map(([, short]) => short);
      btn.lastChild.textContent = others.length ? `auch auf ${others.join(', ')}` : '';
      btn.onclick = async () => {
        closePadPick();
        if (a !== cur) {
          await padPost({ map: { [id]: a } });
          toast(`${PAD_LONG[id]}: ${PAD_ACTIONS[a] || a}`);
        }
      };
      wrap.appendChild(btn);
    }
    list.appendChild(wrap);
  }
  $('pad-pick').hidden = false;
  const active = list.querySelector('.pad-opt.active');
  (active || list.querySelector('.pad-opt')).focus();
}

function closePadPick() {
  $('pad-pick').hidden = true;
  const id = P.picking;
  P.picking = '';
  const back = id && $('pad-diagram').querySelector(`.pc-co[data-co="${id}"]`);
  if (back && !$('pad-diagram-card').classList.contains('compact')) back.focus();
}

const BLE_TEXT = {
  off: 'Bluetooth aus',
  idle: 'an – noch kein Controller gekoppelt',
  connecting: 'verbindet …',
};

// Telemetrie: Bluetooth-Status, Quelle, Modus, Meldungen
// Kopfzeile: Controller-Symbol, solange einer verbunden ist (Bluetooth bzw. Gamepad im Browser)
function renderPadPill(p) {
  const pill = $('pill-pad');
  const ble = p && p.ble === 'connected';
  const web = p && p.src === 'web';
  pill.hidden = !ble && !web;
  if (pill.hidden) return;
  const bat = ble && typeof p.bat === 'number' ? p.bat : null;
  setPill('pill-pad', bat !== null ? `Controller ${bat} %` : 'Controller', bat !== null && bat <= 15 ? 'pill-warn' : '',
    bat !== null ? `${bat} %` : '\u00a0');
  pill.title = ble ? `${p.name || 'Xbox-Controller'} (Bluetooth)${bat !== null ? ` · Akku ${bat} %` : ''}`
    : 'Gamepad im Browser';
}

function renderPad(p) {
  if (!p) return;
  if (P.seq === null) P.seq = p.seq || 0;  // alte Meldungen beim Laden nicht zeigen
  else if (p.seq && p.seq !== P.seq) {
    P.seq = p.seq;
    toast(`🎮 ${p.msg}`);
  }
  if (p.mode !== P.mode && P.mode && p.src === 'web') {
    const g = gamepad();  // Rückmeldung beim Moduswechsel
    if (g && g.vibrationActuator) {
      g.vibrationActuator.playEffect('dual-rumble', { duration: 120, strongMagnitude: 0.4, weakMagnitude: 0.4 }).catch(() => {});
    }
  }
  P.mode = p.mode;
  P.src = p.src;
  renderPadPill(p);
  const ind = $('pad-ind');
  ind.hidden = !p.src;
  ind.textContent = `🎮 ${PAD_MODES[p.mode] || ''}`;
  ind.title = `Controller (${p.src === 'ble' ? 'Bluetooth' : 'Browser'}), Tempo ${p.speed} %`;

  let text = BLE_TEXT[p.ble] || p.ble;
  if (p.ble === 'searching') text = `wartet auf ${p.name || 'den gekoppelten Controller'} – Controller einschalten`;
  if (p.ble === 'pairing') text = `Kopplung läuft (noch ${p.pair_left || 0} s) – Kopplungstaste am Controller halten, bis das Logo schnell blinkt`;
  if (p.ble === 'connected') text = `verbunden: ${p.name || 'Xbox-Controller'}${p.bat !== undefined ? ` · Akku ${p.bat} %` : ''}`;
  $('pad-ble-status').textContent = text;
  if (Date.now() - P.touched > LOCAL_PRIORITY_MS) $('pad-ble-on').checked = p.ble !== 'off';
  $('pad-pair').disabled = p.ble === 'pairing' || p.ble === 'connecting';
  $('pad-forget').disabled = !(P.cfg && P.cfg.addr) && p.ble !== 'connected';
  if (p.ble === 'connected' && P.cfg && !P.cfg.addr && S.tab === 'pad') loadPad();  // neue Kopplung

  if (S.tab === 'pad' && p.src === 'ble' && p.in) renderPadLive(p.in, p.mode);
  if (S.tab === 'pad' && !p.src && Date.now() - P.liveAt > PAD_LIVE_MS) clearPadLive();
}

const padPostThrottled = throttle((patch) => padPost(patch), 200);
async function padPost(patch) {
  P.touched = Date.now();
  try {
    P.cfg = await api('/api/pad', patch);
    renderPadCfg();
  } catch (e) {
    toast(e.message, true);
  }
}

function renderPadCfg() {
  const c = P.cfg;
  if (!c) return;
  const sl = (id, v, fmt) => {
    if (document.activeElement !== $(id)) $(id).value = v;
    $(`${id}-val`).textContent = fmt($(id).value);
  };
  sl('pad-speed', Math.round(c.speed * 100), (v) => `${v} %`);
  sl('pad-deadzone', Math.round(c.deadzone * 100), (v) => `${v} %`);
  sl('pad-expo', Math.round(c.expo * 100), (v) => `${v} %`);
  $('pad-invert').checked = c.invert_y;
  $('pad-swap').checked = !!c.swap_sticks;
  $('pad-forget').disabled = !c.addr;
  renderPadDiagram();
}

async function loadPad() {
  try {
    P.cfg = await api('/api/pad');
    if (P.mode) P.view = P.mode;
    renderPadCfg();
  } catch (e) {
    toast('Controller-Einstellungen nicht verfügbar', true);
  }
}

function bindPad() {
  $('pad-ble-on').onchange = (e) => padPost({ enabled: e.target.checked });
  $('pad-pair').onclick = () => padPost({ pair: true });
  $('pad-forget').onclick = () => {
    if (confirm('Gekoppelten Controller vergessen?')) padPost({ forget: true });
  };
  $('pad-web-on').onchange = (e) => setPadWeb(e.target.checked);
  const slider = (id, key) => {
    $(id).oninput = (e) => {
      $(`${id}-val`).textContent = `${e.target.value} %`;
      padPostThrottled({ [key]: e.target.value / 100 });
    };
  };
  slider('pad-speed', 'speed');
  slider('pad-deadzone', 'deadzone');
  slider('pad-expo', 'expo');
  $('pad-invert').onchange = (e) => padPost({ invert_y: e.target.checked });
  $('pad-swap').onchange = (e) => padPost({ swap_sticks: e.target.checked });
  $('pad-reset').onclick = async () => {
    if (!confirm('Standardbelegung und -werte wiederherstellen?')) return;
    await padPost({ reset: true });
    toast('Standardbelegung wiederhergestellt');
  };
  bindSeg('pad-view-mode', (v) => {
    P.view = v;
    renderPadDiagram();
  });
  document.body.appendChild($('pad-pick'));  // Overlay über der ganzen Seite
  $('pad-pick-close').onclick = closePadPick;
  $('pad-pick').addEventListener('click', (e) => { if (e.target === $('pad-pick')) closePadPick(); });
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape' || $('pad-pick').hidden) return;
    // Esc schließt nur die Auswahl – nicht zusätzlich Not-Aus (globale Taste)
    e.preventDefault();
    e.stopImmediatePropagation();
    closePadPick();
  });
  setInterval(() => { if (P.liveAt && Date.now() - P.liveAt > PAD_LIVE_MS) clearPadLive(); }, 500);
  window.addEventListener('gamepadconnected', () => { if (P.web) padTick(); });
  window.addEventListener('gamepaddisconnected', () => { if (P.web) padTick(); });
  let on = false;
  try { on = localStorage.getItem('padWeb') === '1'; } catch (e) { /* kein Speicher */ }
  setPadWeb(on);
}

// ------------------------------------------------------------------ WLAN ----

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function renderWifi(w) {
  if (!w || !w.sta) return;
  S.wifi = w;
  const sta = w.sta;
  let cur;
  if (sta.state === 'connected') {
    const q = sta.rssi >= -55 ? 'sehr gut' : sta.rssi >= -65 ? 'gut' : sta.rssi >= -75 ? 'mäßig' : 'schwach';
    cur = `Verbunden mit „${sta.ssid}“ – Signal ${signalBars(sta.rssi)} ${q} (${sta.rssi} dBm${sta.channel ? `, Kanal ${sta.channel}` : ''}), Adresse ${sta.ip}.`;
    if (sta.connect_ms) {
      cur += ` Verbunden nach ${(sta.connect_ms / 1000).toFixed(1)} s${sta.tries > 1 ? ` (${sta.tries} Anläufe)` : ''}.`;
    }
    if (w.ap.active) {
      cur += ' Access Point läuft noch' + (w.ap.off_in_s != null ? ` (aus in ${Math.max(0, w.ap.off_in_s)} s).` : '.');
    }
  } else if (sta.state === 'connecting') {
    cur = `Verbinde mit „${sta.ssid}“ …${sta.try > 1 ? ` (Anlauf ${sta.try})` : ''}`;
  } else {
    cur = `Access-Point-Modus („${w.ap.ssid}“, ${w.ap.ip}). `;
    cur += w.saved_ssid
      ? `Gespeichertes Netz: „${w.saved_ssid}“${sta.reason ? ` – ${sta.reason}` : ''}.${sta.retry_in ? ` Nächster Versuch in ${sta.retry_in} s.` : ''}`
      : 'Noch kein Heimnetz eingerichtet.';
  }
  $('wifi-current').textContent = cur;
  $('wifi-submit').textContent = w.ap.active ? 'Verbinden' : 'Speichern';
  $('wifi-hint').textContent = w.ap.active
    ? 'Der Hexapod verbindet sich sofort mit dem gewählten Netz und zeigt danach hier seine neue Adresse an. Der Access Point läuft so lange weiter.'
    : 'Im Heimnetz-Betrieb gelten neue Daten nach einem Neustart. Klappt die Verbindung dann nicht, startet wieder der Access Point „Hexapod-MK3“ (Passwort hexapod123, Adresse 192.168.4.1).';
  if (!$('wifi-ssid').value && w.saved_ssid) $('wifi-ssid').value = w.saved_ssid;
}

async function loadWifi() {
  try {
    renderWifi(await api('/api/wifi'));
  } catch (e) { /* ignorieren */ }
}

// ------------------------------------------------------- Signal messen --
// Empfangsstärke (RSSI aus der Telemetrie, 4 Hz) 10 s lang mitteln und mehrere Messungen
// vergleichen, z. B. mit/ohne externe Antenne oder verschiedene Einbauorte.
const RSSI_MEASURE_MS = 10000;
const RSSI = { run: null, list: [] };

function rssiLoad() {
  try { RSSI.list = JSON.parse(localStorage.getItem('rssiTests') || '[]').slice(-12); } catch (e) { RSSI.list = []; }
}

function rssiSave() {
  try { localStorage.setItem('rssiTests', JSON.stringify(RSSI.list)); } catch (e) { /* kein Speicher */ }
}

function rssiTelemetry(t) {
  const rssi = t.wifi && typeof t.wifi.rssi === 'number' ? t.wifi.rssi : null;
  $('rssi-now').textContent = rssi === null ? (t.wifi && t.wifi.mode === 'ap' ? 'nur im Heimnetz' : '–')
    : `${rssi} dBm ${signalBars(rssi)}`;
  $('rssi-fill').style.width = rssi === null ? '0' : `${clamp((rssi + 95) / 65, 0, 1) * 100}%`;
  const r = RSSI.run;
  if (!r) return;
  if (rssi !== null) r.samples.push(rssi);
  const left = r.until - Date.now();
  if (left > 0) $('rssi-start').textContent = `Messe … ${Math.ceil(left / 1000)} s`;
  else rssiFinish();
}

function rssiFinish() {
  const r = RSSI.run;
  if (!r) return;
  RSSI.run = null;
  clearTimeout(r.timer);
  $('rssi-start').textContent = '10 s messen';
  $('rssi-start').disabled = false;
  if (r.samples.length < 5) {
    toast('Kein Empfangswert – messen geht nur, wenn der Roboter im Heimnetz ist', true);
    return;
  }
  const avg = r.samples.reduce((a, b) => a + b, 0) / r.samples.length;
  RSSI.list.push({ label: r.label, at: Date.now(), avg: Math.round(avg * 10) / 10,
    min: Math.min(...r.samples), max: Math.max(...r.samples), n: r.samples.length });
  RSSI.list = RSSI.list.slice(-12);
  rssiSave();
  renderRssi();
}

function renderRssi() {
  const list = RSSI.list;
  $('rssi-wrap').hidden = $('rssi-foot').hidden = !list.length;
  const body = $('rssi-rows');
  body.innerHTML = '';
  const first = list.length ? list[0].avg : 0;
  const dec = (v) => v.toFixed(1).replace('.', ',');
  list.forEach((m, i) => {
    const tr = document.createElement('tr');
    const d = m.avg - first;
    const name = document.createElement('td');
    name.textContent = m.label;
    name.title = new Date(m.at).toLocaleString('de-DE');
    tr.appendChild(name);
    for (const [text, cls] of [[`${dec(m.avg)} dBm`, ''], [`${m.min} … ${m.max}`, ''],
      [i ? `${d > 0 ? '+' : d < 0 ? '−' : '±'}${dec(Math.abs(d))} dB` : '–', i && d >= 3 ? 'better' : i && d <= -3 ? 'worse' : '']]) {
      const td = document.createElement('td');
      td.textContent = text;
      if (cls) td.className = cls;
      tr.appendChild(td);
    }
    body.appendChild(tr);
  });
  // Einordnung der letzten Messung gegenüber der ersten
  let verdict = '';
  if (list.length > 1) {
    const d = list[list.length - 1].avg - first;
    verdict = Math.abs(d) < 3 ? 'Letzte Messung: kein merklicher Unterschied (unter 3 dB)'
      : d > 0 ? `Letzte Messung: ${dec(d)} dB besser${d >= 6 ? ' – deutlich (doppelte Reichweite ab ca. 6 dB)' : ''}`
        : `Letzte Messung: ${dec(-d)} dB schlechter`;
  }
  $('rssi-verdict').textContent = verdict;
}

function bindRssi() {
  rssiLoad();
  renderRssi();
  $('rssi-start').onclick = () => {
    if (!S.wsOpen) {
      toast('Keine Verbindung zum Hexapod', true);
      return;
    }
    const label = $('rssi-label').value.trim() || `Messung ${RSSI.list.length + 1}`;
    RSSI.run = { label, until: Date.now() + RSSI_MEASURE_MS, samples: [],
      timer: setTimeout(rssiFinish, RSSI_MEASURE_MS + 2000) };  // auch ohne Telemetrie beenden
    $('rssi-start').disabled = true;
    $('rssi-start').textContent = 'Messe …';
    $('rssi-label').value = '';
  };
  $('rssi-clear').onclick = () => {
    RSSI.list = [];
    rssiSave();
    renderRssi();
  };
}

function signalBars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return '▂▄▆█'.slice(0, n).padEnd(4, '·');
}

function renderNetworks(r) {
  const list = $('wifi-networks');
  list.innerHTML = '';
  if (r.error) toast(r.error, true);
  if (!r.networks.length) {
    const li = document.createElement('li');
    li.className = 'muted small';
    li.textContent = 'Keine Netze gefunden.';
    list.appendChild(li);
    return;
  }
  for (const n of r.networks) {
    const li = document.createElement('li');
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'net';
    b.innerHTML = '<span class="name"></span><span class="sig"></span><span></span>';
    b.querySelector('.name').textContent = n.ssid;
    b.querySelector('.sig').textContent = signalBars(n.rssi);
    b.querySelector('.sig').title = `${n.rssi} dBm, Kanal ${n.channel}`;
    b.lastChild.textContent = n.secure ? '🔒' : 'offen';
    b.onclick = () => {
      list.querySelectorAll('.net').forEach((x) => x.classList.remove('selected'));
      b.classList.add('selected');
      $('wifi-ssid').value = n.ssid;
      $('wifi-pass').value = '';
      if (n.secure) $('wifi-pass').focus();
    };
    li.appendChild(b);
    list.appendChild(li);
  }
}

async function scanWifi() {
  const btn = $('wifi-scan');
  btn.disabled = true;
  btn.textContent = 'Suche …';
  try {
    await api('/api/wifi/scan', {});
    let r = null;
    for (let i = 0; i < 25; ++i) {
      await sleep(600);
      try { r = await api('/api/wifi/scan'); } catch (e) { continue; }  // AP kann kurz stocken
      if (!r.running) break;
    }
    if (r && !r.running) renderNetworks(r);
    else toast('Netzsuche dauert zu lange – bitte noch einmal versuchen', true);
  } catch (err) {
    toast(err.message, true);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Netze suchen';
  }
}

function showWifiResult(cls, html) {
  const box = $('wifi-result');
  box.className = cls ? `notice notice-${cls}` : 'notice';
  box.innerHTML = html;
  box.hidden = false;
}

const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// Verbindungsversuch verfolgen. Beim Verbinden wechselt der ESP evtl. den Funkkanal,
// dann verliert dieses Gerät kurz den Access Point – Fehler beim Abfragen sind normal.
async function followConnect(ssid) {
  const name = escapeHtml(ssid);
  showWifiResult('', `Verbinde mit „${name}“ … Dieses Gerät kann dabei kurz die Verbindung zum Access Point verlieren.`);
  const start = Date.now();
  let seenConnecting = false;
  while (Date.now() - start < 60000) {
    await sleep(1500);
    let w;
    try { w = await api('/api/wifi'); } catch (e) { continue; }
    renderWifi(w);
    const st = w.sta.state;
    if (st === 'connecting') {
      seenConnecting = true;
    } else if (st === 'connected') {
      const url = `http://${w.sta.ip}/`;
      const off = w.ap.off_in_s != null ? ` in ca. ${Math.max(0, w.ap.off_in_s)} s` : '';
      showWifiResult('ok',
        `<b>Verbunden mit „${name}“!</b><br>Der Hexapod ist jetzt im Heimnetz erreichbar unter ` +
        `<a href="${url}">${url}</a> (oder <span class="mono">http://${escapeHtml(w.host)}</span>).` +
        `<ol><li>Dieses Gerät wieder mit „${name}“ verbinden.</li>` +
        `<li>Die Adresse öffnen und am besten als Lesezeichen speichern.</li></ol>` +
        `Der Access Point „${escapeHtml(w.ap.ssid)}“ schaltet sich${off} ab.`);
      return;
    } else if (st === 'failed' && (seenConnecting || Date.now() - start > 5000)) {
      showWifiResult('error', `<b>Verbindung mit „${name}“ fehlgeschlagen.</b><br>` +
        `${escapeHtml(w.sta.reason || '')}<br>Passwort prüfen und erneut versuchen.`);
      return;
    }
  }
  showWifiResult('error', 'Keine Rückmeldung vom Hexapod. Ist dieses Gerät noch mit „Hexapod-MK3“ verbunden? ' +
    'Dann die Seite neu laden – der Status steht oben in dieser Karte.');
}

async function reboot() {
  try {
    await api('/api/reboot', {});
    toast('Neustart … Seite lädt gleich neu');
    setTimeout(() => location.reload(), 8000);
  } catch (e) {
    toast(e.message, true);
  }
}

function bindSystem() {
  $('wifi-form').onsubmit = async (e) => {
    e.preventDefault();
    const ssid = $('wifi-ssid').value.trim();
    const apMode = !S.wifi || S.wifi.ap.active;
    try {
      const r = await api('/api/wifi', { ssid, pass: $('wifi-pass').value, connect: apMode });
      if (apMode) {
        await followConnect(ssid);
      } else {
        toast(r.msg);
        if (confirm('WLAN-Daten gespeichert. Jetzt neu starten?')) reboot();
      }
    } catch (err) {
      toast(err.message, true);
    }
  };
  $('wifi-scan').onclick = scanWifi;
  $('wifi-pass-toggle').onclick = () => {
    const p = $('wifi-pass');
    const show = p.type === 'password';
    p.type = show ? 'text' : 'password';
    $('wifi-pass-toggle').textContent = show ? 'Verbergen' : 'Anzeigen';
  };
  $('wifi-clear').onclick = async () => {
    if (!confirm('Gespeicherte WLAN-Daten löschen? Nach dem Neustart startet der Access Point.')) return;
    try {
      toast((await api('/api/wifi', { clear: true })).msg);
    } catch (err) {
      toast(err.message, true);
    }
  };
  $('bat-enabled').onchange = (e) => {
    if (e.target.checked && !confirm('Akkuüberwachung einschalten?\n\nNur sinnvoll mit bestücktem Spannungsteiler an GPIO 1 – sonst können Zufallswerte die Servos sperren.')) {
      e.target.checked = false;
      return;
    }
    setBattery({ enabled: e.target.checked });
  };
  $('bat-sleep').onchange = (e) => setBattery({ deep_sleep: e.target.checked });
  $('i2c-scan').onclick = scanI2c;
  $('sys-reboot').onclick = () => {
    if (confirm('ESP32 jetzt neu starten? Die Servos werden dabei stromlos.')) reboot();
  };
  $('cal-export').onclick = async () => {
    try {
      const data = await api('/api/calibration');
      delete data.dirty;
      const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `hexapod-kalibrierung-${new Date().toISOString().slice(0, 10)}.json`;
      a.click();
      URL.revokeObjectURL(a.href);
    } catch (err) {
      toast(err.message, true);
    }
  };
  $('cal-import').onchange = async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    try {
      const data = JSON.parse(await file.text());
      if (!confirm(`Kalibrierung aus „${file.name}“ importieren und speichern?`)) return;
      toast((await api('/api/calibration', data)).msg);
      send('cal_get');
    } catch (err) {
      toast(`Import fehlgeschlagen: ${err.message}`, true);
    }
  };
}

// ------------------------------------------------------------ 3D-Ansicht --
// Live-Modell aus den Gelenkwinkeln (Vorwärtskinematik wie kinematics.cpp legFk):
// Körper, Beine, Schatten und die Standfläche der Füße am Boden; der Boden wandert
// beim Laufen mit. Daten: {"type":"live"} mit 20 Hz, solange die Ansicht sichtbar ist
// (die Oberfläche erneuert {"cmd":"live"} alle 1,5 s), sonst aus der Telemetrie (4 Hz).

const D2R = Math.PI / 180;
// Blickwinkel: Richtung der Kamera um die Hochachse, Höhe über dem Boden (rad), Zoom
const V3_VIEWS = { '3d': [-2.1, 0.5, 1.05], top: [Math.PI, 1.45, 1.1], side: [-Math.PI / 2, 0.1, 1.3], front: [0, 0.14, 1.35] };
// Gebogenes Tibia-Bauteil (CAD-Umriss, docs/beingeometrie.py): vom Knie erst nach außen, dann
// Knick nach unten zur Fußspitze. Mittellinie als [Anteil entlang der Linie Knie -> Fußspitze,
// seitlich dazu in mm (+ = nach außen/oben)], dazu die Breite je Abschnitt in mm.
const V3_TIBIA_SHAPE = [[0.17, 14.5], [0.37, 21], [0.75, 7.7]];
const V3_LEG_WIDTH = [15, 11, 12, 10, 8, 6];  // Coxa, Femur, Tibia: Knie -> Knick -> Fuß
const V3_GRID = 50;        // mm zwischen den Bodenlinien
const V3_GRID_N = 8;       // Linien je Richtung
const V3 = {
  geo: {  // bis /api/kinematics da ist: Maße wie robot_geometry.h
    leg: { coxa: 41, femur: 43, tibia: 94 },
    mounts: [{ x: 73.5, y: 38.5, angle: 45 }, { x: 0, y: 57, angle: 90 }, { x: -73.5, y: 38.5, angle: 135 },
      { x: 73.5, y: -38.5, angle: -45 }, { x: 0, y: -57, angle: -90 }, { x: -73.5, y: -38.5, angle: -135 }],
  },
  tgt: null,     // zuletzt empfangen {j[18] Grad, b[6] Körperlage oder null, s, tq, ik, v[3]}
  cur: null,     // angezeigt (geglättet)
  src: '',       // 'live' | 'tel'
  liveAt: 0, liveN: 0, liveNAt: 0, liveHz: 0,
  subscribed: false,
  visible: false,
  view: '3d', az: V3_VIEWS['3d'][0], el: V3_VIEWS['3d'][1], zoom: 1,
  odo: { x: 0, y: 0, yaw: 0 },
  raf: 0, last: 0, dirty: true,
  colors: null, colorsAt: 0,
  drag: null,
  infoAt: 0,
};

function v3Live(m) {
  const now = performance.now();
  V3.liveAt = now;
  if (++V3.liveN >= 20 || now - V3.liveNAt > 3000) {
    if (V3.liveNAt) V3.liveHz = V3.liveN * 1000 / Math.max(1, now - V3.liveNAt);
    V3.liveN = 0;
    V3.liveNAt = now;
  }
  if (!Array.isArray(m.j) || m.j.length < NUM_SERVOS) return;
  v3Set({ j: m.j.map((v) => v / 10), b: m.ik && m.b ? m.b : null, s: m.s || 0, tq: !!m.tq, ik: !!m.ik,
    v: m.ik && m.v ? m.v : [0, 0, 0] }, 'live');
}

function v3Telemetry(t) {
  if (performance.now() - V3.liveAt < 1000 || !t.servo || !Array.isArray(t.servo.deg)) return;
  const mo = t.motion || {};
  const ik = mo.mode === 'pose';
  const p = mo.pose;
  const g = mo.gait || {};
  v3Set({ j: t.servo.deg.slice(), b: ik && p ? [p.x, p.y, p.z, p.roll, p.pitch, p.yaw] : null, s: g.swing || 0,
    tq: !!t.servo.torque, ik, v: ik && g.walking ? [g.vx || 0, g.vy || 0, g.wz || 0] : [0, 0, 0] }, 'tel');
}

function v3Set(d, src) {
  V3.tgt = d;
  V3.src = src;
  if (!V3.cur) V3.cur = { ...d, j: d.j.slice(), b: d.b ? d.b.slice() : null };
  V3.dirty = true;
}

// Live-Daten anfordern, solange die Ansicht sichtbar ist
function v3Tick() {
  const want = wsReady() && v3Shown();
  if (want) {
    S.ws.send('{"cmd":"live","on":true}');
    V3.subscribed = true;
  } else if (V3.subscribed && wsReady()) {
    S.ws.send('{"cmd":"live","on":false}');
    V3.subscribed = false;
  } else {
    V3.subscribed = false;
  }
  v3Loop();
}

// Ansicht zu sehen? (nicht hinter der Fahrer-Ansicht, die über allem liegt)
function v3Shown() {
  return S.tab === 'control' && !document.hidden && V3.visible && !FPV.open;
}

function v3Loop() {
  const run = v3Shown();
  if (run && !V3.raf) {
    V3.last = performance.now();
    V3.raf = requestAnimationFrame(v3Frame);
  }
}

function v3SetView(v) {
  const p = V3_VIEWS[v] || V3_VIEWS['3d'];
  V3.view = v;
  V3.az = p[0];
  V3.el = p[1];
  V3.zoom = p[2];
  V3.dirty = true;
  setSeg('model-view', v);
  try { localStorage.setItem('modelView', v); } catch (e) { /* kein Speicher */ }
}

function v3Colors() {
  const cs = getComputedStyle(document.documentElement);
  const g = (n) => cs.getPropertyValue(n).trim();
  const dark = cs.colorScheme ? cs.colorScheme.includes('dark') : true;
  return {
    text: g('--text'), muted: g('--muted'), accent: g('--accent'), soft: g('--accent-soft'), border: g('--border'),
    s2: g('--surface-2'), s3: g('--surface-3'), surface: g('--surface'), ok: g('--ok'), warn: g('--warn'),
    danger: g('--danger'), dark,
    shadow: dark ? 'rgba(0, 0, 0, 0.42)' : 'rgba(16, 24, 40, 0.14)',
    grid: dark ? '138, 153, 168' : '93, 107, 121',
  };
}

// Körper -> Welt (Lauf-System): R = Rz(yaw) * Ry(-pitch) * Rx(roll), dann verschieben
function v3PoseFn(b) {
  const cr = Math.cos(b[3] * D2R), sr = Math.sin(b[3] * D2R);
  const cp = Math.cos(-b[4] * D2R), sp = Math.sin(-b[4] * D2R);
  const cy = Math.cos(b[5] * D2R), sy = Math.sin(b[5] * D2R);
  return (p) => {
    const y1 = cr * p[1] - sr * p[2], z1 = sr * p[1] + cr * p[2];
    const x2 = cp * p[0] + sp * z1, z2 = -sp * p[0] + cp * z1;
    return [cy * x2 - sy * y1 + b[0], sy * x2 + cy * y1 + b[1], z2 + b[2]];
  };
}

// Punkte eines Beins im Körper-System: Coxa-Achse, Femur-Achse, Knie, Form der Tibia, Fußspitze
function v3LegPoints(leg, a) {
  const m = V3.geo.mounts[leg];
  const g = V3.geo.leg;
  const ma = m.angle * D2R;
  const c = (Math.sin(ma) > 0 ? -1 : 1) * a[0] * D2R + ma;
  const f = a[1] * D2R;
  const td = f + (a[2] + 90) * D2R - Math.PI;  // Richtung der Tibia gegen die Waagerechte
  const kr = g.coxa + g.femur * Math.cos(f), kz = g.femur * Math.sin(f);
  const tr = Math.cos(td), tz = Math.sin(td);  // entlang der Tibia-Linie; seitlich: (-tz, tr)
  const rz = [[0, 0], [g.coxa, 0], [kr, kz]];
  for (const [along, side] of V3_TIBIA_SHAPE) {
    rz.push([kr + along * g.tibia * tr - side * tz, kz + along * g.tibia * tz + side * tr]);
  }
  rz.push([kr + g.tibia * tr, kz + g.tibia * tz]);
  return rz.map(([r, z]) => [m.x + r * Math.cos(c), m.y + r * Math.sin(c), z]);
}

function v3Hull(pts) {
  const p = pts.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  if (p.length < 3) return p;
  const cross = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lo = [], hi = [];
  for (const q of p) {
    while (lo.length >= 2 && cross(lo[lo.length - 2], lo[lo.length - 1], q) <= 0) lo.pop();
    lo.push(q);
  }
  for (let i = p.length - 1; i >= 0; --i) {
    while (hi.length >= 2 && cross(hi[hi.length - 2], hi[hi.length - 1], p[i]) <= 0) hi.pop();
    hi.push(p[i]);
  }
  return lo.slice(0, -1).concat(hi.slice(0, -1));  // gegen den Uhrzeigersinn
}

function v3Frame(now) {
  V3.raf = 0;
  if (!v3Shown()) return;
  const dt = Math.min(0.1, Math.max(0, (now - V3.last) / 1000));
  V3.last = now;
  const t = V3.tgt, c = V3.cur;
  if (t && c) {
    // weich nachführen (Live: 20 Hz, Telemetrie: 4 Hz)
    const k = 1 - Math.exp(-dt / (V3.src === 'live' ? 0.06 : 0.16));
    let diff = 0;
    for (let i = 0; i < NUM_SERVOS; ++i) {
      const d = t.j[i] - c.j[i];
      c.j[i] += d * k;
      diff = Math.max(diff, Math.abs(d));
    }
    if (t.b && c.b) {
      for (let i = 0; i < 6; ++i) {
        const d = t.b[i] - c.b[i];
        c.b[i] += d * k;
        diff = Math.max(diff, Math.abs(d));
      }
    } else {
      c.b = t.b ? t.b.slice() : null;
    }
    c.s = t.s;
    c.tq = t.tq;
    c.ik = t.ik;
    // Boden mitführen (Lauf-System bewegt sich mit dem Roboter)
    const fresh = V3.src !== 'live' || now - V3.liveAt < 600;
    const v = fresh ? t.v : [0, 0, 0];
    if (v[0] || v[1] || v[2]) {
      V3.odo.yaw += v[2] * D2R * dt;
      const cy = Math.cos(V3.odo.yaw), sy = Math.sin(V3.odo.yaw);
      V3.odo.x += (cy * v[0] - sy * v[1]) * dt;
      V3.odo.y += (sy * v[0] + cy * v[1]) * dt;
      V3.dirty = true;
    }
    if (diff > 0.02) V3.dirty = true;
  }
  if (now - V3.infoAt > 500) v3Info(now);
  if (V3.dirty) {
    V3.dirty = false;
    v3Draw();
  }
  V3.raf = requestAnimationFrame(v3Frame);
}

function v3Draw() {
  const cv = $('model-canvas');
  const W = cv.width, H = cv.height;
  if (!W || !H) return;
  const ctx = cv.getContext('2d');
  const now = performance.now();
  if (!V3.colors || now - V3.colorsAt > 2000) {
    V3.colors = v3Colors();
    V3.colorsAt = now;
  }
  const col = V3.colors;
  ctx.clearRect(0, 0, W, H);
  const c = V3.cur;

  // Gelenke -> Punkte; ohne IK-Lage (Handbetrieb) steht der tiefste Fuß auf dem Boden
  let legs = null, toWorld = null, body = null;
  if (c) {
    const bl = [];
    for (let leg = 0; leg < 6; ++leg) bl.push(v3LegPoints(leg, c.j.slice(leg * 3, leg * 3 + 3)));
    body = c.b;
    if (!body) {
      let zmin = 0;
      for (const pts of bl) for (const q of pts) zmin = Math.min(zmin, q[2]);
      body = [0, 0, -zmin, 0, 0, 0];
    }
    toWorld = v3PoseFn(body);
    legs = bl.map((pts) => pts.map(toWorld));
  }
  const bodyZ = body ? body[2] : 52;

  // Kamera: kreist um die Körpermitte
  const tgt = [body ? body[0] : 0, body ? body[1] : 0, bodyZ * 0.45];
  const dist = 1400 / V3.zoom;  // weit weg: wenig perspektivische Verzerrung
  const ce = Math.cos(V3.el), se = Math.sin(V3.el);
  const C = [tgt[0] + dist * ce * Math.cos(V3.az), tgt[1] + dist * ce * Math.sin(V3.az), tgt[2] + dist * se];
  let f = [tgt[0] - C[0], tgt[1] - C[1], tgt[2] - C[2]];
  const fl = Math.hypot(...f);
  f = f.map((v) => v / fl);
  let r = [f[1], -f[0], 0];  // f x (0,0,1)
  const rl = Math.hypot(r[0], r[1]) || 1;
  r = r.map((v) => v / rl);
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const focal = Math.min(W * 2.25, H * 3.6);
  const cx = W / 2, cyS = H * 0.54;
  const P = (p) => {
    const d0 = p[0] - C[0], d1 = p[1] - C[1], d2 = p[2] - C[2];
    const z = Math.max(1, d0 * f[0] + d1 * f[1] + d2 * f[2]);
    return [cx + focal * (d0 * r[0] + d1 * r[1] + d2 * r[2]) / z, cyS - focal * (d0 * u[0] + d1 * u[1] + d2 * u[2]) / z, z];
  };
  const px = (mm, z) => Math.max(1, mm * focal / z);  // Breite in Pixeln in der Tiefe z

  // Boden: Raster, wandert beim Laufen (Boden-System -> Lauf-System)
  {
    const o = V3.odo, cy = Math.cos(-o.yaw), sy = Math.sin(-o.yaw);
    const g2w = (gx, gy) => {
      const dx = gx - o.x, dy = gy - o.y;
      return [cy * dx - sy * dy, sy * dx + cy * dy, 0];
    };
    const span = V3_GRID * V3_GRID_N;
    const gx0 = Math.round(o.x / V3_GRID) * V3_GRID, gy0 = Math.round(o.y / V3_GRID) * V3_GRID;
    const buckets = [[], [], [], [], []];
    const seg = (a, b) => {
      const m = [(a[0] + b[0]) / 2 - tgt[0], (a[1] + b[1]) / 2 - tgt[1]];
      const fade = 1 - Math.hypot(m[0], m[1]) / span;
      if (fade <= 0) return;
      buckets[Math.min(4, Math.floor(fade * 5))].push([P(a), P(b)]);
    };
    for (let i = -V3_GRID_N; i <= V3_GRID_N; ++i) {
      for (let k = -V3_GRID_N; k < V3_GRID_N; ++k) {
        seg(g2w(gx0 + i * V3_GRID, gy0 + k * V3_GRID), g2w(gx0 + i * V3_GRID, gy0 + (k + 1) * V3_GRID));
        seg(g2w(gx0 + k * V3_GRID, gy0 + i * V3_GRID), g2w(gx0 + (k + 1) * V3_GRID, gy0 + i * V3_GRID));
      }
    }
    ctx.lineWidth = Math.max(1, W / 900);
    buckets.forEach((list, i) => {
      if (!list.length) return;
      ctx.strokeStyle = `rgba(${col.grid}, ${(0.07 + 0.07 * i).toFixed(2)})`;
      ctx.beginPath();
      for (const [a, b] of list) {
        ctx.moveTo(a[0], a[1]);
        ctx.lineTo(b[0], b[1]);
      }
      ctx.stroke();
    });
  }
  if (!legs) return;

  // Standfläche: konvexe Hülle der Füße am Boden, Körpermitte darüber
  const swing = (leg) => (c.s >> leg) & 1;
  const planted = [];
  for (let leg = 0; leg < 6; ++leg) {
    const ft = legs[leg][legs[leg].length - 1];
    if (!swing(leg) && ft[2] < 4) planted.push([ft[0], ft[1]]);
  }
  const hull = v3Hull(planted);
  let margin = null;
  const com = [body[0], body[1]];
  if (hull.length >= 3) {
    margin = Infinity;
    for (let i = 0; i < hull.length; ++i) {
      const a = hull[i], b = hull[(i + 1) % hull.length];
      const ex = b[0] - a[0], ey = b[1] - a[1];
      margin = Math.min(margin, (ex * (com[1] - a[1]) - ey * (com[0] - a[0])) / (Math.hypot(ex, ey) || 1));
    }
    ctx.beginPath();
    hull.forEach((q, i) => {
      const s = P([q[0], q[1], 0]);
      if (i) ctx.lineTo(s[0], s[1]);
      else ctx.moveTo(s[0], s[1]);
    });
    ctx.closePath();
    ctx.fillStyle = col.soft;
    ctx.fill();
    ctx.setLineDash([Math.max(3, W / 160), Math.max(3, W / 200)]);
    ctx.strokeStyle = col.accent;
    ctx.lineWidth = Math.max(1, W / 600);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  V3.margin = margin;
  V3.planted = planted.length;

  // Schatten (senkrecht auf den Boden)
  const ring = [0, 1, 2, 5, 4, 3];  // Beine im Kreis um den Körper
  const plate = (z, grow) => ring.map((leg) => {
    const m = V3.geo.mounts[leg];
    return toWorld([m.x * grow, m.y * grow, z]);
  });
  ctx.fillStyle = col.shadow;
  ctx.strokeStyle = col.shadow;
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  ctx.beginPath();
  plate(0, 1.12).forEach((q, i) => {
    const s = P([q[0], q[1], 0]);
    if (i) ctx.lineTo(s[0], s[1]);
    else ctx.moveTo(s[0], s[1]);
  });
  ctx.closePath();
  ctx.fill();
  for (const pts of legs) {
    const sh = pts.map((q) => P([q[0], q[1], 0]));
    ctx.lineWidth = px(9, sh[2][2]);
    ctx.beginPath();
    sh.forEach((s, i) => (i ? ctx.lineTo(s[0], s[1]) : ctx.moveTo(s[0], s[1])));
    ctx.stroke();
  }
  // Höhe der Schwungfüße: gestrichelt zum Boden
  ctx.setLineDash([Math.max(2, W / 300), Math.max(2, W / 300)]);
  ctx.lineWidth = Math.max(1, W / 700);
  ctx.strokeStyle = col.accent;
  for (let leg = 0; leg < 6; ++leg) {
    const ft = legs[leg][legs[leg].length - 1];
    if (ft[2] < 3) continue;
    const a = P(ft), b = P([ft[0], ft[1], 0]);
    ctx.beginPath();
    ctx.moveTo(a[0], a[1]);
    ctx.lineTo(b[0], b[1]);
    ctx.stroke();
  }
  ctx.setLineDash([]);

  // Körper und Beine, von hinten nach vorne gezeichnet
  const items = [];
  const top = plate(9, 1.12), bot = plate(-7, 1.12);
  const camDir = (p) => [C[0] - p[0], C[1] - p[1], C[2] - p[2]];
  const face = (pts, shade) => {
    // nur Flächen, die zur Kamera zeigen (Körper ist konvex)
    const a = pts[0], b = pts[1], d = pts[2];
    const e1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], e2 = [d[0] - a[0], d[1] - a[1], d[2] - a[2]];
    const n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    const cen = [0, 1, 2].map((k) => pts.reduce((s, p) => s + p[k], 0) / pts.length);
    const v = camDir(cen);
    if (n[0] * v[0] + n[1] * v[1] + n[2] * v[2] <= 0) return;
    const nl = Math.hypot(...n) || 1;
    const light = Math.max(0, (n[0] * 0.3 - n[1] * 0.35 + n[2] * 0.89) / nl);
    const sp = pts.map(P);
    items.push({ z: sp.reduce((s, q) => s + q[2], 0) / sp.length, draw() {
      ctx.beginPath();
      sp.forEach((q, i) => (i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1])));
      ctx.closePath();
      ctx.fillStyle = shade;
      ctx.fill();
      ctx.fillStyle = `rgba(0, 0, 0, ${((1 - light) * (col.dark ? 0.45 : 0.22)).toFixed(3)})`;
      ctx.fill();
      ctx.strokeStyle = col.border;
      ctx.lineWidth = Math.max(1, W / 700);
      ctx.stroke();
    } });
  };
  face(top, col.s3);
  face(bot.slice().reverse(), col.s3);
  for (let i = 0; i < 6; ++i) {
    const j = (i + 1) % 6;
    face([bot[i], bot[j], top[j], top[i]], col.s2);
  }
  // Blickrichtung: Pfeil auf der Oberseite und zwei "Augen" vorne
  {
    const pts = [[78, 0, 9.5], [52, 13, 9.5], [52, -13, 9.5]].map(toWorld);
    const sp = pts.map(P);
    items.push({ z: Math.min(...sp.map((q) => q[2])) - 0.5, draw() {
      if (!V3.topVisible) return;
      ctx.beginPath();
      sp.forEach((q, i) => (i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1])));
      ctx.closePath();
      ctx.fillStyle = col.accent;
      ctx.fill();
    } });
    for (const y of [-15, 15]) {
      const e = P(toWorld([84, y, 1]));
      items.push({ z: e[2] - 1, draw() {
        ctx.beginPath();
        ctx.arc(e[0], e[1], px(4.5, e[2]), 0, 2 * Math.PI);
        ctx.fillStyle = col.accent;
        ctx.fill();
      } });
    }
  }
  {  // Oberseite sichtbar?
    const n = toWorld([0, 0, 1]), o = toWorld([0, 0, 0]);
    const v = camDir(o);
    V3.topVisible = (n[0] - o[0]) * v[0] + (n[1] - o[1]) * v[1] + (n[2] - o[2]) * v[2] > 0;
  }
  for (let leg = 0; leg < 6; ++leg) {
    const sp = legs[leg].map(P);
    const color = !c.tq ? col.muted : swing(leg) ? col.accent : col.text;
    const last = sp.length - 2;
    for (let k = 0; k <= last; ++k) {
      const a = sp[k], b = sp[k + 1];
      items.push({ z: (a[2] + b[2]) / 2, draw() {
        ctx.strokeStyle = color;
        ctx.lineWidth = px(V3_LEG_WIDTH[k], (a[2] + b[2]) / 2);
        ctx.beginPath();
        ctx.moveTo(a[0], a[1]);
        ctx.lineTo(b[0], b[1]);
        ctx.stroke();
        if (k < 2 || k === last) {  // Femur-Achse, Knie, Fußspitze
          ctx.beginPath();
          ctx.arc(b[0], b[1], px(k === last ? 4 : 5.5, b[2]), 0, 2 * Math.PI);
          ctx.fillStyle = k === last ? color : col.surface;
          ctx.fill();
          ctx.lineWidth = Math.max(1, W / 600);
          ctx.stroke();
        }
      } });
    }
  }
  items.sort((a, b) => b.z - a.z);
  ctx.lineCap = 'round';
  for (const it of items) it.draw();

  // Lot der Körpermitte auf den Boden: grün = sicher in der Standfläche
  const m = P([com[0], com[1], 0]);
  const top0 = P(toWorld([0, 0, -7]));
  ctx.setLineDash([Math.max(2, W / 300), Math.max(2, W / 300)]);
  ctx.strokeStyle = col.muted;
  ctx.lineWidth = Math.max(1, W / 700);
  ctx.beginPath();
  ctx.moveTo(top0[0], top0[1]);
  ctx.lineTo(m[0], m[1]);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.beginPath();
  ctx.arc(m[0], m[1], px(5, m[2]), 0, 2 * Math.PI);
  ctx.fillStyle = margin === null ? col.muted : margin > 10 ? col.ok : margin > 0 ? col.warn : col.danger;
  ctx.fill();
  ctx.lineWidth = Math.max(1, W / 500);
  ctx.strokeStyle = col.surface;
  ctx.stroke();
}

function v3Info(now) {
  V3.infoAt = now;
  const el = $('model-info');
  const badge = $('model-src');
  const c = V3.cur;
  const live = now - V3.liveAt < 1000;
  badge.hidden = !c;
  badge.textContent = live ? `live · ${Math.round(V3.liveHz || 20)} Hz` : '4 Hz';
  badge.className = `badge ${live ? 'badge-on' : 'badge-muted'}`;
  if (!S.wsOpen) {
    el.textContent = 'Keine Verbindung';
    return;
  }
  if (!c) {
    el.textContent = 'Warte auf Daten …';
    return;
  }
  const parts = [];
  if (!c.tq) parts.push('Servos aus – Sollstellung');
  else if (!c.ik) parts.push('Handbetrieb (Kalibrierung)');
  const air = [];
  for (let leg = 0; leg < 6; ++leg) if ((c.s >> leg) & 1) air.push(leg + 1);
  if (c.ik && c.tq) {
    parts.push(air.length ? `In der Luft: ${air.length === 1 ? 'Bein' : 'Beine'} ${air.join(', ')}` : 'Alle Füße am Boden');
  }
  if (V3.margin !== null && V3.margin !== undefined && c.tq) {
    parts.push(V3.margin > 0 ? `Standreserve ${Math.round(V3.margin)} mm` : 'Schwerpunkt außerhalb der Standfläche!');
  }
  if (c.b) parts.push(`Höhe ${Math.round(c.b[2])} mm`);
  el.textContent = parts.join(' · ');
}

function bindModel() {
  const cv = $('model-canvas');
  const wrap = $('model-wrap');
  const fit = () => {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.round(wrap.clientWidth * dpr), h = Math.round(wrap.clientHeight * dpr);
    if (w && h && (cv.width !== w || cv.height !== h)) {
      cv.width = w;
      cv.height = h;
      V3.dirty = true;
    }
  };
  if (window.ResizeObserver) new ResizeObserver(fit).observe(wrap);
  else window.addEventListener('resize', fit);
  fit();
  if (window.IntersectionObserver) {
    new IntersectionObserver((entries) => {
      V3.visible = entries.some((e) => e.isIntersecting);
      v3Tick();
    }).observe($('model-card'));
  } else {
    V3.visible = true;
  }
  bindSeg('model-view', v3SetView);
  let v = '3d';
  try { v = localStorage.getItem('modelView') || '3d'; } catch (e) { /* kein Speicher */ }
  v3SetView(V3_VIEWS[v] ? v : '3d');

  // Ziehen dreht (Finger nur waagerecht – senkrecht scrollt die Seite), Strg + Rad zoomt
  cv.addEventListener('pointerdown', (e) => {
    V3.drag = { x: e.clientX, y: e.clientY, mouse: e.pointerType === 'mouse' };
    cv.setPointerCapture(e.pointerId);
  });
  cv.addEventListener('pointermove', (e) => {
    const d = V3.drag;
    if (!d) return;
    V3.az -= (e.clientX - d.x) * 0.008;
    if (d.mouse) V3.el = clamp(V3.el + (e.clientY - d.y) * 0.008, -0.05, 1.48);
    d.x = e.clientX;
    d.y = e.clientY;
    setSeg('model-view', '');
    V3.dirty = true;
  });
  const end = () => { V3.drag = null; };
  cv.addEventListener('pointerup', end);
  cv.addEventListener('pointercancel', end);
  cv.addEventListener('wheel', (e) => {
    if (!e.ctrlKey && !e.metaKey) return;
    e.preventDefault();
    V3.zoom = clamp(V3.zoom * Math.exp(-e.deltaY * 0.002), 0.5, 3);
    V3.dirty = true;
  }, { passive: false });
  cv.addEventListener('dblclick', () => v3SetView('3d'));
  document.addEventListener('visibilitychange', v3Tick);
  setInterval(v3Tick, 1500);
}

// ---------------------------------------------------------------- Start -----

function init() {
  bindNav();
  bindTheme();
  buildLegs();
  buildManual();
  bindPose();
  bindWalk();
  bindMove();
  bindLeds();
  bindPad();
  bindLog();
  bindOta();
  bindFpv();
  renderProfiles();
  bindGlobal();
  bindEditor();
  bindCamera();
  bindSystem();
  bindImu();
  bindModel();
  bindRssi();
  bindFun();
  bindLife();
  selectServo(0);
  showCalView('cal');
  showTab(pageFromHash());
  renderConnection();
  loadCamera();
  loadKinematics();
  loadWifi();
  connect();
}

init();
