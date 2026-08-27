let stopOnRejectsDirty = false;
let startDirDirty = false;

const fastSlider = document.getElementById('fastSlider');
const slowSlider = document.getElementById('slowSlider');
let motorDebounceTimer = null;

function sendMotorDebounced() {
  clearTimeout(motorDebounceTimer);
  motorDebounceTimer = setTimeout(() => {
    fetch('/inventur/api/motor', {
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
  await fetch('/inventur/api/motor', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({fast: fast, slow: slow})
  });
}

async function pollHardwareStatus() {
  const r = await fetch('/hardware/api/status');
  const d = await r.json();
  document.getElementById('simPanel').style.display = d.dev_mode ? 'block' : 'none';
  document.getElementById('conn').textContent = d.connected ? "Verbunden" : "Getrennt";
  document.getElementById('nfcReady').textContent =
    d.nfc_ready === true ? "Bereit" : (d.nfc_ready === false ? "Nicht bereit" : "-");

  if (!fastSlider.dataset.dragging && d.fast !== null && d.fast !== undefined) {
    fastSlider.value = d.fast;
    document.getElementById('fastval').textContent = d.fast;
    if (document.activeElement !== document.getElementById('fastInput')) {
      document.getElementById('fastInput').value = d.fast;
    }
  }
  if (!slowSlider.dataset.dragging && d.slow !== null && d.slow !== undefined) {
    slowSlider.value = d.slow;
    document.getElementById('slowval').textContent = d.slow;
    if (document.activeElement !== document.getElementById('slowInput')) {
      document.getElementById('slowInput').value = d.slow;
    }
  }
}

async function poll() {
  const r = await fetch('/inventur/api/status');
  const d = await r.json();
  pollHardwareStatus();

  document.getElementById('runningState').textContent = d.running ? "LAEUFT" : "GESTOPPT";
  document.getElementById('direction').textContent = d.current_direction;
  document.getElementById('batch').textContent = d.batch_count;
  document.getElementById('batchSize').textContent = d.batch_size;
  document.getElementById('sLeft').textContent = d.session_left;
  document.getElementById('sRight').textContent = d.session_right;
  document.getElementById('sRejected').textContent = d.session_rejected;
  document.getElementById('rejectBatchCount').textContent = d.reject_batch_count;
  document.getElementById('rejectStopSize').textContent = d.reject_stop_size;

  if (!stopOnRejectsDirty) {
    document.getElementById('stopOnRejectsCheck').checked = d.stop_on_rejects;
  }
  if (!startDirDirty && !d.running) {
    document.querySelectorAll('input[name="startDir"]').forEach(r => {
      r.checked = (r.value === d.start_direction);
    });
  }
  document.getElementById('rejectPausePanel').style.display = d.paused_for_rejects ? 'block' : 'none';

  const tbody = document.querySelector('#logTable tbody');
  tbody.innerHTML = '';
  d.log.forEach(entry => {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${entry.ts}</td><td>${entry.uid ?? '-'}</td>
      <td>${entry.nummer ?? '-'}</td><td>${entry.side ?? '-'}</td><td>${entry.result}</td>`;
    tbody.appendChild(tr);
  });
}

document.querySelectorAll('input[name="startDir"]').forEach(radio => {
  radio.addEventListener('change', async (e) => {
    startDirDirty = true;
    await fetch('/inventur/api/set_start_direction', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({direction: e.target.value})
    });
    startDirDirty = false;
  });
});

document.getElementById('motorOnBtn').addEventListener('click', () => setMotor(255, 130));
document.getElementById('motorOffBtn').addEventListener('click', () => setMotor(0, 0));

document.getElementById('motorInputApply').addEventListener('click', () => {
  const fast = Math.max(0, Math.min(255, parseInt(document.getElementById('fastInput').value) || 0));
  const slow = Math.max(0, Math.min(255, parseInt(document.getElementById('slowInput').value) || 0));
  setMotor(fast, slow);
});

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

document.getElementById('startBtn').addEventListener('click', async () => { await fetch('/inventur/api/start', {method: 'POST'}); });
document.getElementById('stopBtn').addEventListener('click', async () => { await fetch('/inventur/api/stop', {method: 'POST'}); });
document.getElementById('resetBtn').addEventListener('click', async () => { await fetch('/inventur/api/reset', {method: 'POST'}); });
document.getElementById('resumeRejectsBtn').addEventListener('click', async () => { await fetch('/inventur/api/resume_after_rejects', {method: 'POST'}); });

document.getElementById('stopOnRejectsCheck').addEventListener('change', async (e) => {
  stopOnRejectsDirty = true;
  await fetch('/inventur/api/set_stop_on_rejects', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({enabled: e.target.checked})
  });
  stopOnRejectsDirty = false;
});

document.getElementById('rejectStopSizeApply').addEventListener('click', async () => {
  const size = document.getElementById('rejectStopSizeInput').value;
  await fetch('/inventur/api/set_reject_stop_size', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({size: size})
  });
});

document.getElementById('simtagBtn').addEventListener('click', async () => {
  const uid = document.getElementById('simUid').value;
  const r = await fetch('/hardware/api/simtag', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({uid: uid})
  });
  const d = await r.json();
  if (d.ok === false) alert('Simulation nur im Entwicklermodus moeglich.');
});

setInterval(poll, 500);
poll();
