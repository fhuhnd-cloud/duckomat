from flask import Flask, request, jsonify
import serial, threading, time, sqlite3, json, csv, io
from datetime import datetime

# Host current directory as static files
APP = Flask(__name__, static_folder='.', static_url_path='')

SERIAL_PORT = '/dev/ttyUSB0'
BAUDRATE = 115200
DB_PATH = 'duckomat.db'
ser = None
lock = threading.Lock()
started = False

runtime = {'mode':'IDLE','current_session_id':None,'current_mapping_mode':'idle','inventory_direction':'left',
           'latest':{'boot':None,'sensors':{},'actuators':{},'machine':None,'rfid':None,'last_uid':None,'errors':[]}}

def now_iso(): return datetime.utcnow().isoformat(timespec='seconds') + 'Z'
def db(): conn = sqlite3.connect(DB_PATH); conn.row_factory = sqlite3.Row; return conn
def init_db():
    with db() as c:
        c.executescript("""
CREATE TABLE IF NOT EXISTS mappings(uid TEXT PRIMARY KEY,duck_number INTEGER UNIQUE,active INTEGER DEFAULT 1,created_at TEXT DEFAULT CURRENT_TIMESTAMP,updated_at TEXT DEFAULT CURRENT_TIMESTAMP);
CREATE TABLE IF NOT EXISTS inventory_sessions(id INTEGER PRIMARY KEY AUTOINCREMENT,started_at TEXT DEFAULT CURRENT_TIMESTAMP,ended_at TEXT,mode TEXT,status TEXT,expected_total INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS inventory_scans(id INTEGER PRIMARY KEY AUTOINCREMENT,session_id INTEGER,uid TEXT,duck_number INTEGER,result TEXT,box TEXT,timestamp TEXT DEFAULT CURRENT_TIMESTAMP);
CREATE TABLE IF NOT EXISTS box_state(box_name TEXT PRIMARY KEY,count INTEGER DEFAULT 0,capacity INTEGER DEFAULT 100,state TEXT DEFAULT 'open',updated_at TEXT DEFAULT CURRENT_TIMESTAMP);
CREATE TABLE IF NOT EXISTS system_events(id INTEGER PRIMARY KEY AUTOINCREMENT,source TEXT,level TEXT,event_type TEXT,payload TEXT,timestamp TEXT DEFAULT CURRENT_TIMESTAMP);
""")
        cols = [r[1] for r in c.execute("PRAGMA table_info(inventory_sessions)").fetchall()]
        if 'expected_total' not in cols: c.execute("ALTER TABLE inventory_sessions ADD COLUMN expected_total INTEGER DEFAULT 0")
        for box_name in ('left','right','front'): c.execute("INSERT OR IGNORE INTO box_state(box_name, count, capacity, state) VALUES (?, 0, 100, 'open')", (box_name,))

def log_event(source, level, event_type, payload):
    with db() as c: c.execute('INSERT INTO system_events(source, level, event_type, payload, timestamp) VALUES (?, ?, ?, ?, ?)', (source, level, event_type, json.dumps(payload, ensure_ascii=False), now_iso()))

def send(msg):
    data = (json.dumps(msg, separators=(',', ':')) + '\n').encode('utf-8')
    with lock:
        if ser and ser.is_open:
            ser.write(data); ser.flush(); log_event('pi','info','serial_command',msg); return True
    return False

def get_mapping(uid):
    with db() as c: row = c.execute('SELECT uid, duck_number, active, created_at, updated_at FROM mappings WHERE uid = ?', (uid,)).fetchone()
    return dict(row) if row else None

def next_free_number():
    with db() as c: rows = c.execute('SELECT duck_number FROM mappings ORDER BY duck_number').fetchall()
    used = {r['duck_number'] for r in rows}; n = 1
    while n in used: n += 1
    return n

def assign_mapping(uid, duck_number, overwrite=False):
    if not uid or uid == 'NO_TAG': return {'ok':False, 'error':'invalid_uid'}
    with db() as c:
        existing_uid = c.execute('SELECT uid, duck_number FROM mappings WHERE uid = ?', (uid,)).fetchone()
        existing_num = c.execute('SELECT uid, duck_number FROM mappings WHERE duck_number = ?', (duck_number,)).fetchone()
        if existing_num and existing_num['uid'] != uid and not overwrite: return {'ok':False,'error':'number_taken','conflict_uid':existing_num['uid']}
        if existing_num and existing_num['uid'] != uid and overwrite: c.execute('DELETE FROM mappings WHERE uid = ?', (existing_num['uid'],))
        if existing_uid: c.execute('UPDATE mappings SET duck_number = ?, updated_at = ? WHERE uid = ?', (duck_number, now_iso(), uid))
        else: c.execute('INSERT INTO mappings(uid, duck_number, active, created_at, updated_at) VALUES (?, ?, 1, ?, ?)', (uid, duck_number, now_iso(), now_iso()))
    log_event('pi','info','mapping_assigned',{'uid':uid,'duck_number':duck_number,'overwrite':overwrite})
    return {'ok':True,'uid':uid,'duck_number':duck_number}

def clear_all_mappings():
    with db() as c: c.execute('DELETE FROM mappings')
    log_event('pi','warning','mappings_cleared',{})

def session_active_scan_exists(session_id, uid):
    if not uid or uid == 'NO_TAG': return False
    with db() as c: row = c.execute('SELECT id FROM inventory_scans WHERE session_id = ? AND uid = ? AND result = ?', (session_id, uid, 'mapped')).fetchone()
    return row is not None

def inc_box(box_name):
    with db() as c:
        row = c.execute('SELECT count, capacity FROM box_state WHERE box_name = ?', (box_name,)).fetchone()
        if not row: return None
        new_count = row['count'] + 1; new_state = 'full' if new_count >= row['capacity'] else 'open'
        c.execute('UPDATE box_state SET count = ?, state = ?, updated_at = ? WHERE box_name = ?', (new_count, new_state, now_iso(), box_name))
    return {'box_name':box_name,'count':new_count,'capacity':row['capacity'],'state':new_state}

def inventory_decision(uid, rfid_status):
    session_id = runtime['current_session_id']
    if not session_id: return {'result':'no_session','target':'front'}
    if rfid_status == 'no_tag' or not uid or uid == 'NO_TAG': return {'result':'defective','target':'front'}
    mapping = get_mapping(uid)
    if not mapping: return {'result':'unmapped','target':'front'}
    if session_active_scan_exists(session_id, uid): return {'result':'duplicate','target':'front','duck_number':mapping['duck_number']}
    return {'result':'mapped','target':runtime.get('inventory_direction','left'),'duck_number':mapping['duck_number']}

def record_inventory(uid, decision):
    session_id = runtime['current_session_id']; duck_number = decision.get('duck_number'); box = decision.get('target'); result = decision.get('result')
    with db() as c: c.execute('INSERT INTO inventory_scans(session_id, uid, duck_number, result, box, timestamp) VALUES (?, ?, ?, ?, ?, ?)', (session_id, uid, duck_number, result, box, now_iso()))
    box_state = inc_box(box); log_event('pi','info','inventory_scan',{'uid':uid,'decision':decision,'box_state':box_state}); return box_state

def process_rfid_event(evt):
    uid = evt.get('uid'); status = evt.get('status', 'ok')
    if status == 'no_tag' or not uid: uid = 'NO_TAG'
    runtime['latest']['last_uid'] = uid; runtime['latest']['rfid'] = {'uid':uid, 'status':status, 'ts':now_iso()}

    if runtime['mode'] == 'MAPPING_ACTIVE':
        if uid == 'NO_TAG': return {'mode':'mapping','ignored':True,'reason':'defective_not_mappable'}
        if runtime['current_mapping_mode'] == 'auto-new':
            with db() as c: c.execute('DELETE FROM mappings WHERE uid = ?', (uid,))
            number = next_free_number(); return {'mode':'mapping', **assign_mapping(uid, number, overwrite=True)}
        if runtime['current_mapping_mode'] == 'auto-next':
            existing = get_mapping(uid)
            if existing: return {'mode':'mapping','ok':True,'uid':uid,'duck_number':existing['duck_number'],'existing':True}
            number = next_free_number(); return {'mode':'mapping', **assign_mapping(uid, number, overwrite=False)}
        return {'mode':'mapping','ok':True,'uid':uid,'waiting_manual':True}

    if runtime['mode'] == 'INVENTORY_RUNNING':
        decision = inventory_decision(uid, status); box_state = record_inventory(uid, decision); return {'mode':'inventory','uid':uid,'decision':decision,'box_state':box_state}

    return {'mode':runtime['mode'],'uid':uid,'ignored':True}

def serial_reader():
    while True:
        try:
            if ser and ser.is_open and ser.in_waiting:
                line = ser.readline().decode('utf-8', 'ignore').strip()
                if not line: time.sleep(0.02); continue
                try: evt = json.loads(line)
                except Exception: log_event('nano','warning','serial_parse_failed',{'line':line}); time.sleep(0.02); continue
                t = evt.get('type')
                if t == 'boot': runtime['latest']['boot'] = evt
                elif t == 'sensor': runtime['latest']['sensors'][evt.get('name')] = evt
                elif t == 'actuator': runtime['latest']['actuators'][evt.get('name')] = evt
                elif t == 'machine': runtime['latest']['machine'] = evt
                elif t == 'error': runtime['latest']['errors'].append(evt)
                elif t == 'rfid':
                    result = process_rfid_event(evt); runtime['latest']['rfid']['result'] = result
                    if result.get('mode') == 'inventory':
                        decision = result['decision']; send({'cmd':'decision','result':decision['result'],'target':decision['target']})
                log_event('nano','info','serial_event',evt)
        except Exception as exc: log_event('pi','error','serial_reader_exception',{'error':str(exc)})
        time.sleep(0.02)

def start_once():
    global ser, started
    if started: return
    init_db()
    try: ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
    except Exception as exc: ser = None; log_event('pi','warning','serial_open_failed',{'error':str(exc),'port':SERIAL_PORT})
    threading.Thread(target=serial_reader, daemon=True).start(); started = True

@APP.before_request
def ensure_started(): start_once()

@APP.route('/')
def index(): return APP.send_static_file('startmenue_preview.html')

@APP.get('/api/system/status')
def system_status(): return jsonify({'mode':runtime['mode'],'current_session_id':runtime['current_session_id'],'current_mapping_mode':runtime['current_mapping_mode'],'inventory_direction':runtime['inventory_direction'],'serial_connected':bool(ser and ser.is_open),'latest':runtime['latest']})
@APP.get('/api/hardware/status')
def hardware_status(): return jsonify(runtime['latest'])

@APP.post('/api/hardware/mode')
def hardware_mode():
    payload = request.get_json(force=True); mode = payload.get('mode', 'hardware_test'); runtime['mode'] = 'HARDWARE_TEST'; send({'cmd':'set_mode','mode':mode}); return jsonify({'ok':True,'runtime_mode':runtime['mode'],'device_mode':mode})
@APP.post('/api/hardware/command')
def hardware_command(): payload = request.get_json(force=True); ok = send(payload); return jsonify({'ok':ok,'sent':payload})
@APP.post('/api/hardware/belt/start')
def belt_start():
    ok1 = send({'cmd':'actuator','name':'motor_main','state':1})
    ok2 = send({'cmd':'actuator','name':'motor_feed','state':1})
    return jsonify({'ok':ok1 and ok2})
@APP.post('/api/hardware/belt/stop')
def belt_stop():
    ok1 = send({'cmd':'actuator','name':'motor_main','state':0})
    ok2 = send({'cmd':'actuator','name':'motor_feed','state':0})
    return jsonify({'ok':ok1 and ok2})
@APP.post('/api/hardware/emergency-stop')
def emergency_stop(): runtime['mode'] = 'ERROR'; return jsonify({'ok':send({'cmd':'emergency_stop'}),'runtime_mode':runtime['mode']})
@APP.post('/api/hardware/relay')
def relay():
    payload = request.get_json(force=True); return jsonify({'ok':send({'cmd':'actuator','name':payload['name'],'state':int(payload['state'])})})

@APP.get('/api/mapping/list')
def mapping_list():
    with db() as c: rows = c.execute('SELECT uid, duck_number, active, created_at, updated_at FROM mappings ORDER BY duck_number').fetchall()
    return jsonify([dict(r) for r in rows])
@APP.post('/api/mapping/mode')
def mapping_mode():
    payload = request.get_json(force=True); mode = payload.get('mode', 'idle'); runtime['mode'] = 'MAPPING_ACTIVE' if mode != 'idle' else 'IDLE'; runtime['current_mapping_mode'] = mode; return jsonify({'ok':True,'mode':runtime['mode'],'mapping_mode':runtime['current_mapping_mode']})
@APP.post('/api/mapping/assign')
def mapping_assign(): payload = request.get_json(force=True); return jsonify(assign_mapping(payload['uid'], int(payload['duck_number']), bool(payload.get('overwrite', False))))
@APP.post('/api/mapping/reset')
def mapping_reset(): clear_all_mappings(); return jsonify({'ok':True})
@APP.get('/api/mapping/export.csv')
def mapping_export_csv():
    with db() as c: rows = c.execute('SELECT uid, duck_number FROM mappings ORDER BY duck_number').fetchall()
    output = io.StringIO(); writer = csv.writer(output); writer.writerow(['UID','Nummer'])
    for r in rows: writer.writerow([r['uid'], r['duck_number']])
    return APP.response_class(output.getvalue(), mimetype='text/csv')

@APP.post('/api/inventory/start')
def inventory_start():
    payload = request.get_json(silent=True) or {}
    expected_total = int(payload.get('expected_total', 0) or 0)
    runtime['mode'] = 'INVENTORY_RUNNING'
    with db() as c:
        c.execute('INSERT INTO inventory_sessions(mode, status, expected_total) VALUES (?, ?, ?)', ('inventory','running', expected_total)); runtime['current_session_id'] = c.execute('SELECT last_insert_rowid()').fetchone()[0]; c.execute("UPDATE box_state SET count = 0, state = 'open', updated_at = ?", (now_iso(),))
    send({'cmd':'set_mode','mode':'inventory'}); send({'cmd':'actuator','name':'motor_main','state':1}); send({'cmd':'actuator','name':'motor_feed','state':1}); return jsonify({'ok':True,'session_id':runtime['current_session_id'],'expected_total':expected_total})
@APP.post('/api/inventory/pause')
def inventory_pause():
    runtime['mode'] = 'INVENTORY_PAUSED'; send({'cmd':'actuator','name':'motor_main','state':0}); send({'cmd':'actuator','name':'motor_feed','state':0})
    with db() as c: c.execute('UPDATE inventory_sessions SET status = ? WHERE id = ?', ('paused', runtime['current_session_id']))
    return jsonify({'ok':True})
@APP.post('/api/inventory/resume')
def inventory_resume():
    runtime['mode'] = 'INVENTORY_RUNNING'; send({'cmd':'actuator','name':'motor_main','state':1}); send({'cmd':'actuator','name':'motor_feed','state':1})
    with db() as c: c.execute('UPDATE inventory_sessions SET status = ? WHERE id = ?', ('running', runtime['current_session_id']))
    return jsonify({'ok':True})
@APP.post('/api/inventory/stop')
def inventory_stop():
    send({'cmd':'actuator','name':'motor_main','state':0}); send({'cmd':'actuator','name':'motor_feed','state':0})
    with db() as c: c.execute('UPDATE inventory_sessions SET status = ?, ended_at = ? WHERE id = ?', ('stopped', now_iso(), runtime['current_session_id']))
    sid = runtime['current_session_id']; runtime['current_session_id'] = None; runtime['mode'] = 'IDLE'; return jsonify({'ok':True,'session_id':sid})
@APP.post('/api/inventory/direction')
def inventory_direction():
    payload = request.get_json(force=True); runtime['inventory_direction'] = payload.get('direction', 'left'); return jsonify({'ok':True,'direction':runtime['inventory_direction']})
@APP.post('/api/inventory/box/reset')
def inventory_box_reset():
    payload = request.get_json(force=True); box = payload.get('box')
    with db() as c: c.execute("UPDATE box_state SET count = 0, state = 'open', updated_at = ? WHERE box_name = ?", (now_iso(), box))
    return jsonify({'ok':True,'box':box})

@APP.get('/api/inventory/summary')
def inventory_summary():
    with db() as c:
        boxes = c.execute('SELECT box_name, count, capacity, state FROM box_state ORDER BY box_name').fetchall()
        session = None; counts = {'mapped':0,'unmapped':0,'duplicate':0,'defective':0,'total':0,'expected_total':0}
        if runtime['current_session_id']:
            session = c.execute('SELECT id, started_at, ended_at, mode, status, expected_total FROM inventory_sessions WHERE id = ?', (runtime['current_session_id'],)).fetchone()
            agg = c.execute("SELECT result, COUNT(*) AS c FROM inventory_scans WHERE session_id = ? GROUP BY result", (runtime['current_session_id'],)).fetchall()
            for row in agg: counts[row['result']] = row['c']
            counts['total'] = counts.get('mapped',0) + counts.get('unmapped',0) + counts.get('duplicate',0) + counts.get('defective',0)
            counts['expected_total'] = session['expected_total'] if session else 0
        recent = c.execute('SELECT id, uid, duck_number, result, box, timestamp FROM inventory_scans ORDER BY id DESC LIMIT 20').fetchall()
    return jsonify({'runtime_mode':runtime['mode'],'direction':runtime['inventory_direction'],'session':dict(session) if session else None,'boxes':[dict(r) for r in boxes],'counts':counts,'recent_scans':[dict(r) for r in recent]})
@APP.get('/api/inventory/export.csv')
def inventory_export_csv():
    sid = request.args.get('session_id', runtime['current_session_id'])
    with db() as c: rows = c.execute('SELECT uid, duck_number, result, box, timestamp FROM inventory_scans WHERE session_id = ? ORDER BY id', (sid,)).fetchall()
    output = io.StringIO(); writer = csv.writer(output); writer.writerow(['Zeit','UID','Nummer','Status','Kiste'])
    for r in rows: writer.writerow([r['timestamp'], r['uid'], r['duck_number'] or '', r['result'], r['box']])
    return APP.response_class(output.getvalue(), mimetype='text/csv', headers={'Content-Disposition': f'attachment; filename=inventur_{sid}.csv'})
@APP.get('/api/events/recent')
def events_recent():
    limit = int(request.args.get('limit', 50))
    with db() as c: rows = c.execute('SELECT id, source, level, event_type, payload, timestamp FROM system_events ORDER BY id DESC LIMIT ?', (limit,)).fetchall()
    return jsonify([dict(r) for r in rows])

if __name__ == '__main__': APP.run(host='0.0.0.0', port=5000, debug=True)
