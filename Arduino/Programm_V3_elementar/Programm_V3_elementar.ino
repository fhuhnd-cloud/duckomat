#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>

#define PIN_LS1 2
#define PIN_LS2 3
#define PIN_SERVO 5
#define PIN_LED 6
#define PN532_RESET 7
#define PN532_IRQ 8
#define PIN_MOTOR_SCHNELL 9
#define PIN_MOTOR_LANGSAM 10
#define PIN_RELAIS1 11
#define PIN_RELAIS2 12

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
ServoTimer2 myServo;

const uint8_t PWM_FAST_DEFAULT = 255;
const uint8_t PWM_SLOW_DEFAULT = 100;
const int POS_REST_LEFT = 990;
const int POS_KICK_LEFT = 1330;
const int POS_KICK_RIGHT = 1690;
const int POS_REST_RIGHT = 2030;

const unsigned long FUZZ_FILTER_MS = 25;
const unsigned long DUCK_BLIND_MS = 400;
const unsigned long RFID_DUP_MS = 1000;
const unsigned long RFID_FREE_COOLDOWN_MS = 250;
const unsigned long QUEUE_TIMEOUT_MS = 2500;

#define MAX_DUCKS 6
#define UID_HEX_LEN 15

enum BeltState { STOPPED, RUNNING };
enum RFIDMode { RFID_OFF, RFID_FREE, RFID_QUEUE_AFTER_LS1 };

BeltState currentBeltState = STOPPED;
RFIDMode currentRFIDMode = RFID_FREE;

struct Duck {
  unsigned long ls1Time;
  unsigned long ls2Time;
  char uidHex[UID_HEX_LEN];
  uint8_t flags;
};

Duck queue[MAX_DUCKS];

bool nfcAktiv = false;
bool autoSortEnabled = false;
bool serviceMode = true;
bool restPositionIsLeft = true;

uint8_t pwmFastCurrent = PWM_FAST_DEFAULT;
uint8_t pwmSlowCurrent = PWM_SLOW_DEFAULT;

uint8_t stateLS1 = 0;
uint8_t stateLS2 = 0;
unsigned long timerLS1 = 0;
unsigned long timerLS2 = 0;

uint8_t lastUID[7];
uint8_t lastUIDLen = 0;
unsigned long lastUIDTime = 0;
unsigned long lastFreeReadTime = 0;

char serialBuffer[160];
uint8_t serialPos = 0;

#define F_ACTIVE 0x01
#define F_HAS_RFID 0x02
#define F_LS2 0x04

bool qActive(uint8_t i) { return queue[i].flags & F_ACTIVE; }
bool qHasRFID(uint8_t i) { return queue[i].flags & F_HAS_RFID; }
bool qLS2(uint8_t i) { return queue[i].flags & F_LS2; }
void qSet(uint8_t i, uint8_t mask, bool on) { if (on) queue[i].flags |= mask; else queue[i].flags &= ~mask; }

void handleSerial();
void handleJsonCommand(char* line);
void checkLS1();
void checkLS2();
void checkRFID();
void cleanupQueue();

void jsonKV(const __FlashStringHelper* k, const char* v, bool comma = true) {
  Serial.print('"'); Serial.print(k); Serial.print(F("\":"));
  Serial.print('"');
  while (*v) {
    if (*v == '"' || *v == '\\') Serial.print('\\');
    Serial.print(*v++);
  }
  Serial.print('"');
  if (comma) Serial.print(',');
}

void jsonKVNum(const __FlashStringHelper* k, long v, bool comma = true) {
  Serial.print('"'); Serial.print(k); Serial.print(F("\":"));
  Serial.print(v);
  if (comma) Serial.print(',');
}

void jsonKVBool(const __FlashStringHelper* k, bool v, bool comma = true) {
  Serial.print('"'); Serial.print(k); Serial.print(F("\":"));
  Serial.print(v ? F("true") : F("false"));
  if (comma) Serial.print(',');
}

void jsonBegin() { Serial.print('{'); }
void jsonEnd() { Serial.println('}'); }

const char* beltStateToString(BeltState s) {
  if (s == STOPPED) return "STOPPED";
  if (s == RUNNING) return "RUNNING";
  return "UNKNOWN";
}

const char* rfidModeToString(RFIDMode m) {
  if (m == RFID_OFF) return "off";
  if (m == RFID_FREE) return "free";
  if (m == RFID_QUEUE_AFTER_LS1) return "queue_after_ls1";
  return "unknown";
}

uint8_t activeQueueCount() {
  uint8_t cnt = 0;
  for (uint8_t i = 0; i < MAX_DUCKS; i++) if (qActive(i)) cnt++;
  return cnt;
}

void emitBoot(const char* detail) {
  jsonBegin();
  jsonKV(F("type"), "boot");
  jsonKV(F("status"), "ok");
  jsonKV(F("detail"), detail);
  jsonKVBool(F("nfc_active"), nfcAktiv);
  jsonKV(F("rfid_mode"), rfidModeToString(currentRFIDMode));
  jsonKVBool(F("auto_sort_enabled"), autoSortEnabled, false);
  jsonEnd();
}

void emitMachine(const char* eventName) {
  jsonBegin();
  jsonKV(F("type"), "machine");
  jsonKV(F("event"), eventName);
  jsonKV(F("belt_state"), beltStateToString(currentBeltState));
  jsonKV(F("rfid_mode"), rfidModeToString(currentRFIDMode));
  jsonKVNum(F("queue_active"), (long)activeQueueCount());
  jsonKVBool(F("rest_left"), restPositionIsLeft);
  jsonKVNum(F("pwm_fast"), (long)pwmFastCurrent);
  jsonKVNum(F("pwm_slow"), (long)pwmSlowCurrent);
  jsonKVBool(F("auto_sort_enabled"), autoSortEnabled, false);
  jsonEnd();
}

void emitSensor(const char* name, uint8_t state, const char* eventName) {
  jsonBegin();
  jsonKV(F("type"), "sensor");
  jsonKV(F("name"), name);
  jsonKVNum(F("state"), (long)state);
  jsonKV(F("event"), eventName, false);
  jsonEnd();
}

void emitActuator(const char* name, uint8_t state, int value) {
  jsonBegin();
  jsonKV(F("type"), "actuator");
  jsonKV(F("name"), name);
  jsonKVNum(F("state"), (long)state);
  jsonKVNum(F("value"), (long)value, false);
  jsonEnd();
}

void emitRFID(const char* uid, int slot, const char* modeName) {
  jsonBegin();
  jsonKV(F("type"), "rfid");
  jsonKV(F("uid"), uid);
  jsonKV(F("status"), "ok");
  jsonKV(F("mode"), modeName);
  jsonKVNum(F("slot"), (long)slot, false);
  jsonEnd();
}

void emitQueueEvent(const char* eventName, int slot, const char* uid, unsigned long ls1Time, unsigned long ls2Time) {
  jsonBegin();
  jsonKV(F("type"), "machine");
  jsonKV(F("event"), eventName);
  jsonKVNum(F("slot"), slot);
  jsonKV(F("uid"), (uid && uid[0]) ? uid : "");
  jsonKVNum(F("ls1_ms"), (long)ls1Time);
  jsonKVNum(F("ls2_ms"), (long)ls2Time);
  jsonKV(F("belt_state"), beltStateToString(currentBeltState));
  jsonKV(F("rfid_mode"), rfidModeToString(currentRFIDMode), false);
  jsonEnd();
}

void emitError(const char* code) {
  jsonBegin();
  jsonKV(F("type"), "error");
  jsonKV(F("code"), code, false);
  jsonEnd();
}

void uidToHex(uint8_t* uid, uint8_t uidLen, char* out, size_t outSize) {
  static const char hexmap[] = "0123456789ABCDEF";
  size_t pos = 0;
  for (uint8_t i = 0; i < uidLen && pos + 2 < outSize; i++) {
    out[pos++] = hexmap[(uid[i] >> 4) & 0x0F];
    out[pos++] = hexmap[uid[i] & 0x0F];
  }
  out[pos] = '\0';
}

bool isDuplicateUID(uint8_t* uid, uint8_t uidLen) {
  if (uidLen != lastUIDLen) return false;
  if (millis() - lastUIDTime >= RFID_DUP_MS) return false;
  for (uint8_t i = 0; i < uidLen; i++) {
    if (uid[i] != lastUID[i]) return false;
  }
  return true;
}

void rememberUID(uint8_t* uid, uint8_t uidLen) {
  lastUIDLen = uidLen;
  for (uint8_t i = 0; i < uidLen; i++) lastUID[i] = uid[i];
  lastUIDTime = millis();
}

void clearQueueSlot(uint8_t i) {
  queue[i].flags = 0;
  queue[i].ls1Time = 0;
  queue[i].ls2Time = 0;
  queue[i].uidHex[0] = '\0';
}

void setMotorFast(uint8_t pwm) {
  pwmFastCurrent = pwm;
  analogWrite(PIN_MOTOR_SCHNELL, pwm);
  emitActuator("motor_fast", pwm > 0, pwm);
}

void setMotorSlow(uint8_t pwm) {
  pwmSlowCurrent = pwm;
  analogWrite(PIN_MOTOR_LANGSAM, pwm);
  emitActuator("motor_slow", pwm > 0, pwm);
}

void setRelay(uint8_t pin, const char* name, uint8_t state) {
  digitalWrite(pin, state ? HIGH : LOW);
  emitActuator(name, state ? 1 : 0, state ? 1 : 0);
}

void stopBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);
  currentBeltState = STOPPED;
  emitMachine("belts_stopped");
}

void startBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, pwmFastCurrent);
  analogWrite(PIN_MOTOR_LANGSAM, pwmSlowCurrent);
  currentBeltState = RUNNING;
  emitMachine("belts_started");
}

void resumeBelts() {
  startBelts();
  emitMachine("belts_resumed");
}

void emergencyStop() {
  stopBelts();
  for (uint8_t i = 0; i < MAX_DUCKS; i++) clearQueueSlot(i);
  myServo.detach();
  emitMachine("emergency_stop");
}

void servoRest(bool left) {
  restPositionIsLeft = left;
  myServo.attach(PIN_SERVO);
  myServo.write(left ? POS_REST_LEFT : POS_REST_RIGHT);
  delay(250);
  myServo.detach();
  emitActuator("servo", 1, left ? POS_REST_LEFT : POS_REST_RIGHT);
  emitMachine("servo_rest_set");
}

void servoPulseDirect(int pulse) {
  pulse = constrain(pulse, POS_REST_LEFT, POS_REST_RIGHT);
  myServo.attach(PIN_SERVO);
  myServo.write(pulse);
  delay(80);
  myServo.detach();
  emitActuator("servo", 1, pulse);
  emitMachine("servo_direct_set");
}

void servoKickToTarget(const char* target) {
  bool left = strcmp(target, "left") == 0;
  myServo.attach(PIN_SERVO);

  if (left) {
    myServo.write(POS_REST_RIGHT);
    delay(60);
    myServo.write(POS_KICK_LEFT);
    delay(280);
    myServo.write(POS_REST_RIGHT);
    delay(280);
    restPositionIsLeft = false;
    emitActuator("servo", 1, POS_KICK_LEFT);
  } else {
    myServo.write(POS_REST_LEFT);
    delay(60);
    myServo.write(POS_KICK_RIGHT);
    delay(280);
    myServo.write(POS_REST_LEFT);
    delay(280);
    restPositionIsLeft = true;
    emitActuator("servo", 1, POS_KICK_RIGHT);
  }

  myServo.detach();
  emitMachine("servo_kick_done");
}

void checkLS1() {
  bool currentLS1 = digitalRead(PIN_LS1);

  if (stateLS1 == 0) {
    if (currentLS1 == LOW) {
      stateLS1 = 1;
      timerLS1 = millis();
      emitSensor("ls1", 0, "candidate_low");
    }
  } else if (stateLS1 == 1) {
    if (currentLS1 == HIGH) {
      stateLS1 = 0;
      emitSensor("ls1", 1, "fuzz_rejected");
    } else if (millis() - timerLS1 >= FUZZ_FILTER_MS) {
      stateLS1 = 2;
      bool queued = false;
      if (currentRFIDMode == RFID_QUEUE_AFTER_LS1) {
        for (uint8_t i = 0; i < MAX_DUCKS; i++) {
          if (!qActive(i)) {
            queue[i].flags = F_ACTIVE;
            queue[i].ls1Time = timerLS1;
            queue[i].ls2Time = 0;
            queue[i].uidHex[0] = '\0';
            emitQueueEvent("ls1_enqueued", i, "", queue[i].ls1Time, queue[i].ls2Time);
            queued = true;
            break;
          }
        }
        if (!queued) emitError("QUEUE_FULL");
      }
      emitSensor("ls1", 0, "duck_confirmed");
    }
  } else if (stateLS1 == 2) {
    if (millis() - timerLS1 >= DUCK_BLIND_MS && currentLS1 == HIGH) {
      stateLS1 = 0;
      emitSensor("ls1", 1, "ready_again");
    }
  }
}

void checkLS2() {
  bool currentLS2 = digitalRead(PIN_LS2);

  if (stateLS2 == 0) {
    if (currentLS2 == LOW) {
      stateLS2 = 1;
      timerLS2 = millis();
      emitSensor("ls2", 0, "candidate_low");
    }
  } else if (stateLS2 == 1) {
    if (currentLS2 == HIGH) {
      stateLS2 = 0;
      emitSensor("ls2", 1, "fuzz_rejected");
    } else if (millis() - timerLS2 >= FUZZ_FILTER_MS) {
      stateLS2 = 2;

      if (currentRFIDMode == RFID_QUEUE_AFTER_LS1) {
        int targetIdx = -1;
        unsigned long oldest = 0xFFFFFFFF;

        for (uint8_t i = 0; i < MAX_DUCKS; i++) {
          if (qActive(i) && !qLS2(i)) {
            unsigned long travelTime = timerLS2 - queue[i].ls1Time;
            if (travelTime >= 150 && travelTime <= 1500 && queue[i].ls1Time < oldest) {
              oldest = queue[i].ls1Time;
              targetIdx = i;
            }
          }
        }

        if (targetIdx != -1) {
          qSet(targetIdx, F_LS2, true);
          queue[targetIdx].ls2Time = timerLS2;
          emitQueueEvent("ls2_matched", targetIdx, queue[targetIdx].uidHex, queue[targetIdx].ls1Time, queue[targetIdx].ls2Time);
        } else {
          emitError("LS2_ORPHAN");
        }
      }

      emitSensor("ls2", 0, "duck_confirmed");
    }
  } else if (stateLS2 == 2) {
    if (millis() - timerLS2 >= DUCK_BLIND_MS && currentLS2 == HIGH) {
      stateLS2 = 0;
      emitSensor("ls2", 1, "ready_again");
    }
  }
}

void checkRFID() {
  if (!nfcAktiv || currentRFIDMode == RFID_OFF) return;

  bool shouldRead = false;

  if (currentRFIDMode == RFID_FREE) {
    if (millis() - lastFreeReadTime >= RFID_FREE_COOLDOWN_MS) {
      shouldRead = true;
    }
  } else if (currentRFIDMode == RFID_QUEUE_AFTER_LS1) {
    for (uint8_t i = 0; i < MAX_DUCKS; i++) {
      if (qActive(i) && !qHasRFID(i)) {
        shouldRead = true;
        break;
      }
    }
  }

  if (!shouldRead) return;

  uint8_t uid[7], uidLen;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 45)) return;
  if (isDuplicateUID(uid, uidLen)) return;

  char uidHex[UID_HEX_LEN];
  uidToHex(uid, uidLen, uidHex, sizeof(uidHex));
  rememberUID(uid, uidLen);

  if (currentRFIDMode == RFID_FREE) {
    lastFreeReadTime = millis();
    emitRFID(uidHex, -1, "free");
    emitMachine("rfid_free_read");
    return;
  }

  int targetIdx = -1;
  unsigned long oldest = 0xFFFFFFFF;

  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    if (qActive(i) && !qHasRFID(i) && queue[i].ls1Time < oldest) {
      oldest = queue[i].ls1Time;
      targetIdx = i;
    }
  }

  if (targetIdx != -1) {
    qSet(targetIdx, F_HAS_RFID, true);
    strncpy(queue[targetIdx].uidHex, uidHex, UID_HEX_LEN - 1);
    queue[targetIdx].uidHex[UID_HEX_LEN - 1] = '\0';
    emitRFID(uidHex, targetIdx, "queue_after_ls1");
    emitQueueEvent("rfid_matched", targetIdx, queue[targetIdx].uidHex, queue[targetIdx].ls1Time, queue[targetIdx].ls2Time);
  } else {
    emitRFID(uidHex, -1, "queue_after_ls1");
    emitError("RFID_ORPHAN");
  }
}

void cleanupQueue() {
  if (currentRFIDMode != RFID_QUEUE_AFTER_LS1) return;

  unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    if (!qActive(i)) continue;

    if (!qLS2(i) && now - queue[i].ls1Time > QUEUE_TIMEOUT_MS) {
      emitQueueEvent("queue_timeout_before_ls2", i, queue[i].uidHex, queue[i].ls1Time, queue[i].ls2Time);
      clearQueueSlot(i);
      emitError("QUEUE_TIMEOUT");
      continue;
    }

    if (qLS2(i) && now - queue[i].ls2Time > 1500) {
      emitQueueEvent("queue_done_after_ls2", i, queue[i].uidHex, queue[i].ls1Time, queue[i].ls2Time);
      clearQueueSlot(i);
    }
  }
}

bool extractStringValue(const char* src, const char* key, char* out, size_t outSize) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  char* p = strstr((char*)src, pattern);
  if (!p) return false;
  p += strlen(pattern);
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outSize) out[i++] = *p++;
  out[i] = '\0';
  return true;
}

bool extractIntValue(const char* src, const char* key, long* out) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  char* p = strstr((char*)src, pattern);
  if (!p) return false;
  p += strlen(pattern);
  *out = atol(p);
  return true;
}

bool extractBoolValue(const char* src, const char* key, bool* out) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  char* p = strstr((char*)src, pattern);
  if (!p) return false;
  p += strlen(pattern);
  if (!strncmp(p, "true", 4)) { *out = true; return true; }
  if (!strncmp(p, "false", 5)) { *out = false; return true; }
  return false;
}

RFIDMode parseRFIDMode(const char* mode) {
  if (!strcmp(mode, "off")) return RFID_OFF;
  if (!strcmp(mode, "free")) return RFID_FREE;
  if (!strcmp(mode, "queue_after_ls1")) return RFID_QUEUE_AFTER_LS1;
  return currentRFIDMode;
}

void handleJsonCommand(char* line) {
  char cmd[20] = "";
  if (!extractStringValue(line, "cmd", cmd, sizeof(cmd))) {
    emitError("CMD_MISSING");
    return;
  }

  if (!strcmp(cmd, "emergency_stop")) {
    emergencyStop();
    return;
  }

  if (!strcmp(cmd, "belt")) {
    char action[12] = "";
    if (!extractStringValue(line, "action", action, sizeof(action))) {
      emitError("BELT_ACTION");
      return;
    }
    if (!strcmp(action, "start")) startBelts();
    else if (!strcmp(action, "stop")) stopBelts();
    else if (!strcmp(action, "resume")) resumeBelts();
    else emitError("BELT_ACTION");
    return;
  }

  if (!strcmp(cmd, "actuator")) {
    char name[16] = "";
    long state = 0, value = 0;
    extractStringValue(line, "name", name, sizeof(name));
    extractIntValue(line, "state", &state);
    extractIntValue(line, "value", &value);

    if (!strcmp(name, "motor_fast")) {
      setMotorFast(value > 0 ? (uint8_t)value : (state ? pwmFastCurrent : 0));
    } else if (!strcmp(name, "motor_slow")) {
      setMotorSlow(value > 0 ? (uint8_t)value : (state ? pwmSlowCurrent : 0));
    } else if (!strcmp(name, "relay1")) {
      setRelay(PIN_RELAIS1, "relay1", state ? 1 : 0);
    } else if (!strcmp(name, "relay2")) {
      setRelay(PIN_RELAIS2, "relay2", state ? 1 : 0);
    } else if (!strcmp(name, "led")) {
      digitalWrite(PIN_LED, state ? HIGH : LOW);
      emitActuator("led", state ? 1 : 0, state ? 1 : 0);
    } else if (!strcmp(name, "servo")) {
      servoPulseDirect((int)value);
    } else {
      emitError("ACT_UNKNOWN");
    }
    return;
  }

  if (!strcmp(cmd, "servo_rest")) {
    char side[8] = "left";
    extractStringValue(line, "side", side, sizeof(side));
    servoRest(!strcmp(side, "left"));
    return;
  }

  if (!strcmp(cmd, "servo_kick")) {
    char target[8] = "right";
    extractStringValue(line, "target", target, sizeof(target));
    servoKickToTarget(target);
    return;
  }

  if (!strcmp(cmd, "set_config")) {
    bool b;
    long v;
    char mode[24] = "";

    if (extractBoolValue(line, "auto_sort_enabled", &b)) autoSortEnabled = b;
    if (extractIntValue(line, "pwm_fast", &v)) pwmFastCurrent = constrain(v, 0, 255);
    if (extractIntValue(line, "pwm_slow", &v)) pwmSlowCurrent = constrain(v, 0, 255);

    if (extractStringValue(line, "service_mode", serialBuffer, sizeof(serialBuffer))) {
      serviceMode = !strcmp(serialBuffer, "1") || !strcmp(serialBuffer, "true");
    } else if (extractBoolValue(line, "service_mode", &b)) {
      serviceMode = b;
    }

    if (extractStringValue(line, "rfid_mode", mode, sizeof(mode))) {
      currentRFIDMode = parseRFIDMode(mode);
    }

    emitMachine("config_updated");
    return;
  }

  if (!strcmp(cmd, "ping")) {
    emitMachine("pong");
    return;
  }

  emitError("CMD_UNKNOWN");
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialPos > 0) {
        serialBuffer[serialPos] = '\0';
        handleJsonCommand(serialBuffer);
        serialPos = 0;
      }
    } else {
      if (serialPos < sizeof(serialBuffer) - 1) {
        serialBuffer[serialPos++] = c;
      } else {
        serialPos = 0;
        emitError("SER_OVERFLOW");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_LS1, INPUT_PULLUP);
  pinMode(PIN_LS2, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  pinMode(PIN_RELAIS1, OUTPUT);
  pinMode(PIN_RELAIS2, OUTPUT);

  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_RELAIS1, LOW);
  digitalWrite(PIN_RELAIS2, LOW);
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);

  for (uint8_t i = 0; i < MAX_DUCKS; i++) clearQueueSlot(i);

  myServo.attach(PIN_SERVO);
  myServo.write(POS_REST_LEFT);
  delay(500);
  myServo.detach();
  restPositionIsLeft = true;

  Wire.begin();
  Wire.beginTransmission(0x24);
  byte error = Wire.endTransmission();

  if (error == 0) {
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata) {
      nfc.SAMConfig();
      nfcAktiv = true;
      emitBoot("pn532_ready");
    } else {
      emitBoot("pn532_no_fw");
    }
  } else {
    emitBoot("no_i2c_0x24");
  }

  emitSensor("ls1", digitalRead(PIN_LS1) ? 1 : 0, "init");
  emitSensor("ls2", digitalRead(PIN_LS2) ? 1 : 0, "init");
  stopBelts();
  emitMachine("setup_complete");
}

void loop() {
  handleSerial();
  checkLS1();
  checkLS2();
  checkRFID();
  cleanupQueue();
}