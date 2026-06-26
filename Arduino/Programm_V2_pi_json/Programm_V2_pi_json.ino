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
const unsigned long KLOEPPEL_DELAY = 640;
const int POS_REST_LEFT = 990;
const int POS_KICK_LEFT = 1330;
const int POS_KICK_RIGHT = 1690;
const int POS_REST_RIGHT = 2030;
const unsigned long TIME_KICK = 280;
const unsigned long TIME_RETURN_NORMAL = 280;
const unsigned long TIME_RETURN_SWITCH = 150;
const unsigned long BAND1_DELAY_MS = 2000;
const unsigned long FUZZ_FILTER_MS = 25;
const unsigned long DUCK_BLIND_MS = 400;
const unsigned long QUEUE_TIMEOUT_MS = 2000;
const unsigned long RFID_DUP_MS = 1000;

#define MAX_DUCKS 6
#define UID_HEX_LEN 15

enum BeltState { STOPPED, RUNNING, CLEARING };
BeltState currentBeltState = STOPPED;

struct Duck {
  unsigned long ls1Time;
  unsigned long ls2Time;
  char uidHex[UID_HEX_LEN];
  uint8_t flags;
};

Duck queue[MAX_DUCKS];

bool nfcAktiv = false;
bool restPositionIsLeft = true;
bool autoSortEnabled = true;
bool serviceMode = true;
uint8_t badDucksCount = 0;
uint8_t goodDucksCount = 0;
uint8_t pwmFastCurrent = PWM_FAST_DEFAULT;
uint8_t pwmSlowCurrent = PWM_SLOW_DEFAULT;
unsigned long pauseStartTime = 0;
unsigned long band1StopTime = 0;

uint8_t lastUID[7];
uint8_t lastUIDLen = 0;
unsigned long lastUIDTime = 0;

uint8_t stateLS1 = 0;
uint8_t stateLS2 = 0;
uint8_t servoState = 0;
unsigned long timerLS1 = 0;
unsigned long timerLS2 = 0;
unsigned long servoMoveTimer = 0;
unsigned long servoReturnWaitTime = TIME_RETURN_NORMAL;

char serialBuffer[96];
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
void processQueue();
void handleServoStateMachine();
void processAutoDecision(uint8_t i);

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
  Serial.print('"'); Serial.print(k); Serial.print(F("\":")); Serial.print(v); if (comma) Serial.print(',');
}
void jsonKVBool(const __FlashStringHelper* k, bool v, bool comma = true) {
  Serial.print('"'); Serial.print(k); Serial.print(F("\":")); Serial.print(v ? F("true") : F("false")); if (comma) Serial.print(',');
}
void jsonBegin() { Serial.print('{'); }
void jsonEnd() { Serial.println('}'); }

const char* beltStateToString(BeltState s) {
  if (s == STOPPED) return "STOPPED";
  if (s == RUNNING) return "RUNNING";
  if (s == CLEARING) return "CLEARING";
  return "UNKNOWN";
}

void emitBoot(const char* detail) {
  jsonBegin();
  jsonKV(F("type"), "boot");
  jsonKV(F("status"), "ok");
  jsonKV(F("detail"), detail);
  jsonKVBool(F("nfc_active"), nfcAktiv);
  jsonKVBool(F("auto_sort_enabled"), autoSortEnabled, false);
  jsonEnd();
}
void emitMachine(const char* eventName) {
  jsonBegin();
  jsonKV(F("type"), "machine");
  jsonKV(F("event"), eventName);
  jsonKV(F("belt_state"), beltStateToString(currentBeltState));
  jsonKVNum(F("good_count"), (long)goodDucksCount);
  jsonKVNum(F("bad_count"), (long)badDucksCount);
  jsonKVBool(F("rest_left"), restPositionIsLeft);
  jsonKVNum(F("servo_state"), (long)servoState);
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
void emitRFID(const char* uid, int slot) {
  jsonBegin();
  jsonKV(F("type"), "rfid");
  jsonKV(F("uid"), uid);
  jsonKV(F("status"), "ok");
  jsonKVNum(F("slot"), (long)slot, false);
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
  for (uint8_t i = 0; i < uidLen && pos + 2 < outSize; i++) { out[pos++] = hexmap[(uid[i] >> 4) & 0x0F]; out[pos++] = hexmap[uid[i] & 0x0F]; }
  out[pos] = '\0';
}

void setMotorFast(uint8_t pwm) { pwmFastCurrent = pwm; analogWrite(PIN_MOTOR_SCHNELL, pwm); emitActuator("motor_fast", pwm > 0, pwm); }
void setMotorSlow(uint8_t pwm) { pwmSlowCurrent = pwm; analogWrite(PIN_MOTOR_LANGSAM, pwm); emitActuator("motor_slow", pwm > 0, pwm); }
void setRelay(uint8_t pin, const char* name, uint8_t state) { digitalWrite(pin, state ? HIGH : LOW); emitActuator(name, state ? 1 : 0, state ? 1 : 0); }

void stopBelts() { setMotorFast(0); setMotorSlow(0); if (currentBeltState != STOPPED) pauseStartTime = millis(); currentBeltState = STOPPED; emitMachine("belts_stopped"); }
void startBelts() { setMotorFast(pwmFastCurrent); setMotorSlow(pwmSlowCurrent); currentBeltState = RUNNING; emitMachine("belts_started"); }
void resumeBelts() { if (pauseStartTime > 0) { unsigned long pausedDuration = millis() - pauseStartTime; for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (qActive(i) && qLS2(i)) queue[i].ls2Time += pausedDuration; if (qActive(i) && !qLS2(i)) queue[i].ls1Time += pausedDuration; } pauseStartTime = 0; } goodDucksCount = 0; startBelts(); emitMachine("belts_resumed"); }
void emergencyStop() { stopBelts(); for (uint8_t i = 0; i < MAX_DUCKS; i++) queue[i].flags = 0; myServo.detach(); servoState = 0; badDucksCount = 0; goodDucksCount = 0; emitMachine("emergency_stop"); }
void servoRest(bool left) { restPositionIsLeft = left; myServo.attach(PIN_SERVO); myServo.write(left ? POS_REST_LEFT : POS_REST_RIGHT); delay(250); myServo.detach(); emitActuator("servo", 1, left ? POS_REST_LEFT : POS_REST_RIGHT); }
void servoKickToTarget(const char* target) { bool left = strcmp(target, "left") == 0; myServo.attach(PIN_SERVO); if (left) { myServo.write(POS_REST_RIGHT); delay(60); myServo.write(POS_KICK_LEFT); delay(TIME_KICK); myServo.write(POS_REST_RIGHT); delay(TIME_RETURN_NORMAL); restPositionIsLeft = false; } else { myServo.write(POS_REST_LEFT); delay(60); myServo.write(POS_KICK_RIGHT); delay(TIME_KICK); myServo.write(POS_REST_LEFT); delay(TIME_RETURN_NORMAL); restPositionIsLeft = true; } myServo.detach(); emitActuator("servo", 1, left ? POS_KICK_LEFT : POS_KICK_RIGHT); }

void cleanupQueue() { for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (qActive(i) && !qLS2(i) && millis() - queue[i].ls1Time > QUEUE_TIMEOUT_MS) { queue[i].flags = 0; emitError("QUEUE_TIMEOUT"); } } }
void processAutoDecision(uint8_t i) { bool hadRFID = qHasRFID(i); queue[i].flags = 0; if (hadRFID) { goodDucksCount++; emitMachine("good_duck_passed"); if (goodDucksCount >= 3 && currentBeltState == RUNNING) { setMotorSlow(0); currentBeltState = CLEARING; band1StopTime = millis() + BAND1_DELAY_MS; emitMachine("good_limit_reached_clearing"); } } else { badDucksCount++; servoState = 1; servoMoveTimer = millis(); myServo.attach(PIN_SERVO); myServo.write(restPositionIsLeft ? POS_KICK_RIGHT : POS_KICK_LEFT); emitMachine("bad_duck_kick_started"); } }
void processQueue() { if (!autoSortEnabled || servoState != 0) return; for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (qActive(i) && qLS2(i) && millis() - queue[i].ls2Time >= KLOEPPEL_DELAY) { processAutoDecision(i); break; } } }
void handleServoStateMachine() { if (servoState == 1 && millis() - servoMoveTimer >= TIME_KICK) { servoState = 2; servoMoveTimer = millis(); if (badDucksCount >= 3) { badDucksCount = 0; restPositionIsLeft = !restPositionIsLeft; servoReturnWaitTime = TIME_RETURN_SWITCH; } else { servoReturnWaitTime = TIME_RETURN_NORMAL; } myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT); emitMachine("servo_return_started"); } else if (servoState == 2 && millis() - servoMoveTimer >= servoReturnWaitTime) { myServo.detach(); servoState = 0; emitMachine("servo_cycle_done"); } }

void checkLS1() {
  bool currentLS1 = digitalRead(PIN_LS1);
  if (stateLS1 == 0) {
    if (currentLS1 == LOW) { stateLS1 = 1; timerLS1 = millis(); emitSensor("ls1", 0, "candidate_low"); }
  } else if (stateLS1 == 1) {
    if (currentLS1 == HIGH) { stateLS1 = 0; emitSensor("ls1", 1, "fuzz_rejected"); }
    else if (millis() - timerLS1 >= FUZZ_FILTER_MS) {
      stateLS1 = 2;
      for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (!qActive(i)) { queue[i].flags = F_ACTIVE; queue[i].ls1Time = timerLS1; queue[i].ls2Time = 0; queue[i].uidHex[0] = '\0'; break; } }
      emitSensor("ls1", 0, "duck_confirmed");
    }
  } else if (stateLS1 == 2) {
    if (millis() - timerLS1 >= DUCK_BLIND_MS && currentLS1 == HIGH) { stateLS1 = 0; emitSensor("ls1", 1, "ready_again"); }
  }
}

void checkLS2() {
  bool currentLS2 = digitalRead(PIN_LS2);
  if (stateLS2 == 0) {
    if (currentLS2 == LOW) { stateLS2 = 1; timerLS2 = millis(); emitSensor("ls2", 0, "candidate_low"); }
  } else if (stateLS2 == 1) {
    if (currentLS2 == HIGH) { stateLS2 = 0; emitSensor("ls2", 1, "fuzz_rejected"); }
    else if (millis() - timerLS2 >= FUZZ_FILTER_MS) {
      stateLS2 = 2;
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      for (uint8_t i = 0; i < MAX_DUCKS; i++) {
        if (qActive(i) && !qLS2(i)) {
          unsigned long travelTime = timerLS2 - queue[i].ls1Time;
          if (travelTime >= 150 && travelTime <= 1200 && queue[i].ls1Time < oldest) { oldest = queue[i].ls1Time; targetIdx = i; }
        }
      }
      if (targetIdx != -1) { qSet(targetIdx, F_LS2, true); queue[targetIdx].ls2Time = timerLS2; } else { emitError("LS2_ORPHAN"); }
      emitSensor("ls2", 0, "duck_confirmed");
    }
  } else if (stateLS2 == 2) {
    if (millis() - timerLS2 >= DUCK_BLIND_MS && currentLS2 == HIGH) { stateLS2 = 0; emitSensor("ls2", 1, "ready_again"); }
  }
}

void checkRFID() {
  bool duckWaiting = false;
  for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (qActive(i) && !qHasRFID(i) && !qLS2(i)) { duckWaiting = true; break; } }
  if (!duckWaiting) return;
  uint8_t uid[7], uidLen;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 45)) {
    bool isDuplicate = false;
    if (uidLen == lastUIDLen && millis() - lastUIDTime < RFID_DUP_MS) {
      isDuplicate = true;
      for (uint8_t i = 0; i < uidLen; i++) { if (uid[i] != lastUID[i]) { isDuplicate = false; break; } }
    }
    if (!isDuplicate) {
      char uidHex[UID_HEX_LEN];
      uidToHex(uid, uidLen, uidHex, sizeof(uidHex));
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      for (uint8_t i = 0; i < MAX_DUCKS; i++) { if (qActive(i) && !qHasRFID(i) && !qLS2(i) && queue[i].ls1Time < oldest) { oldest = queue[i].ls1Time; targetIdx = i; } }
      if (targetIdx != -1) { qSet(targetIdx, F_HAS_RFID, true); strncpy(queue[targetIdx].uidHex, uidHex, UID_HEX_LEN - 1); queue[targetIdx].uidHex[UID_HEX_LEN - 1] = '\0'; emitRFID(uidHex, targetIdx); }
      lastUIDLen = uidLen; for (uint8_t i = 0; i < uidLen; i++) lastUID[i] = uid[i]; lastUIDTime = millis();
    }
  }
}

bool extractStringValue(const char* src, const char* key, char* out, size_t outSize) {
  char pattern[18]; snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  char* p = strstr((char*)src, pattern); if (!p) return false; p += strlen(pattern);
  size_t i = 0; while (*p && *p != '"' && i + 1 < outSize) out[i++] = *p++; out[i] = '\0'; return true;
}
bool extractIntValue(const char* src, const char* key, long* out) { char pattern[18]; snprintf(pattern, sizeof(pattern), "\"%s\":", key); char* p = strstr((char*)src, pattern); if (!p) return false; p += strlen(pattern); *out = atol(p); return true; }
bool extractBoolValue(const char* src, const char* key, bool* out) { char pattern[22]; snprintf(pattern, sizeof(pattern), "\"%s\":", key); char* p = strstr((char*)src, pattern); if (!p) return false; p += strlen(pattern); if (!strncmp(p, "true", 4)) { *out = true; return true; } if (!strncmp(p, "false", 5)) { *out = false; return true; } return false; }

void handleJsonCommand(char* line) {
  char cmd[16] = "";
  if (!extractStringValue(line, "cmd", cmd, sizeof(cmd))) { emitError("CMD_MISSING"); return; }
  if (!strcmp(cmd, "emergency_stop")) { emergencyStop(); return; }
  if (!strcmp(cmd, "belt")) { char action[10] = ""; if (!extractStringValue(line, "action", action, sizeof(action))) { emitError("BELT_ACTION"); return; } if (!strcmp(action, "start")) startBelts(); else if (!strcmp(action, "stop")) stopBelts(); else if (!strcmp(action, "resume")) resumeBelts(); else emitError("BELT_ACTION"); return; }
  if (!strcmp(cmd, "actuator")) { char name[14] = ""; long state = 0, value = 0; extractStringValue(line, "name", name, sizeof(name)); extractIntValue(line, "state", &state); extractIntValue(line, "value", &value); if (!strcmp(name, "motor_fast")) setMotorFast(value > 0 ? (uint8_t)value : (state ? pwmFastCurrent : 0)); else if (!strcmp(name, "motor_slow")) setMotorSlow(value > 0 ? (uint8_t)value : (state ? pwmSlowCurrent : 0)); else if (!strcmp(name, "relay1")) setRelay(PIN_RELAIS1, "relay1", state ? 1 : 0); else if (!strcmp(name, "relay2")) setRelay(PIN_RELAIS2, "relay2", state ? 1 : 0); else if (!strcmp(name, "led")) { digitalWrite(PIN_LED, state ? HIGH : LOW); emitActuator("led", state ? 1 : 0, state ? 1 : 0); } else emitError("ACT_UNKNOWN"); return; }
  if (!strcmp(cmd, "servo_rest")) { char side[8] = "left"; extractStringValue(line, "side", side, sizeof(side)); servoRest(!strcmp(side, "left")); return; }
  if (!strcmp(cmd, "servo_kick")) { char target[8] = "right"; extractStringValue(line, "target", target, sizeof(target)); servoKickToTarget(target); return; }
  if (!strcmp(cmd, "set_config")) { bool b; long v; if (extractBoolValue(line, "auto_sort_enabled", &b)) autoSortEnabled = b; if (extractIntValue(line, "pwm_fast", &v)) pwmFastCurrent = constrain(v, 0, 255); if (extractIntValue(line, "pwm_slow", &v)) pwmSlowCurrent = constrain(v, 0, 255); if (extractStringValue(line, "service_mode", serialBuffer, sizeof(serialBuffer))) serviceMode = !strcmp(serialBuffer, "1") || !strcmp(serialBuffer, "true"); emitMachine("config_updated"); return; }
  if (!strcmp(cmd, "ping")) { emitMachine("pong"); return; }
  emitError("CMD_UNKNOWN");
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (serialPos > 0) { serialBuffer[serialPos] = '\0'; handleJsonCommand(serialBuffer); serialPos = 0; } }
    else { if (serialPos < sizeof(serialBuffer) - 1) serialBuffer[serialPos++] = c; else { serialPos = 0; emitError("SER_OVERFLOW"); } }
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
  digitalWrite(PIN_LED, LOW); digitalWrite(PIN_RELAIS1, LOW); digitalWrite(PIN_RELAIS2, LOW);
  analogWrite(PIN_MOTOR_SCHNELL, 0); analogWrite(PIN_MOTOR_LANGSAM, 0);
  for (uint8_t i = 0; i < MAX_DUCKS; i++) { queue[i].flags = 0; queue[i].uidHex[0] = '\0'; }
  myServo.attach(PIN_SERVO); myServo.write(POS_REST_LEFT); delay(500); myServo.detach();
  Wire.begin(); Wire.beginTransmission(0x24); byte error = Wire.endTransmission();
  if (error == 0) { nfc.begin(); uint32_t versiondata = nfc.getFirmwareVersion(); if (versiondata) { nfc.SAMConfig(); nfcAktiv = true; emitBoot("pn532_ready"); } else { emitBoot("pn532_no_fw"); } } else { emitBoot("no_i2c_0x24"); }
  stopBelts(); emitMachine("setup_complete");
}

void loop() {
  handleSerial();
  if (currentBeltState == CLEARING && millis() >= band1StopTime) { setMotorFast(0); currentBeltState = STOPPED; pauseStartTime = millis(); emitMachine("clearing_complete_stopped"); }
  checkLS1();
  if (nfcAktiv) checkRFID();
  checkLS2();
  if (serviceMode || currentBeltState == RUNNING || currentBeltState == CLEARING) { cleanupQueue(); processQueue(); }
  handleServoStateMachine();
}