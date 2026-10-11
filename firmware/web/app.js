'use strict';
// Hexapod MK3 – Weboberfläche (ohne externe Abhängigkeiten, läuft auch im AP-Modus offline)

const LEGS = ['vorne links', 'Mitte links', 'hinten links', 'vorne rechts', 'Mitte rechts', 'hinten rechts'];
const JOINTS = ['Coxa', 'Femur', 'Tibia'];
const NUM_SERVOS = 18;
const DEFAULT_SLOPE = 2000 / 180;
const LOCAL_PRIORITY_MS = 900;  // so lange hat eine lokale Eingabe Vorrang vor der Telemetrie

const $ = (id) => document.getElementById(id);

const S = {
  ws: null,
  wsOpen: false,
  lastMsg: 0,
  tel: null,
  cal: new Array(NUM_SERVOS).fill(null),
  sel: 0,
  localUs: null,
  localTime: 0,
  tab: 'control',
  camWanted: false,
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

// ------------------------------------------------------------- WebSocket ----

function connect() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const ws = new WebSocket(proto + location.host + '/ws');
  S.ws = ws;
  ws.onopen = () => {
    S.wsOpen = true;
    S.lastMsg = Date.now();
    renderConnection();
    send('cal_get');
  };
  ws.onclose = () => {
    S.wsOpen = false;
    renderConnection();
    setTimeout(connect, 1500);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    S.lastMsg = Date.now();
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    onMessage(m);
  };
}

function send(cmd, params = {}) {
  if (!S.wsOpen) {
    toast('Keine Verbindung zum Hexapod', true);
    return false;
  }
  S.ws.send(JSON.stringify({ cmd, ...params }));
  return true;
}

setInterval(() => {
  if (!S.wsOpen) return;
  S.ws.send('{"cmd":"ping"}');
  if (Date.now() - S.lastMsg > 5000) S.ws.close();  // Verbindung hängt -> neu aufbauen
}, 1500);

function onMessage(m) {
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
  if (m.ok === false) toast(m.msg || 'Fehler', true);
  else if (m.msg) toast(m.msg);
  if (['cal_save', 'cal_reload', 'cal_reset'].includes(m.cmd)) send('cal_get');
}

// --------------------------------------------------------------- Kopfzeile --

function renderConnection() {
  const pill = $('pill-conn');
  pill.textContent = S.wsOpen ? 'Verbunden' : 'Getrennt';
  pill.className = 'pill ' + (S.wsOpen ? 'pill-ok' : 'pill-bad');
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

  if (!S.wsOpen) {
    msg = 'Keine Verbindung zum Hexapod – verbinde neu …';
    cls = 'info';
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
  } else if (t && t.wifi && t.wifi.mode === 'ap' && S.tab !== 'system') {
    msg = 'Access-Point-Modus – noch nicht mit dem Heimnetz verbunden';
    cls = 'info';
    act = { label: 'WLAN einrichten', fn: () => showTab('system') };
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

  // Akku
  const bat = $('pill-bat');
  if (t.bat.state === 'off') {
    bat.textContent = 'Akku: aus';
    bat.className = 'pill';
  } else if (t.bat.state === 'usb') {
    bat.textContent = 'USB';
    bat.className = 'pill';
  } else {
    bat.textContent = `${t.bat.v.toFixed(2)} V · ${t.bat.pct}%`;
    bat.className = 'pill ' + ({ ok: 'pill-ok', warn: 'pill-warn', critical: 'pill-bad' }[t.bat.state] || '');
  }

  // Servos
  const tp = $('pill-torque');
  if (sv.estop) {
    tp.textContent = 'NOT-AUS';
    tp.className = 'pill pill-bad';
  } else if (sv.torque) {
    tp.textContent = sv.active < NUM_SERVOS ? `Servos ${sv.active}/${NUM_SERVOS}` : 'Servos an';
    tp.className = 'pill pill-on';
  } else {
    tp.textContent = 'Servos aus';
    tp.className = 'pill';
  }
  $('ctl-torque').checked = sv.torque;
  $('cal-torque').checked = sv.torque;

  const slew = $('ctl-slew');
  if (document.activeElement !== slew) {
    slew.value = sv.slew;
    $('ctl-slew-val').textContent = `${Math.round(sv.slew)}°/s`;
  }

  // IMU
  if (t.imu.ok) {
    $('imu-pitch').textContent = fmtDeg(t.imu.pitch) + '°';
    $('imu-roll').textContent = fmtDeg(t.imu.roll) + '°';
    $('horizon-line').style.transform =
      `translateY(${clamp(t.imu.pitch, -45, 45) * 0.9}px) rotate(${-t.imu.roll}deg)`;
    // Rohwerte zur Kontrolle: flach liegend ist z ≈ +1 g, gekippt wandert 1 g auf x/y
    if (t.imu.acc) {
      const a = t.imu.acc.map((v) => (v >= 0 ? '+' : '') + v.toFixed(2)).join(' ');
      $('imu-raw').textContent = `a = ${a} g · ${t.imu.hz} Hz · ${t.imu.temp} °C · Lesefehler ${t.imu.errors}`;
    }
  } else {
    $('imu-pitch').textContent = 'kein Sensor';
    $('imu-roll').textContent = '–';
    $('imu-raw').textContent = t.imu.errors ? `Lesefehler ${t.imu.errors}` : '';
  }
  renderCamStats(t);

  // Kalibrierung
  $('cal-dirty').hidden = !sv.cal_dirty;
  $('cal-save').classList.toggle('attention', sv.cal_dirty);

  renderLegValues();
  renderEditorLive();
  renderManualValues();
  renderPose(t.motion);
  renderBanner();
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
const poseSend = throttle((patch) => { send('pose', patch); posePending = {}; }, 60);
let posePending = {};

function buildPose(kinem) {
  const box = $('pose-sliders');
  box.innerHTML = '';
  $('pose-placeholder').hidden = !kinem.placeholder;
  for (const [key, label, unit, range] of POSE_FIELDS) {
    const [lo, hi] = range(kinem.limits);
    const row = document.createElement('label');
    row.className = 'slider-row';
    row.innerHTML = `<span>${label}</span><input type="range" id="pose-${key}" step="1">` +
      `<span class="mono" id="pose-${key}-val"></span>`;
    const input = row.querySelector('input');
    input.min = Math.floor(lo);
    input.max = Math.ceil(hi);
    input.oninput = () => {
      poseTouched[key] = Date.now();
      $(`pose-${key}-val`).textContent = `${input.value} ${unit}`;
      posePending[key] = +input.value;
      poseSend(posePending);
    };
    box.appendChild(row);
  }
}

function renderPose(m) {
  if (!m) return;
  $('pose-mode').checked = m.mode === 'pose';
  for (const [key, , unit] of POSE_FIELDS) {
    const input = $(`pose-${key}`);
    if (!input || Date.now() - (poseTouched[key] || 0) < LOCAL_PRIORITY_MS) continue;
    if (document.activeElement === input) continue;
    input.value = Math.round(m.pose[key]);
    $(`pose-${key}-val`).textContent = `${Math.round(m.pose[key])} ${unit}`;
  }
  const warn = [];
  if (m.unreachable && m.unreachable.length) warn.push(`nicht erreichbar: Bein ${m.unreachable.map((l) => l + 1).join(', ')}`);
  if (m.clamped && m.clamped.length) warn.push(`an Gelenkgrenze: Bein ${m.clamped.map((l) => l + 1).join(', ')}`);
  $('pose-warn').hidden = !warn.length;
  $('pose-warn').textContent = warn.join(' · ');
}

async function loadKinematics() {
  try {
    buildPose(await api('/api/kinematics'));
    if (S.tel) renderPose(S.tel.motion);
  } catch (e) { /* Firmware ohne IK */ }
}

function bindPose() {
  $('pose-mode').onchange = (e) => {
    const on = e.target.checked;
    if (!send('mode', { mode: on ? 'pose' : 'manual' })) e.target.checked = !on;
    else if (on) torqueHint();
  };
  $('pose-reset').onclick = () => {
    for (const k of Object.keys(poseTouched)) delete poseTouched[k];
    posePending = {};
    send('pose_reset');
  };
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
  if (!S.tel || !$('manual').open) return;
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

// ------------------------------------------------- Globale Bedienelemente ---

function bindGlobal() {
  const estop = () => {
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
    if (e.key === 'Escape') {
      estop();
      e.preventDefault();
      return;
    }
    if (S.tab !== 'cal') return;
    const tag = (document.activeElement && document.activeElement.tagName) || '';
    if (tag === 'INPUT' && document.activeElement.type !== 'range') return;
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

// ---------------------------------------------------------------- Tabs ------

function showTab(name) {
  S.tab = name;
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.tab-page').forEach((p) => p.classList.toggle('active', p.id === `tab-${name}`));
  updateStream();
  clearInterval(S.statusTimer);
  renderBanner();
  if (name === 'system') {
    loadStatus();
    S.statusTimer = setInterval(loadStatus, 5000);
  }
}

// -------------------------------------------------------------- Kamera ------

const camBase = () => `${location.protocol}//${location.hostname}:81`;
let camRetryTimer = null;
let camRetries = 0;

function renderCamStats(t) {
  const el = $('cam-stats');
  const c = t.cam || {};
  const live = $('cam-img').parentElement.classList.contains('live');
  if (!live || !c.clients) {
    // Ohne Stream schläft die Kamera nach kurzer Zeit (spart Strom und Wärme)
    el.textContent = c.awake === undefined ? '' : (c.awake ? 'Kamera aktiv – schläft gleich ohne Stream' : 'Kamera schläft – wacht mit dem Stream auf');
    return;
  }
  if (c.fps !== undefined) camRetries = 0;  // Stream läuft wieder
  const parts = [`${(c.fps || 0).toFixed(1)} fps`, `${c.kb || 0} KB/Bild`, `${((c.kbps || 0) / 1000).toFixed(1)} Mbit/s`];
  if (c.wake_ms) parts.push(`Start ${c.wake_ms} ms`);
  if (t.wifi && t.wifi.rssi !== undefined) parts.push(`WLAN ${t.wifi.rssi} dBm`);
  if (c.err) parts.push(`Kamera-Aussetzer ${c.err}`);
  el.textContent = parts.join(' · ');
}

function scheduleCamRetry() {
  clearTimeout(camRetryTimer);
  camRetries += 1;
  $('cam-placeholder').textContent = `Stream unterbrochen – verbinde neu (Versuch ${camRetries}) …`;
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
    clearTimeout(camRetryTimer);
    camRetryTimer = null;
    camRetries = 0;
    $('cam-placeholder').textContent = 'Stream gestoppt';
    if (view.classList.contains('live')) {
      img.src = 'data:,';  // Verbindung schließen
      view.classList.remove('live');
    }
  }
  $('cam-toggle').textContent = S.camWanted ? 'Stream stoppen' : 'Stream starten';
}

function renderCamera() {
  const c = S.camera;
  if (!c) return;
  const sel = $('cam-fs');
  if (!sel.options.length) {
    for (const f of c.framesizes || []) sel.add(new Option(f.name, f.id));
  }
  sel.value = c.framesize;
  $('cam-q').value = c.quality;
  $('cam-q-val').textContent = c.quality;
  $('cam-fps').value = c.max_fps;
  $('cam-fps-val').textContent = `${c.max_fps} fps`;
  $('cam-rot').value = c.rotation || 0;
  $('cam-xclk').value = c.xclk_mhz || 20;
  $('cam-xclk-note').hidden = !c.xclk_active_mhz || c.xclk_mhz === c.xclk_active_mhz;
  // 90°/270°: Sensor liefert 0°/180°, die Oberfläche dreht um weitere 90°
  $('cam-img').parentElement.classList.toggle('rot90', c.rotation === 90 || c.rotation === 270);
  $('cam-vflip').checked = c.vflip;
  $('cam-hmirror').checked = c.hmirror;
  $('cam-info').textContent = c.ok ? (c.sensor || '') : 'Keine Kamera erkannt';
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

async function setCamera(patch) {
  try {
    S.camera = await api('/api/camera', patch);
    renderCamera();
  } catch (e) {
    toast(e.message, true);
  }
}

function bindCamera() {
  $('cam-snap').href = `${camBase()}/capture`;
  $('cam-toggle').onclick = () => {
    S.camWanted = !S.camWanted;
    updateStream();
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
  $('cam-q').oninput = (e) => { $('cam-q-val').textContent = e.target.value; };
  $('cam-q').onchange = (e) => setCamera({ quality: +e.target.value });
  $('cam-fps').oninput = (e) => { $('cam-fps-val').textContent = `${e.target.value} fps`; };
  $('cam-fps').onchange = (e) => setCamera({ max_fps: +e.target.value });
  $('cam-rot').onchange = (e) => setCamera({ rotation: +e.target.value });
  $('cam-vflip').onchange = (e) => setCamera({ vflip: e.target.checked });
  $('cam-hmirror').onchange = (e) => setCamera({ hmirror: e.target.checked });
  document.addEventListener('visibilitychange', updateStream);
}

// -------------------------------------------------------------- System ------

function fmtBytes(b) {
  if (b >= 1048576) return `${(b / 1048576).toFixed(1)} MB`;
  return `${Math.round(b / 1024)} KB`;
}

function fmtUptime(s) {
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

async function loadStatus() {
  try {
    const s = await api('/api/status');
    const w = s.wifi;
    fillKv($('sys-info'), [
      ['Firmware', `${s.fw} (${s.build})`],
      ['Chip', `${s.chip.model} rev ${s.chip.rev}, ${s.chip.cores} Kerne, ${s.chip.mhz} MHz`],
      ['Laufzeit', fmtUptime(s.uptime)],
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
    const names = { pca1: 'PCA9685 #1 (0x40, links)', pca2: 'PCA9685 #2 (0x41, rechts)', imu: 'MPU6050 (0x68)', camera: `Kamera${s.camera.sensor ? ' ' + s.camera.sensor : ''}` };
    for (const [key, label] of Object.entries(names)) {
      const li = document.createElement('li');
      const ok = s.devices[key];
      li.innerHTML = `<span></span><span class="${ok ? 'ok' : 'bad'}">${ok ? 'OK' : 'FEHLT'}</span>`;
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
    li.innerHTML = '<span><span class="mono"></span> <span></span></span><span></span>';
    li.querySelector('.mono').textContent = hex(addr);
    li.firstChild.lastChild.textContent = name;
    li.lastChild.className = cls;
    li.lastChild.textContent = status;
    list.appendChild(li);
  };
  for (const d of i2c.devices) add(d.addr, d.name, d.expected ? 'ok' : 'warn', d.expected ? 'OK' : 'unerwartet');
  for (const d of i2c.missing) add(d.addr, d.name, 'bad', 'FEHLT');
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
  const stateText = {
    off: 'Überwachung aus', usb: 'kein Akku (USB)', ok: 'OK', warn: 'schwach', critical: 'leer!',
  };
  fillKv($('sys-bat'), [
    [b.enabled ? 'Spannung' : 'Messwert (ungeprüft)', `${b.v.toFixed(2)} V`],
    ['Ladestand', b.enabled ? `${b.pct} %` : '–'],
    ['Zustand', stateText[b.state] || b.state],
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

// ------------------------------------------------------------------ WLAN ----

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function renderWifi(w) {
  if (!w || !w.sta) return;
  S.wifi = w;
  const sta = w.sta;
  let cur;
  if (sta.state === 'connected') {
    cur = `Verbunden mit „${sta.ssid}“ (${sta.rssi} dBm), Adresse ${sta.ip}.`;
    if (w.ap.active) {
      cur += ' Access Point läuft noch' + (w.ap.off_in_s != null ? ` (aus in ${Math.max(0, w.ap.off_in_s)} s).` : '.');
    }
  } else if (sta.state === 'connecting') {
    cur = `Verbinde mit „${sta.ssid}“ …`;
  } else {
    cur = `Access-Point-Modus („${w.ap.ssid}“, ${w.ap.ip}). `;
    cur += w.saved_ssid
      ? `Gespeichertes Netz: „${w.saved_ssid}“${sta.reason ? ` – ${sta.reason}` : ''}.`
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

// ---------------------------------------------------------------- Start -----

function init() {
  document.querySelectorAll('.tab').forEach((t) => { t.onclick = () => showTab(t.dataset.tab); });
  buildLegs();
  buildManual();
  bindPose();
  bindGlobal();
  bindEditor();
  bindCamera();
  bindSystem();
  selectServo(0);
  renderConnection();
  loadCamera();
  loadKinematics();
  loadWifi();
  connect();
}

init();
