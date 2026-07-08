from flask import Flask, request, jsonify
import serial, threading, time, json, csv, io, sqlite3
from datetime import datetime

APP = Flask(__name__, static_folder='.', static_url_path='')

SERIAL_PORT = '/dev/ttyUSB0'
BAUDRATE = 115200
DB_PATH = 'duckomat.db'

ser = None
lock = threading.Lock()
started = False

runtime = {
    'mode': 'IDLE',
    'current_session_id': None,
    'current_mapping_mode': 'idle',
    'inventory_direction': 'left',
    'latest': {
    'boot': None,
    'sensors': {
        'ls1': {'type': 'sensor', 'name': 'ls1', 'state': 1, 'event': 'init'},
        'ls2': {'type': 'sensor', 'name': 'ls2', 'state': 1, 'event': 'init'}
    },
        'actuators': {},
        'machine': None,
        'rfid': None,
        'last_uid': None,
        'errors': []
    },
    'serial_connected': False
}

def now_iso():
    return datetime.utcnow().isoformat(timespec='seconds') + 'Z'

def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    with db() as c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS mappings(
            uid TEXT PRIMARY KEY,
            duck_number INTEGER UNIQUE,
            active INTEGER DEFAULT 1,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS inventory_sessions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            started_at TEXT DEFAULT CURRENT_TIMESTAMP,
            ended_at TEXT,
            mode TEXT,
            status TEXT,
            expected_total INTEGER DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS inventory_scans(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER,
            uid TEXT,
            duck_number INTEGER,
            result TEXT,
            box TEXT,
            timestamp TEXT DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS box_state(
            box_name TEXT PRIMARY KEY,
            count INTEGER DEFAULT 0,
            capacity INTEGER DEFAULT 100,
            state TEXT DEFAULT 'open',
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS system_events(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source TEXT,
            level TEXT,
            event_type TEXT,
            payload TEXT,
            timestamp TEXT DEFAULT CURRENT_TIMESTAMP
        );
        """)
        cols = [r[1] for r in c.execute('PRAGMA table_info(inventory_sessions)').fetchall()]
        if 'expected_total' not in cols:
            c.execute('ALTER TABLE inventory_sessions ADD COLUMN expected_total INTEGER DEFAULT 0')
        for box_name in ('left', 'right', 'front'):
            c.execute(
                "INSERT OR IGNORE INTO box_state(box_name, count, capacity, state) VALUES (?, 0, 100, 'open')",
                (box_name,)
            )

def log_event(source, level, event_type, payload):
    with db() as c:
        c.execute(
            'INSERT INTO system_events(source, level, event_type, payload, timestamp) VALUES (?, ?, ?, ?, ?)',
            (source, level, event_type, json.dumps(payload, ensure_ascii=False), now_iso())
        )

def send(msg):
    data = (json.dumps(msg, separators=(',', ':')) + '\n').encode('utf-8')
    with lock:
        if ser and ser.is_open:
            ser.write(data)
            ser.flush()
            log_event('pi', 'info', 'serial_command', msg)
            return True
    return False

def get_uid_mapping(uid):
    if not uid:
        return None
    with db() as c:
        row = c.execute(
            'SELECT duck_number FROM mappings WHERE uid = ? AND active = 1',
            (uid,)
        ).fetchone()
    return dict(row) if row else None

def handle_evt(evt):
    t = evt.get('type')
    if t == 'boot':
        runtime['latest']['boot'] = evt
    elif t == 'sensor':
        runtime['latest']['sensors'][evt.get('name', 'unknown')] = evt
    elif t == 'actuator':
        runtime['latest']['actuators'][evt.get('name', 'unknown')] = evt
    elif t == 'machine':
        runtime['latest']['machine'] = evt
    elif t == 'rfid':
        runtime['latest']['rfid'] = evt
        runtime['latest']['last_uid'] = evt.get('uid')
    elif t == 'error':
        runtime['latest']['errors'].append(evt)
        runtime['latest']['errors'] = runtime['latest']['errors'][-50:]

def extract_json_objects(buffer):
    objs = []
    start = None
    depth = 0
    in_string = False
    escape = False

    i = 0
    while i < len(buffer):
        ch = buffer[i]

        if start is None:
            if ch == '{':
                start = i
                depth = 1
                in_string = False
                escape = False
        else:
            if in_string:
                if escape:
                    escape = False
                elif ch == '\\':
                    escape = True
                elif ch == '"':
                    in_string = False
            else:
                if ch == '"':
                    in_string = True
                elif ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1
                    if depth == 0:
                        objs.append(buffer[start:i + 1])
                        start = None
        i += 1

    remainder = buffer[start:] if start is not None else ''
    return objs, remainder
    
    
def serial_reader():
    rx_buffer = ''

    while True:
        try:
            runtime['serial_connected'] = bool(ser and ser.is_open)

            if ser and ser.is_open:
                waiting = ser.in_waiting
                if waiting:
                    chunk = ser.read(waiting).decode('utf-8', 'ignore')
                    if chunk:
                        rx_buffer += chunk

                        objects, rx_buffer = extract_json_objects(rx_buffer)

                        for raw in objects:
                            try:
                                evt = json.loads(raw)
                                handle_evt(evt)
                            except Exception as exc:
                                runtime['latest']['errors'].append({
                                    'type': 'error',
                                    'code': 'JSON_PARSE',
                                    'message': str(exc),
                                    'raw': raw[-300:],
                                    'ts': now_iso()
                                })
                                runtime['latest']['errors'] = runtime['latest']['errors'][-50:]

                        if len(rx_buffer) > 2000:
                            rx_buffer = rx_buffer[-500:]

        except Exception as exc:
            runtime['latest']['errors'].append({
                'type': 'error',
                'code': 'SERIAL_READER',
                'message': str(exc),
                'ts': now_iso()
            })
            runtime['latest']['errors'] = runtime['latest']['errors'][-50:]

        time.sleep(0.02)

def start_once():
    global ser, started
    if started:
        return
    init_db()
    try:
        ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
    except Exception:
        ser = None
    threading.Thread(target=serial_reader, daemon=True).start()
    started = True

@APP.before_request
def ensure_started():
    start_once()

@APP.route('/')
def index():
    return APP.send_static_file('startmenue_preview.html')

@APP.route('/hardware')
def hardware():
    return APP.send_static_file('hardware_preview_final-3.html')
    
@APP.route("/hardware-minimal")
def hardware_minimal():
    return APP.send_static_file("hardware_minimal.html")

@APP.route('/sensor-status')
def sensor_status():
    return APP.send_static_file('sensorstatus.html')
    
@APP.get('/api/hardware/status')
def hardware_status():
    uid = runtime['latest'].get('last_uid')
    mapping = get_uid_mapping(uid)
    return jsonify(runtime['latest'] | {
        'serial_connected': runtime['serial_connected'],
        'rfid_mapping': mapping
    })

@APP.post('/api/hardware/ping')
def ping():
    return jsonify({'ok': send({'cmd': 'ping'})})

@APP.post('/api/hardware/belt/start')
def belt_start():
    return jsonify({'ok': send({'cmd': 'belt', 'action': 'start'})})

@APP.post('/api/hardware/belt/stop')
def belt_stop():
    return jsonify({'ok': send({'cmd': 'belt', 'action': 'stop'})})

@APP.post('/api/hardware/belt/resume')
def belt_resume():
    return jsonify({'ok': send({'cmd': 'belt', 'action': 'resume'})})

@APP.post('/api/hardware/emergency-stop')
def emergency_stop():
    runtime['mode'] = 'ERROR'
    return jsonify({'ok': send({'cmd': 'emergency_stop'}), 'runtime_mode': runtime['mode']})

@APP.post('/api/hardware/relay')
def relay():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({
        'cmd': 'actuator',
        'name': payload['name'],
        'state': int(payload['state'])
    })})

@APP.post('/api/hardware/motor')
def motor():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({
        'cmd': 'actuator',
        'name': payload['name'],
        'state': int(payload.get('state', 0)),
        'value': int(payload.get('value', 0))
    })})

@APP.post('/api/hardware/servo/angle')
def servo_angle():
    payload = request.get_json(force=True)
    angle = int(payload.get('angle', 90))
    angle = max(45, min(135, angle))
    pulse = round(990 + ((angle - 45) / 90.0) * (2030 - 990))
    return jsonify({'ok': send({
        'cmd': 'actuator',
        'name': 'servo',
        'state': 1,
        'value': pulse
    }), 'angle': angle, 'pulse': pulse})

@APP.post('/api/hardware/config')
def config():
    payload = request.get_json(force=True)
    cmd = {'cmd': 'set_config'}
    cmd.update({k: payload[k] for k in ('auto_sort_enabled', 'pwm_fast', 'pwm_slow', 'service_mode') if k in payload})
    return jsonify({'ok': send(cmd), 'sent': cmd})

if __name__ == '__main__':
    APP.run(host='0.0.0.0', port=5000, debug=True)
