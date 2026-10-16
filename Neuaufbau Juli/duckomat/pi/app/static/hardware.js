// ============================================================
// Duckomat Hardware UI Controller (hardware.js)
// ============================================================

const fastSlider = document.getElementById('fastSlider');
const slowSlider = document.getElementById('slowSlider');
const fastValLabel = document.getElementById('fastval');
const slowValLabel = document.getElementById('slowval');
const fastInputField = document.getElementById('fastInput');
const slowInputField = document.getElementById('slowInput');

const servoDirectUsInput = document.getElementById('servoDirectUs');
const servoDirectAngleCalc = document.getElementById('servoDirectAngleCalc');

let cfgLoadedOnce = false;
let devModeOn = false;
let motorDebounceTimer = null;
let servoTypeTimer = null;
let ignoreStatusPwmUntil = 0;

let base45 = 2122;
let base90 = 1600;
let base135 = 1050;
let degKickL = 105.0;
let degKickR = 75.0;

function angleToUs(deg) {
  if (deg <= 45.0) return base45;
  if (deg >= 135.0) return base135;
  if (deg <= 90.0) {
    const ratio = (deg - 45.0) / (90.0 - 45.0);
    return Math.round(base45 + ratio * (base90 - base45));
  } else {
    const ratio = (deg - 90.0) / (135.0 - 90.0);
    return Math.round(base90 + ratio * (base135 - base90));
  }
}

function usToAngle(us) {
  if (us >= base45) return 45.0;
  if (us <= base135) return 135.0;
  if (us >= base90) {
    const ratio = (us - base45) / (base90 - base45);
    return Math.round((45.0 + ratio * 45.0) * 10) / 10;
  } else {
    const ratio = (us - base90) / (base135 - base90);
    return Math.round((90.0 + ratio * 45.0) * 10) / 10;
  }
}

function updateCalculatedKicks() {
  const kickUsR = angleToUs(degKickR);
  const kickUsL = angleToUs(degKickL);
  const elKickR = document.getElementById('calc_poskickr');
  const elKickL = document.getElementById('calc_poskickl');
  if (elKickR) elKickR.textContent = kickUsR;
  if (elKickL) elKickL.textContent = kickUsL;
}

function showStatusMessage(msg) {
  let box = document.getElementById('statusMsgBox');
  if (!box) {
    box = document.createElement('div');
    box.id = 'statusMsgBox';
    box.style.position = 'fixed';
    box.style.bottom = '20px';
    box.style.right = '20px';
    box.style.background = '#2c3e50';
    box.style.color = 'white';
    box.style.padding = '10px 16px';
    box.style.borderRadius = '8px';
    box.style.zIndex = '9999';
    document.body.appendChild(box);
  }
  box.textContent = msg;
  box.style.display = 'block';
  clearTimeout(box._hideTimer);
  box._hideTimer = setTimeout(() => { box.style.display = 'none'; }, 1800);
}

function setSensorBox(elId, boxId, value) {
  const el = document.getElementById(elId);
  const box = document.getElementById(boxId);
  if (el) el.textContent = value;
  if (!box) return;
  box.classList.remove("blocked", "free");
  if (value === "BLOCKED") box.classList.add("blocked");
  else if (value === "FREE") box.classList.add("free");
}

function sendMotorDebounced() {
  clearTimeout(motorDebounceTimer);
  motorDebounceTimer = setTimeout(() => {
    ignoreStatusPwmUntil = Date.now() + 1200;
    fetch('/hardware/api/motor', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({fast: parseInt(fastSlider.value), slow: parseInt(slowSlider.value)})
    });
  }, 80);
}

async function setMotor(fast, slow) {
  ignoreStatusPwmUntil = Date.now() + 1500;
  if (fastSlider) fastSlider.value = fast;
  if (slowSlider) slowSlider.value = slow;
  if (fastValLabel) fastValLabel.textContent = fast;
  if (slowValLabel) slowValLabel.textContent = slow;
  if (fastInputField) fastInputField.value = fast;
  if (slowInputField) slowInputField.value = slow;
  await fetch('/hardware/api/motor', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({fast: fast, slow: slow})
  });
}

async function executeServoUs(us) {
  us = Math.max(1000, Math.min(2170, parseInt(us) || 1600));
  if (servoDirectUsInput) servoDirectUsInput.value = us;
  if (servoDirectAngleCalc) servoDirectAngleCalc.textContent = usToAngle(us);
  await fetch('/hardware/api/servo_us', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({us: us})
  });
}

function fillCfgFields(cfg) {
  if (cfg.posrestl !== undefined) document.getElementById('in_posrestl').value = cfg.posrestl;
  if (cfg.posmid !== undefined) document.getElementById('in_posmid').value = cfg.posmid;
  if (cfg.posrestr !== undefined) document.getElementById('in_posrestr').value = cfg.posrestr;
  if (cfg.degkickl !== undefined) document.getElementById('in_degkickl').value = Math.round(cfg.degkickl);
  if (cfg.degkickr !== undefined) document.getElementById('in_degkickr').value = Math.round(cfg.degkickr);
  
  if (cfg.kdelay !== undefined) document.getElementById('in_kdelay').value = cfg.kdelay;
  if (cfg.khold !== undefined) document.getElementById('in_khold').value = cfg.khold;
  if (cfg.rhold !== undefined) document.getElementById('in_rhold').value = cfg.rhold;
  if (cfg.rholdswitch !== undefined) document.getElementById('in_rholdswitch').value = cfg.rholdswitch;
  
  if (cfg.nfcpoll !== undefined) document.getElementById('in_nfcpoll').value = cfg.nfcpoll;
  if (cfg.uidage !== undefined) document.getElementById('in_uidage').value = cfg.uidage;
  if (cfg.cooldown !== undefined) document.getElementById('in_cooldown').value = cfg.cooldown;
  if (cfg.nfctimeout !== undefined) document.getElementById('in_nfctimeout').value = cfg.nfctimeout;
  if (cfg.nfcretries !== undefined) document.getElementById('in_nfcretries').value = cfg.nfcretries;
  
  if (cfg.usthreshmm !== undefined) document.getElementById('in_usthreshmm').value = cfg.usthreshmm;
  if (cfg.usconfirm !== undefined) document.getElementById('in_usconfirm').value = cfg.usconfirm;
  if (cfg.usinterval !== undefined) document.getElementById('in_usinterval').value = cfg.usinterval;
  if (cfg.maxblockms !== undefined) document.getElementById('in_maxblockms').value = cfg.maxblockms;
  
  if (cfg.invert !== undefined) document.getElementById('cfg_invert').checked = cfg.invert;
}

async function poll() {
  try {
    const r = await fetch('/hardware/api/status');
    const d = await r.json();

    document.getElementById('left').textContent = d.left_count;
    document.getElementById('right').textContent = d.right_count;
    document.getElementById('rejected').textContent = d.rejected_count;
    document.getElementById('conn').textContent = d.connected ? "Verbunden" : "Getrennt";
    document.getElementById('nfcReady').textContent =
      d.nfc_ready === true ? "Bereit" : (d.nfc_ready === false ? "Nicht bereit" : "-");

    setSensorBox('ls2', 'ls2box', d.ls2);
    document.getElementById('us2cm').textContent = (d.us2_cm !== null && d.us2_cm !== undefined) ? d.us2_cm.toFixed(1) : '-';

    document.getElementById('uid').textContent = d.last_uid;
    document.getElementById('servostate').textContent = d.servo_state;

    const now = Date.now();
    if (now > ignoreStatusPwmUntil && !fastSlider.dataset.dragging) {
      if (d.fast !== null && d.fast !== undefined) {
        fastSlider.value = d.fast;
        if (fastValLabel) fastValLabel.textContent = d.fast;
        if (document.activeElement !== fastInputField) {
          fastInputField.value = d.fast;
        }
      }
      if (d.slow !== null && d.slow !== undefined) {
        slowSlider.value = d.slow;
        if (slowValLabel) slowValLabel.textContent = d.slow;
        if (document.activeElement !== slowInputField) {
          slowInputField.value = d.slow;
        }
      }
    }

    devModeOn = !!d.dev_mode;
    if (document.activeElement !== document.getElementById('devModeToggle')) {
      document.getElementById('devModeToggle').checked = devModeOn;
    }
    document.getElementById('devPanel').style.display = devModeOn ? 'block' : 'none';

    if (d.cfg && Object.keys(d.cfg).length) {
      if (d.cfg.posrestl) base45 = d.cfg.posrestl;
      if (d.cfg.posmid) base90 = d.cfg.posmid;
      if (d.cfg.posrestr) base135 = d.cfg.posrestr;
      if (d.cfg.degkickl) degKickL = parseFloat(d.cfg.degkickl);
      if (d.cfg.degkickr) degKickR = parseFloat(d.cfg.degkickr);

      updateCalculatedKicks();

      if (!cfgLoadedOnce) {
        fillCfgFields(d.cfg);
        cfgLoadedOnce = true;
      }

      document.getElementById('cur_posrestl').textContent = d.cfg.posrestl;
      document.getElementById('cur_posmid').textContent = d.cfg.posmid || base90;
      document.getElementById('cur_posrestr').textContent = d.cfg.posrestr;
      document.getElementById('cur_degkickl').textContent = Math.round(d.cfg.degkickl || degKickL);
      document.getElementById('cur_degkickr').textContent = Math.round(d.cfg.degkickr || degKickR);
      
      document.getElementById('cur_kdelay').textContent = d.cfg.kdelay;
      document.getElementById('cur_khold').textContent = d.cfg.khold;
      document.getElementById('cur_rhold').textContent = d.cfg.rhold;
      document.getElementById('cur_rholdswitch').textContent = d.cfg.rholdswitch;

      document.getElementById('cur_nfcpoll').textContent = d.cfg.nfcpoll;
      document.getElementById('cur_uidage').textContent = d.cfg.uidage;
      document.getElementById('cur_cooldown').textContent = d.cfg.cooldown;
      document.getElementById('cur_nfctimeout').textContent = d.cfg.nfctimeout;
      document.getElementById('cur_nfcretries').textContent = d.cfg.nfcretries;

      document.getElementById('cur_usthreshmm').textContent = d.cfg.usthreshmm;
      document.getElementById('cur_usconfirm').textContent = d.cfg.usconfirm;
      document.getElementById('cur_usinterval').textContent = d.cfg.usinterval;
      document.getElementById('cur_maxblockms').textContent = d.cfg.maxblockms;
    }

    const rawLog = document.getElementById('rawLog');
    if (rawLog && devModeOn && d.log) {
      rawLog.textContent = d.log.join('\n');
      rawLog.scrollTop = rawLog.scrollHeight;
    }
  } catch (err) {
    console.error("Poll-Fehler:", err);
  }
}

// -------------------- Event Listener --------------------
fastSlider.addEventListener('input', () => {
  fastSlider.dataset.dragging = "1";
  fastValLabel.textContent = fastSlider.value;
  fastInputField.value = fastSlider.value;
  sendMotorDebounced();
});

slowSlider.addEventListener('input', () => {
  slowSlider.dataset.dragging = "1";
  slowValLabel.textContent = slowSlider.value;
  slowInputField.value = slowSlider.value;
  sendMotorDebounced();
});

fastSlider.addEventListener('change', () => { 
  fastSlider.dataset.dragging = ""; 
  ignoreStatusPwmUntil = Date.now() + 1000;
});

slowSlider.addEventListener('change', () => { 
  slowSlider.dataset.dragging = ""; 
  ignoreStatusPwmUntil = Date.now() + 1000;
});

document.getElementById('motorOnBtn').addEventListener('click', () => setMotor(255, 110));
document.getElementById('motorOffBtn').addEventListener('click', () => setMotor(0, 0));

document.getElementById('motorInputApply').addEventListener('click', () => {
  const fast = Math.max(0, Math.min(255, parseInt(fastInputField.value) || 0));
  const slow = Math.max(0, Math.min(255, parseInt(slowInputField.value) || 0));
  setMotor(fast, slow);
});

document.getElementById('servoAngleApply').addEventListener('click', async () => {
  const angle = parseFloat(document.getElementById('servoAngle').value) || 45.0;
  const us = angleToUs(angle);
  executeServoUs(us);
  showStatusMessage(angle + '° (' + us + ' µs) angefahren');
});

servoDirectUsInput.addEventListener('input', () => {
  clearTimeout(servoTypeTimer);
  const raw = servoDirectUsInput.value;
  if (!raw || raw.length < 3) return;
  servoTypeTimer = setTimeout(() => {
    executeServoUs(parseInt(raw));
  }, 400);
});

document.querySelectorAll('.btn-step').forEach(btn => {
  btn.addEventListener('click', () => {
    clearTimeout(servoTypeTimer);
    const step = parseInt(btn.dataset.step) || 0;
    let current = parseInt(servoDirectUsInput.value) || 1600;
    current += step;
    executeServoUs(current);
  });
});

document.getElementById('simtagBtn').addEventListener('click', async () => {
  const uid = document.getElementById('simUid').value;
  const r = await fetch('/hardware/api/simtag', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({uid: uid})
  });
  const d = await r.json();
  if (d.ok === false) showStatusMessage('Simulation nur im Entwicklermodus moeglich.');
});

document.getElementById('devModeToggle').addEventListener('change', async (e) => {
  await fetch('/hardware/api/dev_mode', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({enabled: e.target.checked})
  });
});

document.getElementById('testKickLBtn').addEventListener('click', async () => {
  await fetch('/hardware/api/test_kick', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({side: 'L'})
  });
});

document.getElementById('testKickRBtn').addEventListener('click', async () => {
  await fetch('/hardware/api/test_kick', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({side: 'R'})
  });
});

document.querySelectorAll('.tune').forEach(input => {
  let timer = null;
  input.addEventListener('input', () => {
    clearTimeout(timer);
    timer = setTimeout(async () => {
      const key = input.dataset.key;
      const value = input.value;
      if (value === '') return;

      if (key === 'POSRESTL') base45 = parseInt(value);
      if (key === 'POSMID') base90 = parseInt(value);
      if (key === 'POSRESTR') base135 = parseInt(value);
      if (key === 'DEGKICKL') degKickL = parseFloat(value);
      if (key === 'DEGKICKR') degKickR = parseFloat(value);

      updateCalculatedKicks();

      await fetch('/hardware/api/config', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({key: key, value: value})
      });
      showStatusMessage(key + ' -> ' + value + ' gesendet');
    }, 500);
  });
});

document.getElementById('cfg_invert').addEventListener('change', async (e) => {
  await fetch('/hardware/api/config', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({key: 'INVERT', value: e.target.checked ? 1 : 0})
  });
});

document.getElementById('cfgRefreshBtn').addEventListener('click', async () => {
  cfgLoadedOnce = false;
  await fetch('/hardware/api/config_refresh', {method: 'POST'});
});

updateCalculatedKicks();
setInterval(poll, 400);
poll();