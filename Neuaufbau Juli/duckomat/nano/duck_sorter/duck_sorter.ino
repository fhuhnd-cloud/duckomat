#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>
#include "duck_types.h"

// ============================================================
// Duckomat Nano-Firmware (fw=28.8)
//
// KALIBRIERTE BASISWERTE:
// 1. POSRESTL = 2122 µs (45° Ruhe Links)
// 2. POSMID   = 1600 µs (90° Neutral)
// 3. POSRESTR = 1050 µs (135° Ruhe Rechts)
// 4. Automatisch berechnet:
//    - Kick Rechts (75°) = 1774 µs
//    - Kick Links (105°) = 1417 µs
// 5. Freigegebene Schutzgrenzen: 750 µs - 2250 µs (MASTER Digital DS6030 TG)
// ============================================================

// -------------------- Pins --------------------
#define PIN_US1_TRIG 11
#define PIN_US1_ECHO 12
#define PIN_US2_TRIG 3
#define PIN_US2_ECHO 2
#define PIN_SERVO 5
#define PIN_LED 6
#define PN532_RESET 7  // Dummy-Pin (physisch nicht verbunden)
#define PN532_IRQ 8    // Dummy-Pin (physisch nicht verbunden)

#define PIN_MOTOR_SCHNELL 9
#define PIN_MOTOR_LANGSAM 10

#define I2C_SDA_PIN A4
#define I2C_SCL_PIN A5

// -------------------- Kalibrierte Parameter --------------------
const bool TEST_MODE = false;

const int SERVO_SAFE_MIN = 750;
const int SERVO_SAFE_MAX = 2250;
const bool SERVO_AUTO_DETACH_ENABLED = true;
const unsigned long SERVO_DETACH_DELAY_MS = 350;

// Die 3 gemessenen Basis-Winkel
int g_posRestLeft = 2122;     // 45° Ruhe Links
int g_posMid = 1600;          // 90° Mitte / Neutral
int g_posRestRight = 1050;     // 135° Ruhe Rechts

// Automatisch berechnete Kick-Winkel
int g_posKickRight = 1774;    // 75° Kick Rechts
int g_posKickLeft = 1417;     // 105° Kick Links

unsigned long g_kloeppelDelay = 400;      // 400 ms Delay US2 -> Klöppel
unsigned long g_kickHoldMs = 260;
unsigned long g_returnHoldMs = 260;
unsigned long g_returnWechselMs = 150;
bool g_invertLogic = false;

unsigned long g_usThresholdMm = 100;     // 100 mm = 10 cm Schwellwert
uint8_t g_usConfirmCount = 2;
unsigned long g_usIntervalMs = 15;
unsigned long g_usEchoTimeoutUs = 4500UL;
const float US_MAX_VALID_CM = 25.0;

unsigned long g_maxSingleDuckBlockMs = 1200;

unsigned long g_nfcTimeoutMs = 35;
uint8_t g_nfcRetries = 5;
const unsigned long NFC_IDLE_POLL_MS = 300;
const unsigned long NFC_RETRY_INTERVAL_MS = 4000;
const unsigned long HELLO_INTERVAL_MS = 1500;

// -------------------- Servo --------------------
ServoTimer2 kloeppel;
bool servoAttached = false;
unsigned long servoLastDriveMs = 0;

// -------------------- PN532 --------------------
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
bool nfcReady = false;
unsigned long lastNfcRetryMs = 0;
unsigned long lastNfcPollMs = 0;
NfcMode nfcMode = NFC_MODE_CONTINUOUS;

// -------------------- Runtime State --------------------
uint8_t pwmFast = 0;
uint8_t pwmSlow = 0;

char armedSide = 'N';
bool armedIsSwitch = false;
char lastUid[25] = "NONE";

unsigned long lastHelloMs = 0;

char rxLine[80];
uint8_t rxPos = 0;

UsStatus us1Status = {false, false, 0, false, false};
UsStatus us2Status = {false, false, 0, false, false};

unsigned long us1BlockedSinceMs = 0;
unsigned long us2BlockedSinceMs = 0;
bool us1LongBlockWarned = false;
bool us2LongBlockWarned = false;

unsigned long us1LastNoEchoWarnMs = 0;
unsigned long us2LastNoEchoWarnMs = 0;
const unsigned long NOECHO_WARN_INTERVAL_MS = 5000UL;

bool us1LastSentBlocked = false;
bool us2LastSentBlocked = false;
bool us1EverSent = false;
bool us2EverSent = false;

unsigned long usLastMeasureMs = 0;
uint8_t usNextSensor = 1;

DuckCtx duck = {false, 0, 0, 0, false, "NONE"};
uint32_t duckSeqCounter = 0;

#define KICK_QUEUE_SIZE 8
KickJob kickQueue[KICK_QUEUE_SIZE];
uint8_t qHead = 0;
uint8_t qTail = 0;
uint8_t qCount = 0;

ServoActionState servoActionState = SERVO_IDLE;
char activeKickSide = 'N';
uint32_t activeKickSeq = 0;
bool activeKickIsSwitch = false;
unsigned long servoPhaseMs = 0;

// -------------------- Hilfsfunktionen --------------------
void recalculateKickPositions() {
  g_posKickRight = (int)(g_posRestLeft + (30.0 / 45.0) * (g_posMid - g_posRestLeft));
  g_posKickLeft  = (int)(g_posMid + (15.0 / 45.0) * (g_posRestRight - g_posMid));
}

bool isDue(unsigned long now, unsigned long target) {
  return (long)(now - target) >= 0;
}

int clampServo(int us) {
  if (us < SERVO_SAFE_MIN) return SERVO_SAFE_MIN;
  if (us > SERVO_SAFE_MAX) return SERVO_SAFE_MAX;
  return us;
}

void driveServo(int us) {
  if (!servoAttached) {
    kloeppel.attach(PIN_SERVO);
    servoAttached = true;
  }
  kloeppel.write(us);
  servoLastDriveMs = millis();
}

void updateServoAutoDetach() {
  if (!SERVO_AUTO_DETACH_ENABLED) return;
  if (!servoAttached) return;
  if (servoActionState != SERVO_IDLE) return;
  if (millis() - servoLastDriveMs >= SERVO_DETACH_DELAY_MS) {
    kloeppel.detach();
    servoAttached = false;
    pinMode(PIN_SERVO, OUTPUT);
    digitalWrite(PIN_SERVO, LOW);
  }
}

void uidToDecimal(const uint8_t* rawUid, uint8_t len, char* outStr) {
  uint64_t dec = 0;
  for (uint8_t i = 0; i < len; i++) {
    dec = (dec << 8) | rawUid[i];
  }
  if (dec == 0) {
    strcpy(outStr, "0");
    return;
  }
  char rev[25];
  uint8_t r = 0;
  while (dec > 0) {
    rev[r++] = '0' + (dec % 10);
    dec /= 10;
  }
  uint8_t o = 0;
  while (r > 0) {
    outStr[o++] = rev[--r];
  }
  outStr[o] = '\0';
}

void clearI2CBusUnconditional() {
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, OUTPUT);
  delayMicroseconds(50);

  for (uint8_t i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(10);
  }

  pinMode(I2C_SDA_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(10);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(10);

  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(50);
}

bool initPN532Hardware() {
  #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(25000 /* us */, true /* reset_on_timeout */);
  #endif

  Wire.beginTransmission(0x24);
  byte err = Wire.endTransmission();
  if (err != 0) {
    nfcReady = false;
    return false;
  }

  nfc.begin();

  #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(25000 /* us */, true /* reset_on_timeout */);
  #endif

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    nfcReady = false;
    return false;
  }

  nfc.SAMConfig();
  nfcReady = true;
  return true;
}

// -------------------- Serial Protocol --------------------
void sendAck(long token, const __FlashStringHelper* cmd) {
  Serial.print(F("ACK token=")); Serial.print(token);
  Serial.print(F(" cmd=")); Serial.println(cmd);
}

void sendNack(long token, const __FlashStringHelper* reason) {
  Serial.print(F("NACK token=")); Serial.print(token);
  Serial.print(F(" reason=")); Serial.println(reason);
}

void sendHello() {
  Serial.print(F("HELLO fw=28 test=")); Serial.print(TEST_MODE ? 1 : 0);
  Serial.print(F(" nfc=")); Serial.print(nfcReady ? 1 : 0);
  Serial.print(F(" nfcmode=")); Serial.println(nfcMode == NFC_MODE_CONTINUOUS ? F("CONTINUOUS") : F("DUCKONLY"));
}

void sendState() {
  Serial.print(F("STATE armed=")); Serial.print(armedSide);
  Serial.print(F(" q=")); Serial.print(qCount);
  Serial.print(F(" fast=")); Serial.print(pwmFast);
  Serial.print(F(" slow=")); Serial.print(pwmSlow);
  Serial.print(F(" duck=")); Serial.print(duck.active ? 1 : 0);
  Serial.print(F(" nfc=")); Serial.print(nfcReady ? 1 : 0);
  Serial.print(F(" nfcmode=")); Serial.print(nfcMode == NFC_MODE_CONTINUOUS ? F("CONTINUOUS") : F("DUCKONLY"));
  Serial.print(F(" servo="));
  switch (servoActionState) {
    case SERVO_IDLE: Serial.print(F("IDLE")); break;
    case SERVO_HIT: Serial.print(F("HIT")); break;
    case SERVO_RETURN: Serial.print(F("RETURN")); break;
  }
  Serial.print(F(" lastuid=")); Serial.println(lastUid);
}

void sendCfg() {
  Serial.print(F("CFG posrestl=")); Serial.print(g_posRestLeft);
  Serial.print(F(" posmid=")); Serial.print(g_posMid);
  Serial.print(F(" posrestr=")); Serial.print(g_posRestRight);
  Serial.print(F(" poskickl=")); Serial.print(g_posKickLeft);
  Serial.print(F(" poskickr=")); Serial.print(g_posKickRight);
  Serial.print(F(" kdelay=")); Serial.print(g_kloeppelDelay);
  Serial.print(F(" khold=")); Serial.print(g_kickHoldMs);
  Serial.print(F(" rhold=")); Serial.print(g_returnHoldMs);
  Serial.print(F(" rholdswitch=")); Serial.print(g_returnWechselMs);
  Serial.print(F(" invert=")); Serial.print(g_invertLogic ? 1 : 0);
  Serial.print(F(" usthreshmm=")); Serial.print(g_usThresholdMm);
  Serial.print(F(" usconfirm=")); Serial.print(g_usConfirmCount);
  Serial.print(F(" usinterval=")); Serial.print(g_usIntervalMs);
  Serial.print(F(" usechotimeout=")); Serial.print(g_usEchoTimeoutUs);
  Serial.print(F(" maxblockms=")); Serial.print(g_maxSingleDuckBlockMs);
  Serial.print(F(" nfctimeout=")); Serial.print(g_nfcTimeoutMs);
  Serial.print(F(" nfcretries=")); Serial.println(g_nfcRetries);
}

void sendEvent(const __FlashStringHelper* type, uint32_t seq) {
  Serial.print(F("EV type=")); Serial.print(type);
  Serial.print(F(" seq=")); Serial.println(seq);
}

void sendSensorEdge(const __FlashStringHelper* which, bool blocked, float cm) {
  Serial.print(F("EV type=")); Serial.print(which);
  Serial.print(F(" state=")); Serial.print(blocked ? F("BLOCKED") : F("FREE"));
  Serial.print(F(" cm="));
  if (cm < 0.0) Serial.println(F("-1"));
  else Serial.println(cm, 1);
}

void sendSensorInitStatus() {
  sendSensorEdge(F("LS1"), us1Status.blocked, -1.0);
  sendSensorEdge(F("LS2"), us2Status.blocked, -1.0);
}

void sendTagEvent(const char* uid) {
  Serial.print(F("EV type=TAG seq=0 uid="));
  Serial.println(uid);
}

void sendDuckEvent(uint32_t seq, const char* uid, DuckResult result) {
  Serial.print(F("DUCK seq=")); Serial.print(seq);
  Serial.print(F(" uid=")); Serial.print(uid);
  Serial.print(F(" result="));
  switch (result) {
    case RES_KICK_L: Serial.println(F("KICK_L")); break;
    case RES_KICK_R: Serial.println(F("KICK_R")); break;
    case RES_QUEUE_FULL: Serial.println(F("VALID_QUEUE_FULL_DROP")); break;
    case RES_UNARMED: Serial.println(F("VALID_UNARMED_DROP")); break;
    case RES_UNREADABLE: Serial.println(F("UNREADABLE_DROP")); break;
  }
}

// -------------------- Queue & Servo Logik --------------------
bool enqueueKick(char side, uint32_t seq, unsigned long dueMs, bool isSwitch) {
  if (qCount >= KICK_QUEUE_SIZE) return false;
  kickQueue[qTail].used = true;
  kickQueue[qTail].side = side;
  kickQueue[qTail].seq = seq;
  kickQueue[qTail].dueMs = dueMs;
  kickQueue[qTail].isSwitch = isSwitch;
  qTail = (qTail + 1) % KICK_QUEUE_SIZE;
  qCount++;
  return true;
}

bool peekKick(KickJob &job) {
  if (qCount == 0) return false;
  job = kickQueue[qHead];
  return true;
}

void popKick() {
  if (qCount == 0) return;
  qHead = (qHead + 1) % KICK_QUEUE_SIZE;
  qCount--;
}

void updateServo(unsigned long now) {
  updateServoAutoDetach();

  if (servoActionState == SERVO_IDLE) {
    KickJob job;
    if (peekKick(job)) {
      if (isDue(now, job.dueMs)) {
        popKick();
        activeKickSide = job.side;
        activeKickSeq = job.seq;
        activeKickIsSwitch = job.isSwitch;

        sendEvent(job.side == 'L' ? F("KICK_L") : F("KICK_R"), job.seq);

        int targetPos = (job.side == 'L')
          ? (g_invertLogic ? g_posKickRight : g_posKickLeft)
          : (g_invertLogic ? g_posKickLeft : g_posKickRight);

        driveServo(clampServo(targetPos));
        servoActionState = SERVO_HIT;
        servoPhaseMs = now + g_kickHoldMs;
      }
    }
  }
  else if (servoActionState == SERVO_HIT) {
    if (isDue(now, servoPhaseMs)) {
      int restPos;
      unsigned long holdTime;

      if (activeKickIsSwitch) {
        restPos = (activeKickSide == 'L')
          ? (g_invertLogic ? g_posRestLeft : g_posRestRight)
          : (g_invertLogic ? g_posRestRight : g_posRestLeft);
        holdTime = g_returnWechselMs;
      } else {
        restPos = (activeKickSide == 'L')
          ? (g_invertLogic ? g_posRestRight : g_posRestLeft)
          : (g_invertLogic ? g_posRestLeft : g_posRestRight);
        holdTime = g_returnHoldMs;
      }

      driveServo(clampServo(restPos));
      servoActionState = SERVO_RETURN;
      servoPhaseMs = now + holdTime;
    }
  }
  else if (servoActionState == SERVO_RETURN) {
    if (isDue(now, servoPhaseMs)) {
      servoActionState = SERVO_IDLE;
      sendEvent(F("SERVO_REST"), activeKickSeq);
    }
  }
}

// -------------------- Sensorik (Ultraschall) --------------------
float measureUltraschallRaw(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, g_usEchoTimeoutUs);
  if (duration == 0) return -1.0;

  float cm = (duration * 0.0343) / 2.0;
  if (cm > US_MAX_VALID_CM) return -1.0;
  return cm;
}

void processUsSensor(uint8_t idx, float cm, unsigned long now) {
  UsStatus &st = (idx == 1) ? us1Status : us2Status;
  const __FlashStringHelper* legacyName = (idx == 1) ? F("LS1") : F("LS2");
  const __FlashStringHelper* usName = (idx == 1) ? F("US1") : F("US2");

  if (cm < 0.0) {
    st.anomalyNoEcho = true;
    unsigned long &lastWarn = (idx == 1) ? us1LastNoEchoWarnMs : us2LastNoEchoWarnMs;
    if (now - lastWarn >= NOECHO_WARN_INTERVAL_MS) {
      Serial.print(F("EV type=SENSOR_ANOMALY_NOECHO sensor="));
      Serial.println(legacyName);
      lastWarn = now;
    }
  } else {
    st.anomalyNoEcho = false;
  }

  bool rawBlock = (cm >= 0.0 && cm <= (float)(g_usThresholdMm / 10.0));
  if (rawBlock) {
    if (st.confirmCounter < g_usConfirmCount) st.confirmCounter++;
  } else {
    st.confirmCounter = 0;
  }

  bool isBlocked = (st.confirmCounter >= g_usConfirmCount);
  st.blocked = isBlocked;
  bool &everSent = (idx == 1) ? us1EverSent : us2EverSent;
  bool &lastSent = (idx == 1) ? us1LastSentBlocked : us2LastSentBlocked;

  if (!everSent || isBlocked != lastSent) {
    sendSensorEdge(legacyName, isBlocked, cm);
    sendSensorEdge(usName, isBlocked, cm);
    
    lastSent = isBlocked;
    everSent = true;

    if (idx == 1 && isBlocked) {
      duck.active = true;
      duck.seq = ++duckSeqCounter;
      duck.startTimeMs = now;
      duck.nfcFound = false;
      strcpy(duck.uid, "NONE");
      sendEvent(F("DUCK_START"), duck.seq);
    }
    else if (idx == 2 && isBlocked) {
      if (duck.active) {
        if (duck.nfcFound && strcmp(duck.uid, "NONE") != 0) {
          if (armedSide == 'L' || armedSide == 'R') {
            bool ok = enqueueKick(armedSide, duck.seq, now + g_kloeppelDelay, armedIsSwitch);
            sendDuckEvent(duck.seq, duck.uid, ok ? (armedSide == 'L' ? RES_KICK_L : RES_KICK_R) : RES_QUEUE_FULL);
          } else {
            sendDuckEvent(duck.seq, duck.uid, RES_UNARMED);
          }
        } else {
          sendDuckEvent(duck.seq, duck.uid, RES_UNREADABLE);
        }
        duck.active = false;
      }
    }
  }

  unsigned long &blockSince = (idx == 1) ? us1BlockedSinceMs : us2BlockedSinceMs;
  bool &longWarned = (idx == 1) ? us1LongBlockWarned : us2LongBlockWarned;

  if (isBlocked) {
    if (blockSince == 0) blockSince = now;
    if (!longWarned && (now - blockSince >= g_maxSingleDuckBlockMs)) {
      Serial.print(F("EV type=LONG_BLOCK_SUSPECTED sensor="));
      Serial.println(legacyName);
      longWarned = true;
    }
  } else {
    blockSince = 0;
    longWarned = false;
  }
}

void updateSensors(unsigned long now) {
  if (now - usLastMeasureMs < g_usIntervalMs) return;
  usLastMeasureMs = now;

  if (usNextSensor == 1) {
    float cm1 = measureUltraschallRaw(PIN_US1_TRIG, PIN_US1_ECHO);
    processUsSensor(1, cm1, now);
    usNextSensor = 2;
  } else {
    float cm2 = measureUltraschallRaw(PIN_US2_TRIG, PIN_US2_ECHO);
    processUsSensor(2, cm2, now);
    usNextSensor = 1;
  }
}

// -------------------- NFC Polling --------------------
void updateNfc(unsigned long now) {
  if (!nfcReady) {
    if (now - lastNfcRetryMs >= NFC_RETRY_INTERVAL_MS) {
      lastNfcRetryMs = now;
      initPN532Hardware();
    }
    return;
  }

  bool shouldPoll = false;
  if (duck.active && !duck.nfcFound) {
    shouldPoll = true;
  } else if (nfcMode == NFC_MODE_CONTINUOUS && (now - lastNfcPollMs >= NFC_IDLE_POLL_MS)) {
    shouldPoll = true;
  }

  if (!shouldPoll) return;
  lastNfcPollMs = now;

  uint8_t rawUid[7];
  uint8_t uidLen = 0;

  bool success = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    rawUid,
    &uidLen,
    g_nfcTimeoutMs
  );

  if (success && uidLen > 0) {
    char decBuf[25];
    uidToDecimal(rawUid, uidLen, decBuf);

    if (strcmp(decBuf, lastUid) != 0 || duck.active) {
      strcpy(lastUid, decBuf);
      sendTagEvent(decBuf);
    }

    if (duck.active && !duck.nfcFound) {
      strcpy(duck.uid, decBuf);
      duck.nfcFound = true;
    }
  }
}

// -------------------- Serial Parser --------------------
long parseLongVal(const char* line, const char* key) {
  const char* p = strstr(line, key);
  if (!p) return 0;
  p += strlen(key);
  return atol(p);
}

void parseStringVal(const char* line, const char* key, char* outVal, size_t maxLen) {
  outVal[0] = '\0';
  const char* p = strstr(line, key);
  if (!p) return;
  p += strlen(key);
  size_t i = 0;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < maxLen - 1) {
    outVal[i++] = *p++;
  }
  outVal[i] = '\0';
}

void handleCommand(char* cmd) {
  long token = parseLongVal(cmd, "token=");

  if (strncmp(cmd, "HELLO", 5) == 0) {
    sendHello();
    sendState();
    sendCfg();
    sendSensorInitStatus();
  }
  else if (strncmp(cmd, "STATE?", 6) == 0) {
    sendState();
    sendSensorInitStatus();
  }
  else if (strncmp(cmd, "CFG?", 4) == 0) {
    sendCfg();
  }
  else if (strncmp(cmd, "ARM", 3) == 0) {
    char side[4] = "N";
    parseStringVal(cmd, "side=", side, sizeof(side));
    armedSide = side[0];
    armedIsSwitch = (parseLongVal(cmd, "switch=") == 1);
    sendAck(token, F("ARM"));
  }
  else if (strncmp(cmd, "MOTOR", 5) == 0) {
    pwmFast = (uint8_t)parseLongVal(cmd, "fast=");
    pwmSlow = (uint8_t)parseLongVal(cmd, "slow=");
    analogWrite(PIN_MOTOR_SCHNELL, pwmFast);
    analogWrite(PIN_MOTOR_LANGSAM, pwmSlow);
    sendAck(token, F("MOTOR"));
    sendState();
  }
  else if (strncmp(cmd, "NFCMODE", 7) == 0) {
    char mode[16] = "";
    parseStringVal(cmd, "mode=", mode, sizeof(mode));
    if (strcmp(mode, "DUCKONLY") == 0) nfcMode = NFC_MODE_DUCKONLY;
    else nfcMode = NFC_MODE_CONTINUOUS;
    sendAck(token, F("NFCMODE"));
  }
  else if (strncmp(cmd, "KICK", 4) == 0) {
    char side[4] = "L";
    parseStringVal(cmd, "side=", side, sizeof(side));
    enqueueKick(side[0], 0, millis(), false);
    sendAck(token, F("KICK"));
  }
  else if (strncmp(cmd, "SERVOUS", 7) == 0) {
    int us = (int)parseLongVal(cmd, "us=");
    driveServo(clampServo(us));
    sendAck(token, F("SERVOUS"));
  }
  else if (strncmp(cmd, "CONFIG", 6) == 0) {
    char key[20] = "";
    parseStringVal(cmd, "key=", key, sizeof(key));
    long val = parseLongVal(cmd, "value=");

    if (strcmp(key, "POSRESTL") == 0) {
      g_posRestLeft = val;
      recalculateKickPositions();
    }
    else if (strcmp(key, "POSMID") == 0) {
      g_posMid = val;
      recalculateKickPositions();
    }
    else if (strcmp(key, "POSRESTR") == 0) {
      g_posRestRight = val;
      recalculateKickPositions();
    }
    else if (strcmp(key, "KDELAY") == 0) g_kloeppelDelay = val;
    else if (strcmp(key, "KHOLD") == 0) g_kickHoldMs = val;
    else if (strcmp(key, "RHOLD") == 0) g_returnHoldMs = val;
    else if (strcmp(key, "RHOLDSW") == 0) g_returnWechselMs = val;
    else if (strcmp(key, "INVERT") == 0) g_invertLogic = (val == 1);
    else if (strcmp(key, "USTHRESHMM") == 0) g_usThresholdMm = val;
    else if (strcmp(key, "USCONFIRM") == 0) g_usConfirmCount = (uint8_t)val;
    else if (strcmp(key, "USINTERVAL") == 0) g_usIntervalMs = val;
    else if (strcmp(key, "USECHOTIMEOUT") == 0) g_usEchoTimeoutUs = val;
    else if (strcmp(key, "MAXBLOCKMS") == 0) g_maxSingleDuckBlockMs = val;
    else if (strcmp(key, "NFCTIMEOUT") == 0) g_nfcTimeoutMs = val;
    else if (strcmp(key, "NFCRETRIES") == 0) g_nfcRetries = (uint8_t)val;

    sendAck(token, F("CONFIG"));
  }
  else {
    sendNack(token, F("UNKNOWN_CMD"));
  }
}

void updateSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxLine[rxPos] = '\0';
      if (rxPos > 0) {
        handleCommand(rxLine);
      }
      rxPos = 0;
    } else {
      if (rxPos < sizeof(rxLine) - 1) {
        rxLine[rxPos++] = c;
      }
    }
  }
}

// -------------------- Setup & Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(PIN_US1_TRIG, OUTPUT);
  pinMode(PIN_US1_ECHO, INPUT);
  pinMode(PIN_US2_TRIG, OUTPUT);
  pinMode(PIN_US2_ECHO, INPUT);

  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);

  recalculateKickPositions();

  clearI2CBusUnconditional();

  Wire.begin();
  #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(25000 /* us */, true /* reset_on_timeout */);
  #endif

  initPN532Hardware();

  // Startposition: 45° Ruhe Links (2122 µs)
  driveServo(clampServo(g_posRestLeft));

  sendHello();
  sendState();
  sendCfg();
  sendSensorInitStatus();
}

void loop() {
  unsigned long now = millis();

  updateSerial();
  updateSensors(now);
  updateNfc(now);
  updateServo(now);

  if (now - lastHelloMs >= HELLO_INTERVAL_MS) {
    lastHelloMs = now;
    sendHello();
  }
}