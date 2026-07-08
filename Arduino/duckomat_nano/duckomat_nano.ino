#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>

#define PIN_LS1 2
#define PIN_LS2 3
#define PIN_SERVO 5
#define PIN_LED 6
#define PIN_MOTOR_SCHNELL 9
#define PIN_MOTOR_LANGSAM 10
#define PIN_RELAIS1 11
#define PIN_RELAIS2 12
#define PN532_IRQ 8
#define PN532_RESET 7

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
ServoTimer2 myServo;

const int boxCapacity = 3;
const int pwmSchnell = 255;
const int pwmLangsam = 100;
const int kloeppelDelay = 530;
const int POS_REST_LEFT = 990;
const int POS_KICK_LEFT = 1330;
const int POS_REST_RIGHT = 2030;
const int POS_KICK_RIGHT = 1690;
const int timeKick = 280;
const int timeReturnNormal = 280;
const int timeReturnWechsel = 150;
const unsigned long RFID_DUP_MS = 1000;
const unsigned long LS_BLIND_MS = 400;
const unsigned long LS_FUZZ_MS = 25;

enum BeltState { STOPPED, RUNNING };
BeltState currentBeltState = STOPPED;

bool restPositionIsLeft = true;
int badDucksCount = 0;
int goodDucksCount = 0;
bool nfcAktiv = false;
bool paused = false;

uint8_t lastUID[7];
uint8_t lastUIDLen = 0;
unsigned long lastUIDTime = 0;

#define MAX_DUCKS 10
struct Duck {
  bool active;
  unsigned long ls1Time;
  bool hasRFID;
  bool ls2Triggered;
  unsigned long ls2Time;
};
Duck queue[MAX_DUCKS];

int stateLS1 = 0;
unsigned long timerLS1 = 0;
int stateLS2 = 0;
unsigned long timerLS2 = 0;

int servoState = 0;
unsigned long servoMoveTimer = 0;
unsigned long servoReturnWaitTime = 280;

void startBelts();
void stopBelts();
void cleanupQueue();
void handleSerial();
void checkLS1();
void checkRFID();
void checkLS2();
void processQueue();
void handleServoStateMachine();
String uidToHex(const uint8_t *uid, uint8_t uidLen);
void sendEvent(const char* type, const String& uid, int slot, const char* state);
void resetAll();
void pauseSystem();
void resumeSystem();

String uidToHex(const uint8_t *uid, uint8_t uidLen) {
  char buf[20];
  int p = 0;
  for (uint8_t i = 0; i < uidLen; i++) {
    sprintf(&buf[p], "%02X", uid[i]);
    p += 2;
  }
  buf[p] = 0;
  return String(buf);
}

void sendEvent(const char* type, const String& uid, int slot, const char* state) {
  StaticJsonDocument<192> doc;
  doc["type"] = type;
  doc["uid"] = uid;
  doc["slot"] = slot;
  doc["state"] = state;
  doc["ms"] = millis();
  serializeJson(doc, Serial);
  Serial.println();
}

void resetAll() {
  paused = false;
  stopBelts();
  for (int i = 0; i < MAX_DUCKS; i++) queue[i].active = false;
  myServo.detach();
  servoState = 0;
  badDucksCount = 0;
  goodDucksCount = 0;
  stateLS1 = 0;
  stateLS2 = 0;
  lastUIDLen = 0;
  lastUIDTime = 0;
  sendEvent("system", "", -1, "reset");
}

void pauseSystem() {
  paused = true;
  stopBelts();
  sendEvent("system", "", -1, "paused");
}

void resumeSystem() {
  paused = false;
  sendEvent("system", "", -1, "resumed");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Boot OK");

  pinMode(PIN_LS1, INPUT_PULLUP);
  pinMode(PIN_LS2, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  pinMode(PIN_RELAIS1, OUTPUT);
  pinMode(PIN_RELAIS2, OUTPUT);

  for (int i = 0; i < MAX_DUCKS; i++) queue[i].active = false;

  myServo.attach(PIN_SERVO);
  myServo.write(POS_REST_LEFT);
  delay(500);
  myServo.detach();

  stopBelts();

  Wire.begin();
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (versiondata) {
    Serial.print("PN532 gefunden, Firmware: 0x");
    Serial.println(versiondata, HEX);
    nfc.SAMConfig();
    nfcAktiv = true;
  } else {
    Serial.println("PN532 NICHT gefunden");
  }

  sendEvent("test", "0000", -1, "setup_done");
}

void loop() {
  handleSerial();
  if (paused) return;
  checkLS1();
  if (nfcAktiv) checkRFID();
  checkLS2();
  cleanupQueue();
  processQueue();
  handleServoStateMachine();
}

void cleanupQueue() {
  for (int i = 0; i < MAX_DUCKS; i++) {
    if (queue[i].active && !queue[i].ls2Triggered && millis() - queue[i].ls1Time > 2000) {
      queue[i].active = false;
    }
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String payload = Serial.readStringUntil('\n');
  payload.trim();

  if (payload.length() == 1) {
    char c = payload[0];
    if (c == 'n' || c == 'N') resetAll();
    else if (c == 'p' || c == 'P') pauseSystem();
    else if (c == 'r' || c == 'R') {
      resumeSystem();
      startBelts();
    }
    else if (c == 'b' || c == 'B') {
      if (currentBeltState != STOPPED) stopBelts();
      else startBelts();
    }
  } else if (payload.startsWith("{")) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      sendEvent("error", "", -1, "bad_json");
      return;
    }
    String type = doc["type"] | "";
    if (type == "actuator") {
      String name = doc["name"] | "";
      int state = doc["state"] | 0;
      if ((name == "motor_main" || name == "motor_feed") && state == 1) startBelts();
      else if ((name == "motor_main" || name == "motor_feed") && state == 0) stopBelts();
    } else if (type == "emergency_stop") {
      resetAll();
      sendEvent("system", "", -1, "emergency_stop");
    }
  }
}

void checkLS1() {
  if (paused) return;
  bool currentLS1 = digitalRead(PIN_LS1);
  if (stateLS1 == 0 && currentLS1 == LOW) {
    stateLS1 = 1;
    timerLS1 = millis();
  } else if (stateLS1 == 1) {
    if (currentLS1 == HIGH) stateLS1 = 0;
    else if (millis() - timerLS1 >= LS_FUZZ_MS) {
      stateLS1 = 2;
      for (int i = 0; i < MAX_DUCKS; i++) {
        if (!queue[i].active) {
          queue[i] = {true, timerLS1, false, false, 0};
          sendEvent("sensor", "", i, "ls1");
          break;
        }
      }
      if (currentBeltState == STOPPED) startBelts();
    }
  } else if (stateLS1 == 2 && millis() - timerLS1 >= LS_BLIND_MS && currentLS1 == HIGH) {
    stateLS1 = 0;
  }
}

void checkRFID() {
  if (paused) return;
  bool duckWaitingForRFID = false;
  for (int i = 0; i < MAX_DUCKS; i++) if (queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered) duckWaitingForRFID = true;
  if (!duckWaitingForRFID) return;

  uint8_t uid[7];
  uint8_t uidLen;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 45)) {
    bool duplicate = uidLen == lastUIDLen && (millis() - lastUIDTime < RFID_DUP_MS);
    for (uint8_t i = 0; duplicate && i < uidLen; i++) if (uid[i] != lastUID[i]) duplicate = false;
    if (duplicate) return;

    int targetIdx = -1;
    unsigned long oldest = 0xFFFFFFFF;
    for (int i = 0; i < MAX_DUCKS; i++) {
      if (queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered && queue[i].ls1Time < oldest) {
        oldest = queue[i].ls1Time;
        targetIdx = i;
      }
    }

    if (targetIdx != -1) {
      queue[targetIdx].hasRFID = true;
      String u = uidToHex(uid, uidLen);
      sendEvent("uid", u, targetIdx, "read");
      lastUIDLen = uidLen;
      for (uint8_t i = 0; i < uidLen; i++) lastUID[i] = uid[i];
      lastUIDTime = millis();
    }
  }
}

void checkLS2() {
  if (paused) return;
  bool currentLS2 = digitalRead(PIN_LS2);
  if (stateLS2 == 0 && currentLS2 == LOW) {
    stateLS2 = 1;
    timerLS2 = millis();
  } else if (stateLS2 == 1) {
    if (currentLS2 == HIGH) stateLS2 = 0;
    else if (millis() - timerLS2 >= LS_FUZZ_MS) {
      stateLS2 = 2;
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      for (int i = 0; i < MAX_DUCKS; i++) {
        if (queue[i].active && !queue[i].ls2Triggered) {
          unsigned long travelTime = timerLS2 - queue[i].ls1Time;
          if (travelTime >= 150 && travelTime <= 1200 && queue[i].ls1Time < oldest) {
            oldest = queue[i].ls1Time;
            targetIdx = i;
          }
        }
      }
      if (targetIdx != -1) {
        queue[targetIdx].ls2Triggered = true;
        queue[targetIdx].ls2Time = timerLS2;
        sendEvent("sensor", "", targetIdx, "ls2");
      }
    }
  } else if (stateLS2 == 2 && millis() - timerLS2 >= LS_BLIND_MS && currentLS2 == HIGH) {
    stateLS2 = 0;
  }
}

void processQueue() {
  if (paused) return;
  if (servoState != 0) return;
  for (int i = 0; i < MAX_DUCKS; i++) {
    if (queue[i].active && queue[i].ls2Triggered && millis() - queue[i].ls2Time >= kloeppelDelay) {
      queue[i].active = false;
      if (queue[i].hasRFID) {
        badDucksCount++;
        sendEvent("duck", "", i, "bad");
        servoState = 1;
        servoMoveTimer = millis();
        myServo.attach(PIN_SERVO);
        myServo.write(restPositionIsLeft ? POS_KICK_RIGHT : POS_KICK_LEFT);
        sendEvent("servo", "", i, restPositionIsLeft ? "kick_right" : "kick_left");
      } else {
        goodDucksCount++;
        sendEvent("duck", "", i, "good");
      }
      break;
    }
  }
}

void handleServoStateMachine() {
  if (paused) return;
  if (servoState == 1 && millis() - servoMoveTimer >= timeKick) {
    servoState = 2;
    servoMoveTimer = millis();
    if (badDucksCount >= boxCapacity) {
      badDucksCount = 0;
      restPositionIsLeft = !restPositionIsLeft;
      servoReturnWaitTime = timeReturnWechsel;
    } else {
      servoReturnWaitTime = timeReturnNormal;
    }
    myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT);
    sendEvent("servo", "", -1, restPositionIsLeft ? "rest_left" : "rest_right");
  } else if (servoState == 2 && millis() - servoMoveTimer >= servoReturnWaitTime) {
    myServo.detach();
    servoState = 0;
  }
}

void startBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, pwmSchnell);
  analogWrite(PIN_MOTOR_LANGSAM, pwmLangsam);
  currentBeltState = RUNNING;
  sendEvent("belt", "", -1, "on");
}

void stopBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);
  currentBeltState = STOPPED;
  sendEvent("belt", "", -1, "off");
}