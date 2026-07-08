import json, sqlite3, threading, time
from datetime import datetime
from flask import Flask, request, redirect, url_for, render_template_string
from serial import Serial

DB = 'duckomat.db'
SERIAL_PORT = '/dev/ttyUSB0'
BAUD = 115200
app = Flask(__name__)
HTML = """
<!doctype html><html><head><meta charset='utf-8'><title>Duckomat</title>
<style>body{font-family:system-ui;margin:20px;max-width:1100px} table{border-collapse:collapse;width:100%} td,th{border:1px solid #ccc;padding:6px} input{padding:6px}</style></head><body>
<h1>Duckomat</h1>
<p>Gesamt erkannt: {{stats.total_events}} | bekannte UIDs: {{stats.known_uids}} | unbekannte UIDs: {{stats.unknown_uids}}</p>
<h2>Unbekannte UIDs</h2>
<table><tr><th>UID</th><th>Slots</th><th>Aktion</th></tr>
{% for row in unknown %}
<tr><td>{{row['uid']}}</td><td>{{row['cnt']}}</td><td><form method='post' action='/map'><input type='hidden' name='uid' value='{{row['uid']}}'><input name='duck_number' type='number' placeholder='Nummer' required><input name='label' placeholder='Name optional'><button type='submit'>Speichern</button></form></td></tr>
{% endfor %}</table>
<h2>Letzte Ereignisse</h2>
<table><tr><th>Zeit</th><th>UID</th><th>Nummer</th><th>Status</th><th>Slot</th></tr>
{% for row in events %}<tr><td>{{row['detected_at']}}</td><td>{{row['uid']}}</td><td>{{row['duck_number']}}</td><td>{{row['status']}}</td><td>{{row['slot']}}</td></tr>{% endfor %}</table>
</body></html>
"""

def db():
    con = sqlite3.connect(DB)
    con.row_factory = sqlite3.Row
    return con

def init_db():
    con = db(); cur = con.cursor()
    cur.execute('CREATE TABLE IF NOT EXISTS tags(uid TEXT PRIMARY KEY, duck_number INTEGER, label TEXT, created_at TEXT)')
    cur.execute('CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT, uid TEXT, duck_number INTEGER, slot INTEGER, detected_at TEXT, status TEXT, raw TEXT)')
    con.commit(); con.close()

def save_event(uid, slot, status='read', raw=''):
    con = db(); cur = con.cursor()
    row = cur.execute('SELECT duck_number FROM tags WHERE uid=?', (uid,)).fetchone()
    duck_number = row['duck_number'] if row else None
    cur.execute('INSERT INTO events(uid, duck_number, slot, detected_at, status, raw) VALUES(?,?,?,?,?,?)', (uid, duck_number, slot, datetime.now().isoformat(timespec='seconds'), status, raw))
    con.commit(); con.close()

def serial_thread():
    ser = None
    while True:
        try:
            if ser is None or not ser.is_open:
                ser = Serial(SERIAL_PORT, BAUD, timeout=1)
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
            if line.startswith('{'):
                try:
                    msg = json.loads(line)
                except Exception:
                    continue
                if msg.get('type') == 'uid' and msg.get('uid'):
                    save_event(msg['uid'], int(msg.get('slot', -1)), msg.get('state', 'read'), line)
        except Exception:
            time.sleep(2)
            ser = None

@app.route('/')
def index():
    con = db(); cur = con.cursor()
    unknown = cur.execute('SELECT uid, COUNT(*) AS cnt FROM events WHERE uid NOT IN (SELECT uid FROM tags) GROUP BY uid ORDER BY cnt DESC').fetchall()
    events = cur.execute('SELECT uid, duck_number, slot, detected_at, status FROM events ORDER BY id DESC LIMIT 30').fetchall()
    stats = {
        'total_events': cur.execute('SELECT COUNT(*) FROM events').fetchone()[0],
        'known_uids': cur.execute('SELECT COUNT(*) FROM tags').fetchone()[0],
        'unknown_uids': cur.execute('SELECT COUNT(DISTINCT uid) FROM events WHERE uid NOT IN (SELECT uid FROM tags)').fetchone()[0],
    }
    con.close()
    return render_template_string(HTML, unknown=unknown, events=events, stats=stats)

@app.route('/map', methods=['POST'])
def map_uid():
    uid = request.form['uid'].strip().upper()
    duck_number = int(request.form['duck_number'])
    label = request.form.get('label', '').strip()
    con = db(); cur = con.cursor()
    cur.execute('INSERT INTO tags(uid, duck_number, label, created_at) VALUES(?,?,?,?) ON CONFLICT(uid) DO UPDATE SET duck_number=excluded.duck_number, label=excluded.label', (uid, duck_number, label, datetime.now().isoformat(timespec='seconds')))
    cur.execute('UPDATE events SET duck_number=? WHERE uid=?', (duck_number, uid))
    con.commit(); con.close()
    return redirect(url_for('index'))

if __name__ == '__main__':
    init_db()
    threading.Thread(target=serial_thread, daemon=True).start()
    app.run(host='0.0.0.0', port=5000, debug=False)
