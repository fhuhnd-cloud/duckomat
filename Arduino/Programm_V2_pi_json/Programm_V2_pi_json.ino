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

const uint8_t pwmSchnellDefault = 255;
const uint8_t pwmLangsamDefault = 100;
const unsigned long kloeppelDelay = 640;
const int POS_REST_LEFT  = 990;
const int POS_KICK_LEFT  = 1330;
const int POS_KICK_RIGHT = 1690;
const int POS_REST_RIGHT = 2030;
const unsigned long timeKick = 280;
const unsigned long timeReturnNormal = 280;
const unsigned long timeReturnWechsel = 150;
const unsigned long BAND1_DELAY_MS = 2000;
const unsigned long FUZZ_FILTER_MS = 25;
const unsigned long DUCK_BLIND_MS = 400;
const unsigned long QUEUE_TIMEOUT_MS = 2000;
const unsigned long RFID_DUP_MS = 1000;

#define MAX_DUCKS 8
#define UID_HEX_LEN 15

enum BeltState { STOPPED, RUNNING, CLEARING };
BeltState currentBeltState = STOPPED;

bool nfcAktiv = false;
bool restPositionIsLeft = true;
bool autoSortEnabled = true;
uint8_t badDucksCount = 0;
uint8_t goodDucksCount = 0;
unsigned long pauseStartTime = 0;
unsigned long band1StopTime = 0;
uint8_t pwmSchnellAktuell = pwmSchnellDefault;
uint8_t pwmLangsamAktuell = pwmLangsamDefault;

uint8_t lastUID[7];
uint8_t lastUIDLen = 0;
unsigned long lastUIDTime = 0;
char lastUIDHex[UID_HEX_LEN] = "";

struct Duck {
  bool active;
  bool hasRFID;
  bool ls2Triggered;
  unsigned long ls1Time;
  unsigned long ls2Time;
  char uidHex[UID_HEX_LEN];
};
Duck queue[MAX_DUCKS];

uint8_t stateLS1 = 0;
unsigned long timerLS1 = 0;
uint8_t stateLS2 = 0;
unsigned long timerLS2 = 0;
uint8_t servoState = 0;
unsigned long servoMoveTimer = 0;
unsigned long servoReturnWaitTime = timeReturnNormal;

char serialBuffer[96];
uint8_t serialPos = 0;

const char S_BOOT[] PROGMEM = "boot";
const char S_MACHINE[] PROGMEM = "machine";
const char S_SENSOR[] PROGMEM = "sensor";
const char S_ACTUATOR[] PROGMEM = "actuator";
const char S_RFID[] PROGMEM = "rfid";
const char S_ERROR[] PROGMEM = "error";
const char S_QUEUE[] PROGMEM = "queue";

void printKeyValStr(const __FlashStringHelper* key, const char* val, bool comma=true) {
  Serial.print('"'); Serial.print(key); Serial.print(F("\":"));
  Serial.print('"');
  for (const char* p = val; *p; ++p) {
    if (*p == '"' || *p == '\\') Serial.print('\\');
    Serial.print(*p);
  }
  Serial.print('"');
  if (comma) Serial.print(',');
}

void printKeyValInt(const __FlashStringHelper* key, long val, bool comma=true) {
  Serial.print('"'); Serial.print(key); Serial.print(F("\":"));
  Serial.print(val);
  if (comma) Serial.print(',');
}

void printKeyValBool(const __FlashStringHelper* key, bool val, bool comma=true) {
  Serial.print('"'); Serial.print(key); Serial.print(F("\":"));
  Serial.print(val ? F("true") : F("false"));
  if (comma) Serial.print(',');
}

void jsonStart() { Serial.print('{'); }
void jsonEnd() { Serial.println('}'); }

const char* beltStateToString(BeltState s) {
  switch (s) {
    case STOPPED: return "STOPPED";
    case RUNNING: return "RUNNING";
    case CLEARING: return "CLEARING";
    default: return "UNKNOWN";
  }
}

void emitBoot(const char* status, const char* detail) {
  jsonStart();
  printKeyValStr(F("type"), S_BOOT);
  printKeyValStr(F("status"), status);
  printKeyValStr(F("detail"), detail);
  printKeyValBool(F("nfc_active"), nfcAktiv);
  printKeyValBool(F("auto_sort_enabled"), autoSortEnabled, false);
  jsonEnd();
}

void emitMachine(const char* eventName) {
  jsonStart();
  printKeyValStr(F("type"), S_MACHINE);
  printKeyValStr(F("event"), eventName);
  printKeyValStr(F("belt_state"), beltStateToString(currentBeltState));
  printKeyValInt(F("good_count"), goodDucksCount);
  printKeyValInt(F("bad_count"), badDucksCount);
  printKeyValBool(F("rest_left"), restPositionIsLeft);
  printKeyValInt(F("servo_state"), servoState);
  printKeyValInt(F("pwm_fast"), pwmSchnellAktuell);
  printKeyValInt(F("pwm_slow"), pwmLangsamAktuell);
  printKeyValBool(F("auto_sort_enabled"), autoSortEnabled, false);
  jsonEnd();
}

void emitSensor(const char* name, uint8_t state, const char* eventName) {
  jsonStart();
  printKeyValStr(F("type"), S_SENSOR);
  printKeyValStr(F("name"), name);
  printKeyValInt(F("state"), state);
  printKeyValStr(F("event"), eventName, false);
  jsonEnd();
}

void emitActuator(const char* name, uint8_t state, int value) {
  jsonStart();
  printKeyValStr(F("type"), S_ACTUATOR);
  printKeyValStr(F("name"), name);
  printKeyValInt(F("state"), state);
  printKeyValInt(F("value"), value, false);
  jsonEnd();
}

void emitError(const char* code, const char* message) {
  jsonStart();
  printKeyValStr(F("type"), S_ERROR);
  printKeyValStr(F("code"), code);
  printKeyValStr(F("message"), message, false);
  jsonEnd();
}

void emitRFID(const char* uid, const char* status, int slot) {
  jsonStart();
  printKeyValStr(F("type"), S_RFID);
  printKeyValStr(F("uid"), uid);
  printKeyValStr(F("status"), status);
  printKeyValInt(F("slot"), slot, false);
  jsonEnd();
}

void emitQueueEvent(const char* eventName, int slot, const char* uid, bool hasRFID) {
  jsonStart();
  printKeyValStr(F("type"), S_QUEUE);
  printKeyValStr(F("event"), eventName);
  printKeyValInt(F("slot"), slot);
  printKeyValStr(F("uid"), uid);
  printKeyValBool(F("has_rfid"), hasRFID, false);
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

void setMotorFast(uint8_t pwm) {
  pwmSchnellAktuell = pwm;
  analogWrite(PIN_MOTOR_SCHNELL, pwm);
  emitActuator("motor_fast", pwm > 0 ? 1 : 0, pwm);
}

void setMotorSlow(uint8_t pwm) {
  pwmLangsamAktuell = pwm;
  analogWrite(PIN_MOTOR_LANGSAM, pwm);
  emitActuator("motor_slow", pwm > 0 ? 1 : 0, pwm);
}

void setRelay(uint8_t pin, const char* name, uint8_t state) {
  digitalWrite(pin, state ? HIGH : LOW);
  emitActuator(name, state ? 1 : 0, state ? 1 : 0);
}

void stopBelts() {
  setMotorFast(0);
  setMotorSlow(0);
  if (currentBeltState != STOPPED) pauseStartTime = millis();
  currentBeltState = STOPPED;
  emitMachine("belts_stopped");
}

void startBelts() {
  setMotorFast(pwmSchnellDefault);
  setMotorSlow(pwmLangsamDefault);
  currentBeltState = RUNNING;
  emitMachine("belts_started");
}

void resumeBelts() {
  if (pauseStartTime > 0) {
    unsigned long pausedDuration = millis() - pauseStartTime;
    for (uint8_t i = 0; i < MAX_DUCKS; i++) {
      if (queue[i].active && queue[i].ls2Triggered) queue[i].ls2Time += pausedDuration;
      if (queue[i].active && !queue[i].ls2Triggered) queue[i].ls1Time += pausedDuration;
    }
    pauseStartTime = 0;
  }
  goodDucksCount = 0;
  startBelts();
  emitMachine("belts_resumed");
}

void emergencyStop() {
  stopBelts();
  for (uint8_t i = 0; i < MAX_DUCKS; i++) queue[i].active = false;
  myServo.detach();
  servoState = 0;
  badDucksCount = 0;
  goodDucksCount = 0;
  emitMachine("emergency_stop");
}

void servoRest(bool left) {
  restPositionIsLeft = left;
  myServo.attach(PIN_SERVO);
  myServo.write(left ? POS_REST_LEFT : POS_REST_RIGHT);
  delay(250);
  myServo.detach();
  emitActuator("servo", 1, left ? POS_REST_LEFT : POS_REST_RIGHT);
}

void servoKickToTarget(const char* target) {
  bool left = strcmp(target, "left") == 0;
  myServo.attach(PIN_SERVO);
  if (left) {
    myServo.write(POS_REST_RIGHT);
    delay(60);
    myServo.write(POS_KICK_LEFT);
    delay(timeKick);
    myServo.write(POS_REST_RIGHT);
    delay(timeReturnNormal);
    restPositionIsLeft = false;
  } else {
    myServo.write(POS_REST_LEFT);
    delay(60);
    myServo.write(POS_KICK_RIGHT);
    delay(timeKick);
    myServo.write(POS_REST_LEFT);
    delay(timeReturnNormal);
    restPositionIsLeft = true;
  }
  myServo.detach();
  emitActuator("servo", 1, left ? POS_KICK_LEFT : POS_KICK_RIGHT);
}

void cleanupQueue() {
  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    if (queue[i].active && !queue[i].ls2Triggered) {
      if (millis() - queue[i].ls1Time > QUEUE_TIMEOUT_MS) {
        queue[i].active = false;
        emitQueueEvent("timeout_removed", i, queue[i].uidHex, queue[i].hasRFID);
      }
    }
  }
}

void processAutoDecision(uint8_t i) {
  queue[i].active = false;
  if (queue[i].hasRFID) {
    goodDucksCount++;
    emitMachine("good_duck_passed");
    if (goodDucksCount >= 3 && currentBeltState == RUNNING) {
      setMotorSlow(0);
      currentBeltState = CLEARING;
      band1StopTime = millis() + BAND1_DELAY_MS;
      emitMachine("good_limit_reached_clearing");
    }
  } else {
    badDucksCount++;
    servoState = 1;
    servoMoveTimer = millis();
    myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT);
    myServo.attach(PIN_SERVO);
    myServo.write(restPositionIsLeft ? POS_KICK_RIGHT : POS_KICK_LEFT);
    emitMachine("bad_duck_kick_started");
  }
}

void processQueue() {
  if (!autoSortEnabled || servoState != 0) return;
  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    if (queue[i].active && queue[i].ls2Triggered) {
      if (millis() - queue[i].ls2Time >= kloeppelDelay) {
        processAutoDecision(i);
        break;
      }
    }
  }
}

void handleServoStateMachine() {
  if (servoState == 1) {
    if (millis() - servoMoveTimer >= timeKick) {
      servoState = 2;
      servoMoveTimer = millis();
      if (badDucksCount >= 3) {
        badDucksCount = 0;
        restPositionIsLeft = !restPositionIsLeft;
        servoReturnWaitTime = timeReturnWechsel;
      } else {
        servoReturnWaitTime = timeReturnNormal;
      }
      myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT);
      emitMachine("servo_return_started");
    }
  } else if (servoState == 2) {
    if (millis() - servoMoveTimer >= servoReturnWaitTime) {
      myServo.detach();
      servoState = 0;
      emitMachine("servo_cycle_done");
    }
  }
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
      for (uint8_t i = 0; i < MAX_DUCKS; i++) {
        if (!queue[i].active) {
          queue[i].active = true;
          queue[i].hasRFID = false;
          queue[i].ls2Triggered = false;
          queue[i].ls1Time = timerLS1;
          queue[i].ls2Time = 0;
          queue[i].uidHex[0] = '\0';
          emitQueueEvent("ls1_registered", i, "", false);
          break;
        }
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

void checkRFID() {
  bool duckWaitingForRFID = false;
  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    if (queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered) {
      duckWaitingForRFID = true;
      break;
    }
  }
  if (!duckWaitingForRFID) return;

  uint8_t uid[7];
  uint8_t uidLen;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 45)) {
    bool isDuplicate = false;
    if (uidLen == lastUIDLen && (millis() - lastUIDTime < RFID_DUP_MS)) {
      isDuplicate = true;
      for (uint8_t i = 0; i < uidLen; i++) {
        if (uid[i] != lastUID[i]) { isDuplicate = false; break; }
      }
    }
    if (!isDuplicate) {
      char uidHex[UID_HEX_LEN];
      uidToHex(uid, uidLen, uidHex, sizeof(uidHex));
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      for (uint8_t i = 0; i < MAX_DUCKS; i++) {
        if (queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered) {
          if (queue[i].ls1Time < oldest) {
            oldest = queue[i].ls1Time;
            targetIdx = i;
          }
        }
      }
      if (targetIdx != -1) {
        queue[targetIdx].hasRFID = true;
        strncpy(queue[targetIdx].uidHex, uidHex, UID_HEX_LEN - 1);
        queue[targetIdx].uidHex[UID_HEX_LEN - 1] = '\0';
        emitRFID(uidHex, "ok", targetIdx);
      }
      lastUIDLen = uidLen;
      for (uint8_t i = 0; i < uidLen; i++) lastUID[i] = uid[i];
      lastUIDTime = millis();
      strncpy(lastUIDHex, uidHex, UID_HEX_LEN - 1);
      lastUIDHex[UID_HEX_LEN - 1] = '\0';
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
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      for (uint8_t i = 0; i < MAX_DUCKS; i++) {
        if (queue[i].active && !queue[i].ls2Triggered) {
          unsigned long travelTime = timerLS2 - queue[i].ls1Time;
          if (travelTime >= 150 && travelTime <= 1200) {
            if (queue[i].ls1Time < oldest) {
              oldest = queue[i].ls1Time;
              targetIdx = i;
            }
          }
        }
      }
      if (targetIdx != -1) {
        queue[targetIdx].ls2Triggered = true;
        queue[targetIdx].ls2Time = timerLS2;
        emitQueueEvent("ls2_confirmed", targetIdx, queue[targetIdx].uidHex, queue[targetIdx].hasRFID);
      } else {
        emitError("LS2_ORPHAN", "No matching LS1 duck");
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

bool extractStringValue(const char* src, const char* key, char* out, size_t outSize) {
  char pattern[20];
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
  char pattern[20];
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
  if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

void handleJsonCommand(char* line) {
  char cmd[20] = "";
  if (!extractStringValue(line, "cmd", cmd, sizeof(cmd))) {
    emitError("CMD_MISSING", "Missing cmd");
    return;
  }

  if (strcmp(cmd, "emergency_stop") == 0) {
    emergencyStop();
    return;
  }

  if (strcmp(cmd, "belt") == 0) {
    char action[16] = "";
    if (!extractStringValue(line, "action", action, sizeof(action))) {
      emitError("BELT_ACTION", "Missing action");
      return;
    }
    if (strcmp(action, "start") == 0) startBelts();
    else if (strcmp(action, "stop") == 0) stopBelts();
    else if (strcmp(action, "resume") == 0) resumeBelts();
    else emitError("BELT_ACTION", "Unknown action");
    return;
  }

  if (strcmp(cmd, "actuator") == 0) {
    char name[16] = "";
    long state = 0;
    long value = 0;
    extractStringValue(line, "name", name, sizeof(name));
    extractIntValue(line, "state", &state);
    extractIntValue(line, "value", &value);

    if (strcmp(name, "motor_fast") == 0) setMotorFast(value > 0 ? (uint8_t)value : (state ? pwmSchnellDefault : 0));
    else if (strcmp(name, "motor_slow") == 0) setMotorSlow(value > 0 ? (uint8_t)value : (state ? pwmLangsamDefault : 0));
    else if (strcmp(name, "relay1") == 0) setRelay(PIN_RELAIS1, "relay1", state ? 1 : 0);
    else if (strcmp(name, "relay2") == 0) setRelay(PIN_RELAIS2, "relay2", state ? 1 : 0);
    else if (strcmp(name, "led") == 0) { digitalWrite(PIN_LED, state ? HIGH : LOW); emitActuator("led", state ? 1 : 0, state ? 1 : 0); }
    else emitError("ACTUATOR_UNKNOWN", "Unknown actuator");
    return;
  }

  if (strcmp(cmd, "servo_rest") == 0) {
    char side[8] = "left";
    extractStringValue(line, "side", side, sizeof(side));
    servoRest(strcmp(side, "left") == 0);
    return;
  }

  if (strcmp(cmd, "servo_kick") == 0) {
    char target[8] = "right";
    extractStringValue(line, "target", target, sizeof(target));
    servoKickToTarget(target);
    return;
  }

  if (strcmp(cmd, "set_config") == 0) {
    bool b;
    long v;
    if (extractBoolValue(line, "auto_sort_enabled", &b)) autoSortEnabled = b;
    if (extractIntValue(line, "pwm_fast", &v)) pwmSchnellAktuell = constrain(v, 0, 255);
    if (extractIntValue(line, "pwm_slow", &v)) pwmLangsamAktuell = constrain(v, 0, 255);
    emitMachine("config_updated");
    return;
  }

  if (strcmp(cmd, "ping") == 0) {
    emitMachine("pong");
    return;
  }

  emitError("CMD_UNKNOWN", "Unknown cmd");
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
      if (serialPos < sizeof(serialBuffer) - 1) serialBuffer[serialPos++] = c;
      else {
        serialPos = 0;
        emitError("SERIAL_OVERFLOW", "Serial buffer overflow");
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

  for (uint8_t i = 0; i < MAX_DUCKS; i++) {
    queue[i].active = false;
    queue[i].uidHex[0] = '\0';
  }

  myServo.attach(PIN_SERVO);
  myServo.write(POS_REST_LEFT);
  delay(500);
  myServo.detach();

  Wire.begin();
  Wire.beginTransmission(0x24);
  byte error = Wire.endTransmission();
  if (error == 0) {
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata) {
      nfc.SAMConfig();
      nfcAktiv = true;
      emitBoot("ok", "pn532_ready");
    } else {
      emitBoot("warning", "pn532_no_fw");
    }
  } else {
    emitBoot("warning", "no_i2c_0x24");
  }

  stopBelts();
  emitMachine("setup_complete");
}

void loop() {
  handleSerial();

  if (currentBeltState == CLEARING && millis() >= band1StopTime) {
    setMotorFast(0);
    currentBeltState = STOPPED;
    pauseStartTime = millis();
    emitMachine("clearing_complete_stopped");
  }

  if (currentBeltState == RUNNING || currentBeltState == CLEARING) {
    checkLS1();
    if (nfcAktiv) checkRFID();
    checkLS2();
    cleanupQueue();
    processQueue();
  }

  handleServoStateMachine();
}