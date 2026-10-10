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

function usToDeg(id, us) {
  const c = S.cal[id];
  return c ? (us - c.center_us) / c.us_per_deg : 0;
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
    renderLegs();
    renderEditorCal();
  }
  if (m.cal) {
    S.cal[m.cal.id] = m.cal;
    renderLegs();
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
  } else {
    $('imu-pitch').textContent = 'kein Sensor';
    $('imu-roll').textContent = '–';
  }

  // Kalibrierung
  $('cal-dirty').hidden = !sv.cal_dirty;
  $('cal-save').classList.toggle('attention', sv.cal_dirty);

  renderLegValues();
  renderEditorLive();
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

// ------------------------------------------------------------- Editor -------

function currentUs() {
  if (S.localUs !== null && Date.now() - S.localTime < LOCAL_PRIORITY_MS) return S.localUs;
  if (S.tel) return S.tel.servo.us[S.sel];
  const c = S.cal[S.sel];
  return c ? c.center_us : 1500;
}

function selectServo(id) {
  S.sel = (id + NUM_SERVOS) % NUM_SERVOS;
  S.localUs = null;
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
  S.localUs = c ? c.center_us + deg * c.us_per_deg : null;
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
  $('ed-zero').onclick = () => send('cal_zero', { id: S.sel });
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
  $('ed-go-zero').onclick = () => setTestDeg(0);
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
  $('ctl-neutral').onclick = () => send('all_deg', { deg: 0 });
  $('ctl-slew').oninput = (e) => {
    $('ctl-slew-val').textContent = `${e.target.value}°/s`;
  };
  $('ctl-slew').onchange = (e) => send('slew', { deg_per_s: +e.target.value });

  $('cal-all-1500').onclick = () => send('all_us', { us: 1500 });
  $('cal-all-neutral').onclick = () => send('all_deg', { deg: 0 });
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
  if (name === 'system') {
    loadStatus();
    S.statusTimer = setInterval(loadStatus, 5000);
  }
}

// -------------------------------------------------------------- Kamera ------

const camBase = () => `${location.protocol}//${location.hostname}:81`;

function updateStream() {
  const img = $('cam-img');
  const live = S.camWanted && S.tab === 'control' && !document.hidden;
  const view = img.parentElement;
  if (live && !view.classList.contains('live')) {
    img.src = `${camBase()}/stream?t=${Date.now()}`;
    view.classList.add('live');
  } else if (!live && view.classList.contains('live')) {
    img.src = 'data:,';  // Verbindung schließen
    view.classList.remove('live');
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
  $('cam-img').onerror = () => {
    if ($('cam-img').parentElement.classList.contains('live')) {
      toast('Stream unterbrochen', true);
      S.camWanted = false;
      updateStream();
    }
  };
  $('cam-fs').onchange = (e) => setCamera({ framesize: +e.target.value });
  $('cam-q').oninput = (e) => { $('cam-q-val').textContent = e.target.value; };
  $('cam-q').onchange = (e) => setCamera({ quality: +e.target.value });
  $('cam-fps').oninput = (e) => { $('cam-fps-val').textContent = `${e.target.value} fps`; };
  $('cam-fps').onchange = (e) => setCamera({ max_fps: +e.target.value });
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
      ['WLAN', w.mode === 'ap' ? `Access Point „${w.ssid}“ (${w.clients} Geräte)` : `${w.ssid} (${w.rssi} dBm)`],
      ['Adresse', `${w.ip} · ${w.host}`],
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
    renderBatterySettings(s.bat);
    $('wifi-current').textContent = w.mode === 'ap'
      ? 'Aktuell im Access-Point-Modus.'
      : `Verbunden mit „${w.ssid}“.`;
  } catch (e) {
    toast(`Status: ${e.message}`, true);
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

async function loadWifi() {
  try {
    const w = await api('/api/wifi');
    if (!$('wifi-ssid').value) $('wifi-ssid').value = w.saved_ssid || '';
  } catch (e) { /* ignorieren */ }
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
    try {
      const r = await api('/api/wifi', { ssid: $('wifi-ssid').value.trim(), pass: $('wifi-pass').value });
      toast(r.msg);
      if (confirm('WLAN-Daten gespeichert. Jetzt neu starten?')) reboot();
    } catch (err) {
      toast(err.message, true);
    }
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
  bindGlobal();
  bindEditor();
  bindCamera();
  bindSystem();
  selectServo(0);
  renderConnection();
  loadCamera();
  loadWifi();
  connect();
}

init();
