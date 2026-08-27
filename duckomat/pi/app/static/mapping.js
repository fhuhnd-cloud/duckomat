const fastSlider = document.getElementById('fastSlider');
const slowSlider = document.getElementById('slowSlider');

let motorDebounceTimer = null;
function sendMotorDebounced() {
  clearTimeout(motorDebounceTimer);
  motorDebounceTimer = setTimeout(() => {
    fetch('/mapping/api/motor', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({fast: fastSlider.value, slow: slowSlider.value})
    });
  }, 80);
}

async function loadTable() {
  const r = await fetch('/mapping/api/list');
  const rows = await r.json();
  const tbody = document.querySelector('#mappingTable tbody');
  tbody.innerHTML = '';
  rows.forEach(row => {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${row.uid}</td><td>${row.nummer}</td><td>${row.created_at}</td>
      <td><button onclick="deleteRow('${row.uid}')">Loeschen</button></td>`;
    tbody.appendChild(tr);
  });
}

async function deleteRow(uid) {
  await fetch('/mapping/api/delete', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({uid: uid})
  });
  loadTable();
  pollStatus();
}

async function pollStatus() {
  const r = await fetch('/mapping/api/status');
  const d = await r.json();

  document.getElementById('liveUid').textContent = d.last_live_uid ?? '-';
  document.getElementById('mappedCount').textContent = d.mapped_count;
  document.getElementById('autoState').textContent = d.auto_active ? "AKTIV" : "GESTOPPT";
  document.getElementById('autoLast').textContent = d.last_assigned
    ? `UID ${d.last_assigned.uid} -> Nummer ${d.last_assigned.nummer}` : '-';
  document.getElementById('simPanel').style.display = d.dev_mode ? 'block' : 'none';
}

document.getElementById('useLiveUidBtn').addEventListener('click', async () => {
  const r = await fetch('/mapping/api/status');
  const d = await r.json();
  if (d.last_live_uid) document.getElementById('newUid').value = d.last_live_uid;
});

document.getElementById('addBtn').addEventListener('click', async () => {
  const uid = document.getElementById('newUid').value;
  const nummer = document.getElementById('newNummer').value;
  await fetch('/mapping/api/add', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({uid: uid, nummer: nummer})
  });
  document.getElementById('newUid').value = '';
  document.getElementById('newNummer').value = '';
  loadTable();
  pollStatus();
});

document.getElementById('autoNumberBtn').addEventListener('click', async () => {
  const r = await fetch('/mapping/api/next_number');
  const d = await r.json();
  document.getElementById('newNummer').value = d.next;
});

document.getElementById('autoStartBtn').addEventListener('click', async () => {
  if (!confirm('Das bestehende Mapping wird geloescht und die Automatik gestartet. Fortfahren?')) return;
  await fetch('/mapping/api/auto_start', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({reset: true})
  });
  loadTable();
  pollStatus();
});

document.getElementById('autoStopBtn').addEventListener('click', async () => {
  await fetch('/mapping/api/auto_stop', {method: 'POST'});
  pollStatus();
});

document.getElementById('resetAllBtn').addEventListener('click', async () => {
  if (!confirm('Wirklich das GESAMTE Mapping unwiderruflich loeschen?')) return;
  await fetch('/mapping/api/reset_all', {method: 'POST'});
  loadTable();
  pollStatus();
});

document.getElementById('importBtn').addEventListener('click', async () => {
  const fileInput = document.getElementById('csvFile');
  if (!fileInput.files.length) return;
  const formData = new FormData();
  formData.append('file', fileInput.files[0]);
  const r = await fetch('/mapping/api/import_csv', { method: 'POST', body: formData });
  const d = await r.json();
  alert('Importiert: ' + d.imported);
  loadTable();
  pollStatus();
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

fastSlider.addEventListener('input', () => {
  document.getElementById('fastval').textContent = fastSlider.value;
  sendMotorDebounced();
});
slowSlider.addEventListener('input', () => {
  document.getElementById('slowval').textContent = slowSlider.value;
  sendMotorDebounced();
});

setInterval(pollStatus, 500);
loadTable();
pollStatus();
