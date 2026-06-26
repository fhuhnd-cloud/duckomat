from flask import Flask, request, jsonify
import serial, threading, time, json
from datetime import datetime

APP = Flask(__name__, static_folder='.', static_url_path='')
SERIAL_PORT = '/dev/ttyUSB0'
BAUDRATE = 115200
ser = None
lock = threading.Lock()
started = False

runtime = {
    'serial_connected': False,
    'latest': {
        'boot': None,
        'sensors': {},
        'actuators': {},
        'machine': None,
        'rfid': None,
        'last_uid': None,
        'errors': []
    }
}

def now_iso():
    return datetime.utcnow().isoformat(timespec='seconds') + 'Z'

def send(msg):
    data = (json.dumps(msg, separators=(',', ':')) + '
').encode('utf-8')
    with lock:
        if ser and ser.is_open:
            ser.write(data)
            ser.flush()
            return True
    return False

def norm_machine(evt):
    return {
        'type': 'machine',
        'event': evt.get('event'),
        'belt_state': evt.get('belt_state', evt.get('belt')),
        'belt': evt.get('belt', evt.get('belt_state')),
        'good_count': evt.get('good_count', evt.get('good', 0)),
        'good': evt.get('good', evt.get('good_count', 0)),
        'bad_count': evt.get('bad_count', evt.get('bad', 0)),
        'bad': evt.get('bad', evt.get('bad_count', 0)),
        'rest_left': evt.get('rest_left', evt.get('left')),
        'left': evt.get('left', evt.get('rest_left')),
        'servo_state': evt.get('servo_state', 0),
        'pwm_fast': evt.get('pwm_fast', 0),
        'pwm_slow': evt.get('pwm_slow', 0),
        'auto_sort_enabled': evt.get('auto_sort_enabled'),
        'ts': now_iso()
    }

def handle_evt(evt):
    t = evt.get('type')
    if t == 'boot':
        runtime['latest']['boot'] = evt
    elif t == 'sensor':
        runtime['latest']['sensors'][evt.get('name', 'unknown')] = {'name': evt.get('name'), 'state': evt.get('state'), 'event': evt.get('event'), 'ts': now_iso()}
    elif t == 'actuator':
        runtime['latest']['actuators'][evt.get('name', 'unknown')] = evt
    elif t == 'machine':
        runtime['latest']['machine'] = norm_machine(evt)
    elif t == 'rfid':
        runtime['latest']['rfid'] = {'uid': evt.get('uid'), 'status': evt.get('status', 'ok'), 'slot': evt.get('slot'), 'ts': now_iso()}
        runtime['latest']['last_uid'] = evt.get('uid')
    elif t == 'error':
        runtime['latest']['errors'].append(evt)
        runtime['latest']['errors'] = runtime['latest']['errors'][-50:]

def serial_reader():
    while True:
        try:
            if ser and ser.is_open and ser.in_waiting:
                line = ser.readline().decode('utf-8', 'ignore').strip()
                if line:
                    try:
                        handle_evt(json.loads(line))
                    except Exception:
                        pass
            runtime['serial_connected'] = bool(ser and ser.is_open)
        except Exception as exc:
            runtime['latest']['errors'].append({'type':'error','code':'SERIAL_READER','message':str(exc),'ts':now_iso()})
            runtime['latest']['errors'] = runtime['latest']['errors'][-50:]
        time.sleep(0.02)

def start_once():
    global ser, started
    if started:
        return
    try:
        ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
        runtime['serial_connected'] = True
    except Exception as exc:
        runtime['latest']['errors'].append({'type':'error','code':'SERIAL_OPEN','message':str(exc),'ts':now_iso()})
    threading.Thread(target=serial_reader, daemon=True).start()
    started = True

@APP.before_request
def ensure_started():
    start_once()

@APP.route('/')
def index():
    return APP.send_static_file('startmenue_preview.html')

@APP.get('/api/hardware/status')
def hardware_status():
    return jsonify({'serial_connected': runtime['serial_connected'], **runtime['latest']})

@APP.post('/api/hardware/ping')
def hardware_ping():
    return jsonify({'ok': send({'cmd':'ping'})})

@APP.post('/api/hardware/belt/start')
def belt_start():
    return jsonify({'ok': send({'cmd':'belt','action':'start'})})

@APP.post('/api/hardware/belt/stop')
def belt_stop():
    return jsonify({'ok': send({'cmd':'belt','action':'stop'})})

@APP.post('/api/hardware/belt/resume')
def belt_resume():
    return jsonify({'ok': send({'cmd':'belt','action':'resume'})})

@APP.post('/api/hardware/emergency-stop')
def emergency_stop():
    return jsonify({'ok': send({'cmd':'emergency_stop'}), 'runtime_mode':'ERROR'})

@APP.post('/api/hardware/relay')
def relay():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({'cmd':'actuator','name':payload['name'],'state':int(payload['state'])})})

@APP.post('/api/hardware/motor')
def motor():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({'cmd':'actuator','name':payload['name'],'state':int(payload.get('state',0)),'value':int(payload.get('value',0))})})

@APP.post('/api/hardware/servo/rest')
def servo_rest():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({'cmd':'servo_rest','side':payload.get('side','left')})})

@APP.post('/api/hardware/servo/kick')
def servo_kick():
    payload = request.get_json(force=True)
    return jsonify({'ok': send({'cmd':'servo_kick','target':payload.get('target','right')})})

@APP.post('/api/hardware/config')
def config():
    payload = request.get_json(force=True)
    cmd = {'cmd':'set_config'}
    if 'auto_sort_enabled' in payload:
        cmd['auto_sort_enabled'] = bool(payload['auto_sort_enabled'])
    if 'pwm_fast' in payload:
        cmd['pwm_fast'] = int(payload['pwm_fast'])
    if 'pwm_slow' in payload:
        cmd['pwm_slow'] = int(payload['pwm_slow'])
    return jsonify({'ok': send(cmd), 'sent': cmd})

if __name__ == '__main__':
    APP.run(host='0.0.0.0', port=5000, debug=True)
