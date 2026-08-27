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
  box._hideTimer = setTimeout(() => { box.style.display = 'none'; }, 2000);
}

function setSensorBox(elId, boxId, value) {
  const el = document.getElementById(elId);
  const box = document.getElementById(boxId);
  el.textContent = value;
  if (!box) return;
  box.classList.remove("blocked", "free");
  if (value === "BLOCKED") box.classList.add("blocked");
  else if (value === "FREE") box.classList.add("free");
}

let motorDebounceTimer = null;
function sendMotorDebounced() {
  clearTimeout(motorDebounceTimer);
  motorDebounceTimer = setTimeout(() => {
    fetch('/hardware/api/motor', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({fast: fastSlider.value, slow: slowSlider.value})
    });
  }, 80);
}

async function setMotor(fast, slow) {
  fastSlider.value = fast;
  slowSlider.value = slow;
  document.getElementById('fastval').textContent = fast;
  document.getElementById('slowval').textContent = slow;
  await fetch('/hardware/api/motor', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({fast: fast, slow: slow})
  });
}

let cfgLoadedOnce = false;
let devModeOn = false;

function usToAngle(us, usLeft, usRight) {
  if (usRight === usLeft) return 45;
  const ratio = (us - usLeft) / (usRight - usLeft);
  return Math.round((45 + ratio * 90) * 10) / 10;
}

async function poll() {
  const r = await fetch('/hardware/api/status');
  const d = await r.json();

  document.getElementById('left').textContent = d.left_count;
  document.getElementById('right').textContent = d.right_count;
  document.getElementById('rejected').textContent = d.rejected_count;
  document.getElementById('conn').textContent = d.connected ? "Verbunden" : "Getrennt";
  document.getElementById('nfcReady').textContent =
    d.nfc_ready === true ? "Bereit" : (d.nfc_ready === false ? "Nicht bereit" : "-");

  setSensorBox('ls1', 'ls1box', d.ls1);
  setSensorBox('ls2', 'ls2box', d.ls2);
  document.getElementById('us1cm').textContent = (d.us1_cm !== null && d.us1_cm !== undefined) ? d.us1_cm.toFixed(1) : '-';
  document.getElementById('us2cm').textContent = (d.us2_cm !== null && d.us2_cm !== undefined) ? d.us2_cm.toFixed(1) : '-';
  document.getElementById('uid').textContent = d.last_uid;
  document.getElementById('servostate').textContent = d.servo_state;
  document.getElementById('nfcModeDisplay').textContent = d.nfcmode || '-';

  if (d.fast !== null && !fastSlider.dataset.dragging) {
    fastSlider.value = d.fast;
    document.getElementById('fastval').textContent = d.fast;
    if (document.activeElement !== document.getElementById('fastInput')) {
      document.getElementById('fastInput').value = d.fast;
    }
  }
  if (d.slow !== null && !slowSlider.dataset.dragging) {
    slowSlider.value = d.slow;
    document.getElementById('slowval').textContent = d.slow;
    if (document.activeElement !== document.getElementById('slowInput')) {
      document.getElementById('slowInput').value = d.slow;
    }
  }

  devModeOn = !!d.dev_mode;
  if (document.activeElement !== document.getElementById('devModeToggle')) {
    document.getElementById('devModeToggle').checked = devModeOn;
  }
  document.getElementById('devPanel').style.display = devModeOn ? 'block' : 'none';

  if (d.cfg && Object.keys(d.cfg).length) {
    if (!cfgLoadedOnce) {
      fillCfgFields(d.cfg);
      cfgLoadedOnce = true;
    }
    document.getElementById('cur_posrestl').textContent = d.cfg.posrestl;
    document.getElementById('cur_poskickl').textContent = d.cfg.poskickl;
    document.getElementById('cur_poskickr').textContent = d.cfg.poskickr;
    document.getElementById('cur_posrestr').textContent = d.cfg.posrestr;
    document.getElementById('cur_kdelay').textContent = d.cfg.kdelay;
    document.getElementById('cur_khold').textContent = d.cfg.khold;
    document.getElementById('cur_rhold').textContent = d.cfg.rhold;
    document.getElementById('cur_rholdswitch').textContent = d.cfg.rholdswitch;
    document.getElementById('cur_usthreshmm').textContent = d.cfg.usthreshmm;
    document.getElementById('cur_usconfirm').textContent = d.cfg.usconfirm;
    document.getElementById('cur_usinterval').textContent = d.cfg.usinterval;

    document.getElementById('ang_posrestl').textContent = usToAngle(d.cfg.posrestl, d.cfg.posrestl, d.cfg.posrestr);
    document.getElementById('ang_poskickl').textContent = usToAngle(d.cfg.poskickl, d.cfg.posrestl, d.cfg.posrestr);
    document.getElementById('ang_poskickr').textContent = usToAngle(d.cfg.poskickr, d.cfg.posrestl, d.cfg.posrestr);
    document.getElementById('ang_posrestr').textContent = usToAngle(d.cfg.posrestr, d.cfg.posrestl, d.cfg.posrestr);
  }

  const rawLog = document.getElementById('rawLog');
  if (rawLog && devModeOn && d.log) {
    rawLog.textContent = d.log.join('\n');
    rawLog.scrollTop = rawLog.scrollHeight;
  }
}

function fillCfgFields(cfg) {
  document.getElementById('in_posrestl').value = cfg.posrestl;
  document.getElementById('in_poskickl').value = cfg.poskickl;
  document.getElementById('in_poskickr').value = cfg.poskickr;
  document.getElementById('in_posrestr').value = cfg.posrestr;
  document.getElementById('in_kdelay').value = cfg.kdelay;
  document.getElementById('in_khold').value = cfg.khold;
  document.getElementById('in_rhold').value = cfg.rhold;
  document.getElementById('in_rholdswitch').value = cfg.rholdswitch;
  document.getElementById('in_usthreshmm').value = cfg.usthreshmm;
  document.getElementById('in_usconfirm').value = cfg.usconfirm;
  document.getElementById('in_usinterval').value = cfg.usinterval;
  document.getElementById('cfg_invert').checked = cfg.invert;
}

const fastSlider = document.getElementById('fastSlider');
const slowSlider = document.getElementById('slowSlider');

fastSlider.addEventListener('input', () => {
  fastSlider.dataset.dragging = "1";
  document.getElementById('fastval').textContent = fastSlider.value;
  sendMotorDebounced();
});
slowSlider.addEventListener('input', () => {
  slowSlider.dataset.dragging = "1";
  document.getElementById('slowval').textContent = slowSlider.value;
  sendMotorDebounced();
});
fastSlider.addEventListener('change', () => { fastSlider.dataset.dragging = ""; });
slowSlider.addEventListener('change', () => { slowSlider.dataset.dragging = ""; });

document.getElementById('motorOnBtn').addEventListener('click', () => setMotor(255, 130));
document.getElementById('motorOffBtn').addEventListener('click', () => setMotor(0, 0));

document.getElementById('motorInputApply').addEventListener('click', () => {
  const fast = Math.max(0, Math.min(255, parseInt(document.getElementById('fastInput').value) || 0));
  const slow = Math.max(0, Math.min(255, parseInt(document.getElementById('slowInput').value) || 0));
  setMotor(fast, slow);
});

document.getElementById('servoAngleApply').addEventListener('click', async () => {
  const angle = document.getElementById('servoAngle').value;
  await fetch('/hardware/api/servo_angle', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({angle: angle})
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

document.getElementById('nfcModeContBtn').addEventListener('click', async () => {
  await fetch('/hardware/api/nfcmode', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mode: 'CONTINUOUS'})
  });
});
document.getElementById('nfcModeDuckBtn').addEventListener('click', async () => {
  await fetch('/hardware/api/nfcmode', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({mode: 'DUCKONLY'})
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

document.getElementById('batchSizeApply').addEventListener('click', async () => {
  const size = document.getElementById('cfg_batchsize').value;
  await fetch('/hardware/api/batch_size', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({size: size})
  });
  showStatusMessage('Kistengroesse auf ' + size + ' gesetzt.');
});

setInterval(poll, 400);
poll();
