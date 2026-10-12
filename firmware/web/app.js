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
  if (m.cmd === 'pose') {  // kommt beim Ziehen im 60-ms-Takt
    if (m.msg) showPoseNote(m.msg);
    return;
  }
  if (m.ok === false) toast(m.msg || 'Fehler', true);
  else if (m.msg) toast(m.msg);
  if (['cal_save', 'cal_reload', 'cal_reset'].includes(m.cmd)) send('cal_get');
}

// --------------------------------------------------------------- Kopfzeile --

function setPill(id, text, cls = '') {
  const pill = $(id);
  pill.querySelector('.ptext').textContent = text;
  pill.className = 'pill ' + cls;
}

function renderConnection() {
  setPill('pill-conn', S.wsOpen ? 'Verbunden' : 'Getrennt', S.wsOpen ? 'pill-ok' : 'pill-bad');
  $('sb-conn').classList.toggle('on', S.wsOpen);
  $('sb-conn-text').textContent = S.wsOpen ? `Verbunden · ${location.hostname}` : 'Getrennt';
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

  // Akku
  if (t.bat.state === 'off') setPill('pill-bat', 'Akku aus', 'pill-idle');
  else if (t.bat.state === 'usb') setPill('pill-bat', 'USB', 'pill-idle');
  else {
    setPill('pill-bat', `${t.bat.v.toFixed(2)} V · ${t.bat.pct}%`,
      { ok: 'pill-ok', warn: 'pill-warn', critical: 'pill-bad' }[t.bat.state] || '');
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
    el.hidden = !text;
    el.textContent = text;
    el.className = `small imu-cal ${cls}`;
  }
  const busy = imu.cal === 'running';
  for (const id of ['imu-level', 'imu-level2', 'imu-gyro']) $(id).disabled = busy || !imu.ok;

  // Einbaulage (nicht überschreiben, während ausgewählt wird)
  if (imu.mount && document.activeElement !== $('imu-xdir')) $('imu-xdir').value = imu.mount;
  if (imu.flip !== undefined && document.activeElement !== $('imu-flip')) $('imu-flip').value = imu.flip ? '1' : '0';
  if (S.tab === 'settings' && imu.level) {
    const [r, p] = imu.level;
    fillKv($('imu-info'), [
      ['Nullpunkt', r || p ? `Sensor um Roll ${fmtDeg(r)}°, Nick ${fmtDeg(p)}° geneigt` : 'nicht gesetzt'],
      ['Gyro-Nullpunkt', imu.bias_src || '–'],
    ]);
  }
}

function bindImu() {
  const level = () => send('imu_cal', { level: true });
  $('imu-level').onclick = level;
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
  if (!S.wsOpen) return false;
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
};

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
  // Vorderbeine: nach rechts = Fuß vor, nach oben = Fuß hoch
  const l = [lx, -ly];
  const r = MOVE.mirror ? l : [rx, -ry];
  return ['arm', { lx: l[0], lz: l[1], rx: r[0], rz: r[1] }];
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
  if (MOVE.sent) sendQuiet(MOVE.sent, MOVE.sent === 'body' ? { x: 0, y: 0, roll: 0, pitch: 0, yaw: 0 } : { lx: 0, lz: 0, rx: 0, rz: 0 });
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
  if (!MOVE_LABELS[mode]) mode = 'walk';
  if (W.timer || W.keys.size) walkStopAll();
  moveStopAll();
  document.querySelectorAll('.joy').forEach((j) => {
    j.classList.remove('active');
    j.querySelector('.joy-knob').style.transform = '';
  });
  MOVE.mode = mode;
  setSeg('move-mode', mode);
  $('joy-turn').classList.toggle('joy-h', mode === 'walk');
  $('joy-move-label').textContent = MOVE_LABELS[mode][0];
  $('joy-turn-label').textContent = MOVE_LABELS[mode][1];
  $('joy-move').setAttribute('aria-label', MOVE_LABELS[mode][0]);
  $('joy-turn').setAttribute('aria-label', MOVE_LABELS[mode][1]);
  $('walk-settings').hidden = mode !== 'walk';
  $('body-settings').hidden = mode !== 'body';
  $('arms-settings').hidden = mode !== 'arms';
  $('arms-buttons').hidden = mode !== 'arms';
  $('posture-buttons').hidden = mode === 'arms';
  try { localStorage.setItem('moveMode', mode); } catch (e) { /* egal */ }
  if (S.tel) renderWalk(S.tel.motion);
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
  $('arms-mirror').onchange = (e) => {
    MOVE.mirror = e.target.checked;
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
  const tag = (document.activeElement && document.activeElement.tagName) || '';
  if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return false;
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

function renderWalk(m) {
  if (!m) return;
  const g = m.gait || {};
  const st = $('walk-state');
  const posture = m.mode === 'pose' ? m.posture : null;
  const busy = posture === 'standing_up' || posture === 'sitting_down';
  const POSTURE_TEXT = { sitting: 'sitzt', standing_up: 'steht auf …', sitting_down: 'setzt sich …' };
  const arms = m.mode === 'pose' && m.arms ? m.arms : 'down';
  if (m.mode !== 'pose') {
    st.textContent = 'IK aus';
    st.className = 'badge badge-muted';
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
  $('body-info').textContent = m.offset_limited ? 'An der Reichweitengrenze – Körper lässt sich nicht weiter bewegen' : '';

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
    if (g.active && g.active !== g.type) info.push('Gangart wechselt nach dem Anhalten');
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

// ------------------------------------------------- Globale Bedienelemente ---

function bindGlobal() {
  const estop = () => {
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

// -------------------------------------------------------- Seiten & Menü -----

// Seite -> Adresse (#steuerung …), damit Neuladen und Zurück-Taste funktionieren
const PAGES = { control: 'steuerung', cal: 'kalibrierung', system: 'system', settings: 'einstellungen' };
const pageFromHash = () => Object.keys(PAGES).find((k) => location.hash === `#${PAGES[k]}`) || 'control';

function showTab(name) {
  if (!PAGES[name]) name = 'control';
  if (name !== 'control' && (W.timer || W.keys.size)) walkStopAll();
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
  clearInterval(S.statusTimer);
  renderBanner();
  if (name === 'system' || name === 'settings') {
    loadStatus();
    S.statusTimer = setInterval(loadStatus, 5000);
  }
  if (name === 'system') renderTiles();
  if (name === 'cal') renderManualValues();
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

function renderCamStats(t) {
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
  $('cam-gear').onclick = () => {
    const open = $('cam-settings').hidden;
    $('cam-settings').hidden = !open;
    $('cam-gear').setAttribute('aria-expanded', open);
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
    setTile('wifi', `${rssi} dBm`, statusEl(cls, text + (w && w.sta.ssid ? ` · ${w.sta.ssid}` : '')));
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

async function loadStatus() {
  try {
    const s = await api('/api/status');
    S.status = s;
    if (S.tab === 'system') renderTiles();
    const w = s.wifi;
    fillKv($('sys-info'), [
      ['Firmware', `${s.fw} (${s.build})`],
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
  bindNav();
  bindTheme();
  buildLegs();
  buildManual();
  bindPose();
  bindWalk();
  bindMove();
  bindGlobal();
  bindEditor();
  bindCamera();
  bindSystem();
  bindImu();
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
