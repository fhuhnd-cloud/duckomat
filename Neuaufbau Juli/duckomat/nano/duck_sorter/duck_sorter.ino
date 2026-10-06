#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>
#include <avr/wdt.h>
#include "duck_types.h"

// ============================================================
// Duckomat Nano-Firmware (fw=24)
//
// KRITISCHER FIX ggue. fw=23: nfc.readPassiveTargetID() war ein
// BLOCKIERENDER Aufruf (bis zu g_nfcTimeoutMs=60ms Stillstand der
// gesamten loop()). Waehrend dieser Zeit wurden weder US-Sensoren
// abgetastet noch eingehende Serial-Befehle (ARM!) verarbeitet. Bei dicht
// folgenden Enten fuehrte das zu (a) verpassten LS1/LS2-Flanken und (b)
// zu spaet verarbeiteten ARM-Kommandos -> VALID_UNARMED_DROP.
//
// LOESUNG: Non-blocking NFC-Polling ueber den IRQ-Pin (PN532_IRQ=8).
// startPassiveTargetIDDetection() stoesst die Erkennung an, danach wird
// in JEDEM loop()-Durchlauf nur per digitalRead(IRQ) geprueft, ob ein Tag
// bereitsteht (readDetectedPassiveTargetID() dann nahezu instantan). Kein
// blockierender Wartezyklus mehr in der Hauptschleife.
//
// ZUSATZ: LS1/LS2-Sensorereignisse werden jetzt nur noch bei tatsaechlicher
// Zustandsaenderung gesendet (Edge-Trigger), nicht mehr bei jeder einzelnen
// Messung - reduziert die serielle Last und gibt ARM-Kommandos vom Pi
// weniger Konkurrenz auf der Leitung.
//
// FRUEHERE MEILENSTEINE (fw=21..23), zur Erinnerung fuer den naechsten Chat:
// - fw=21: UID-Uebertragung von Hex auf Dezimalformat umgestellt, damit sie
//   mit einem baugleichen Kaufscanner uebereinstimmt (uidToDecimal()).
// - fw=22: Anpassung an neu installierte US-Sensor-Schirme (Timeout,
//   Blindbereich-Handling, LONG_BLOCK_SUSPECTED-Diagnose).
// - fw=23: "Kein Echo" wieder als UNGUELTIG behandelt (nicht als
//   BLOCKIERT), da Schirme konstruktiv <2cm unmoeglich machen;
//   SENSOR_ANOMALY_NOECHO-Diagnosemeldung ergaenzt; Echo-Timeout auf
//   4500us korrigiert (war mit 2500us zu knapp und lieferte dauerhaft -1).
// ============================================================

uint8_t mcusr_mirror __attribute__((section(".noinit")));

void get_mcusr(void) \
  __attribute__((naked)) \
  __attribute__((used)) \
  __attribute__((section(".init3")));
void get_mcusr(void) {
  mcusr_mirror = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

// -------------------- Pins --------------------
#define PIN_US1_TRIG 11
#define PIN_US1_ECHO 12
#define PIN_US2_TRIG 3
#define PIN_US2_ECHO 2
#define PIN_SERVO 5
#define PIN_LED 6
#define PN532_RESET 7
#define PN532_IRQ 8
#define PIN_MOTOR_SCHNELL 10
#define PIN_MOTOR_LANGSAM 9

#define I2C_SDA_PIN A4
#define I2C_SCL_PIN A5

// -------------------- Config --------------------
const bool TEST_MODE = false;

const int SERVO_SAFE_MIN = 990;
const int SERVO_SAFE_MAX = 2060;
const unsigned long SERVO_DETACH_DELAY_MS = 5000;

const unsigned long DUCK_STUCK_TIMEOUT_MS = 8000UL;
const uint8_t WATCHDOG_TIMEOUT = WDTO_4S;

int g_posRestLeft  = 990;
int g_posKickLeft  = 1683;
int g_posKickRight = 1337;
int g_posRestRight = 2030;
unsigned long g_kloeppelDelay = 400;
unsigned long g_kickHoldMs     = 200;
unsigned long g_returnHoldMs   = 200;
unsigned long g_returnWechselMs = 150;
bool g_invertLogic = false;

unsigned long g_usThresholdMm = 100;
uint8_t g_usConfirmCount = 2;
unsigned long g_usIntervalMs = 10;
unsigned long g_usEchoTimeoutUs = 4500UL;
const float US_MAX_VALID_CM = 25.0;

unsigned long g_maxSingleDuckBlockMs = 1200;

unsigned long g_nfcTimeoutMs = 60;
uint8_t g_nfcRetries = 5;
const unsigned long NFC_RETRY_INTERVAL_MS = 3000;

// -------------------- Servo --------------------
ServoTimer2 kloeppel;
bool servoAttached = false;
unsigned long servoLastDriveMs = 0;

// -------------------- PN532 --------------------
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
bool nfcReady = false;
unsigned long lastNfcRetryMs = 0;
NfcMode nfcMode = NFC_MODE_CONTINUOUS;

enum NfcPollState { NFC_POLL_IDLE, NFC_POLL_WAITING };
NfcPollState nfcPollState = NFC_POLL_IDLE;
unsigned long nfcPollStartMs = 0;

// -------------------- Runtime state --------------------
uint8_t pwmFast = 0;
uint8_t pwmSlow = 0;

char armedSide = 'N';
bool armedIsSwitch = false;
char lastUid[24] = "NONE";

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
uint32_t us1NoEchoCount = 0;
uint32_t us2NoEchoCount = 0;
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
  if (!servoAttached) return;
  if (servoActionState != SERVO_IDLE) return;
  if (millis() - servoLastDriveMs >= SERVO_DETACH_DELAY_MS) {
    kloeppel.detach();
    servoAttached = false;
  }
}

void sendAck(long token, const __FlashStringHelper* cmd) {
  Serial.print(F("ACK token=")); Serial.print(token);
  Serial.print(F(" cmd=")); Serial.println(cmd);
}

void sendNack(long token, const __FlashStringHelper* reason) {
  Serial.print(F("NACK token=")); Serial.print(token);
  Serial.print(F(" reason=")); Serial.println(reason);
}

void sendHello() {
  Serial.print(F("HELLO fw=24 test=")); Serial.print(TEST_MODE ? 1 : 0);
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
    case SERVO_IDLE:   Serial.print(F("IDLE")); break;
    case SERVO_HIT:    Serial.print(F("HIT")); break;
    case SERVO_RETURN: Serial.print(F("RETURN")); break;
  }
  Serial.print(F(" lastuid=")); Serial.println(lastUid);
}

void sendCfg() {
  Serial.print(F("CFG posrestl=")); Serial.print(g_posRestLeft);
  Serial.print(F(" poskickl=")); Serial.print(g_posKickLeft);
  Serial.print(F(" poskickr=")); Serial.print(g_posKickRight);
  Serial.print(F(" posrestr=")); Serial.print(g_posRestRight);
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

void sendEventDetail(const __FlashStringHelper* type, uint32_t seq, const __FlashStringHelper* detail) {
  Serial.print(F("EV type=")); Serial.print(type);
  Serial.print(F(" seq=")); Serial.print(seq);
  Serial.print(F(" detail=")); Serial.println(detail);
}

void sendSensorEdge(const __FlashStringHelper* which, bool blocked, float cm) {
  Serial.print(F("EV type=")); Serial.print(which);
  Serial.print(F(" state=")); Serial.print(blocked ? F("BLOCKED") : F("FREE"));
  Serial.print(F(" cm="));
  if (cm < 0.0) Serial.println(F("-1"));
  else Serial.println(cm, 1);
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
    case RES_KICK_L:      Serial.println(F("KICK_L")); break;
    case RES_KICK_R:       Serial.println(F("KICK_R")); break;
    case RES_QUEUE_FULL:   Serial.println(F("VALID_QUEUE_FULL_DROP")); break;
    case RES_UNARMED:      Serial.println(F("VALID_UNARMED_DROP")); break;
    case RES_UNREADABLE:   Serial.println(F("UNREADABLE_DROP")); break;
  }
}

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
  kickQueue[qHead].used = false;
  qHead = (qHead + 1) % KICK_QUEUE_SIZE;
  qCount--;
}

void updateKickExecutor() {
  unsigned long nowMs = millis();

  if (servoActionState == SERVO_IDLE) {
    KickJob job;
    if (peekKick(job) && isDue(nowMs, job.dueMs)) {
      activeKickSide = job.side;
      activeKickSeq = job.seq;
      activeKickIsSwitch = job.isSwitch;
      popKick();
      driveServo(activeKickSide == 'L' ? g_posKickLeft : g_posKickRight);
      servoActionState = SERVO_HIT;
      servoPhaseMs = nowMs;
      Serial.print(F("EV type=KICK_FIRE seq=")); Serial.print(activeKickSeq);
      Serial.print(F(" side=")); Serial.println(activeKickSide);
    }
  } else if (servoActionState == SERVO_HIT) {
    if (nowMs - servoPhaseMs >= g_kickHoldMs) {
      int returnPos;
      if (activeKickIsSwitch) {
        returnPos = (activeKickSide == 'L') ? g_posRestRight : g_posRestLeft;
      } else {
        returnPos = (activeKickSide == 'L') ? g_posRestLeft : g_posRestRight;
      }
      driveServo(returnPos);
      servoActionState = SERVO_RETURN;
      servoPhaseMs = nowMs;
    }
  } else if (servoActionState == SERVO_RETURN) {
    unsigned long returnTime = activeKickIsSwitch ? g_returnWechselMs : g_returnHoldMs;
    if (nowMs - servoPhaseMs >= returnTime) {
      Serial.print(F("EV type=KICK_DONE seq=")); Serial.print(activeKickSeq);
      Serial.print(F(" side=")); Serial.println(activeKickSide);
      activeKickSide = 'N';
      activeKickSeq = 0;
      activeKickIsSwitch = false;
      servoActionState = SERVO_IDLE;
    }
  }
}

void uidToDecimal(const uint8_t* uid, uint8_t uidLen, char* out, size_t outSize) {
  uint64_t value = 0;
  for (int i = (int)uidLen - 1; i >= 0; i--) {
    value = (value << 8) | uid[i];
  }

  if (value == 0) {
    if (outSize >= 2) { out[0] = '0'; out[1] = '\0'; }
    return;
  }

  char buf[21];
  uint8_t pos = 0;
  while (value > 0 && pos < sizeof(buf) - 1) {
    buf[pos++] = '0' + (char)(value % 10);
    value /= 10;
  }

  size_t outPos = 0;
  for (int i = (int)pos - 1; i >= 0 && outPos < outSize - 1; i--) {
    out[outPos++] = buf[i];
  }
  out[outPos] = '\0';
}

void startDuck() {
  duck.active = true;
  duck.seq = ++duckSeqCounter;
  duck.startMs = millis();
  duck.ls2Ms = 0;
  duck.uidValid = false;
  strcpy(duck.uidHex, "NONE");
  sendEvent(F("LS1_DUCK"), duck.seq);
}

void finishDuck() {
  duck.ls2Ms = millis();
  sendEvent(F("LS2_DUCK"), duck.seq);

  bool treatAsValid = g_invertLogic ? !duck.uidValid : duck.uidValid;

  if (treatAsValid) {
    if (armedSide == 'L' || armedSide == 'R') {
      bool ok = enqueueKick(armedSide, duck.seq, duck.ls2Ms + g_kloeppelDelay, armedIsSwitch);
      char usedSide = armedSide;
      armedSide = 'N';
      armedIsSwitch = false;
      if (ok) {
        sendDuckEvent(duck.seq, duck.uidHex, usedSide == 'L' ? RES_KICK_L : RES_KICK_R);
      } else {
        sendDuckEvent(duck.seq, duck.uidHex, RES_QUEUE_FULL);
      }
    } else {
      sendDuckEvent(duck.seq, duck.uidHex, RES_UNARMED);
    }
  } else {
    sendDuckEvent(duck.seq, duck.uidValid ? duck.uidHex : "NONE", RES_UNREADABLE);
  }

  duck.active = false;
  sendState();
}

void onLS1Falling() {
  if (duck.active) {
    sendEventDetail(F("ERROR"), duck.seq, F("LS1_OVERLAP"));
    return;
  }
  startDuck();
}

void onLS2Falling() {
  if (!duck.active) {
    sendEventDetail(F("EV_IGNORED"), 0, F("LS2_WITHOUT_LS1"));
    return;
  }
  finishDuck();
}

void checkDuckStuck() {
  if (!duck.active) return;
  if (millis() - duck.startMs < DUCK_STUCK_TIMEOUT_MS) return;

  sendEventDetail(F("ERROR"), duck.seq, F("DUCK_STUCK_TIMEOUT"));
  duck.active = false;
  sendState();
}

float measureDistanceCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long dauer = pulseIn(echoPin, HIGH, g_usEchoTimeoutUs);
  if (dauer == 0) return -1.0;
  return dauer / 58.0;
}

bool classifyReading(float cm, bool &blockedOut) {
  if (cm < 0.0 || cm > US_MAX_VALID_CM) {
    return false;
  }
  blockedOut = (cm < (g_usThresholdMm / 10.0));
  return true;
}

bool updateUsStatus(UsStatus &st, bool currentBlocked, bool valid, bool &newStableBlocked) {
  if (!valid) {
    newStableBlocked = st.stableBlocked;
    return false;
  }

  if (!st.hasCandidate || st.candidateBlocked != currentBlocked) {
    st.candidateBlocked = currentBlocked;
    st.candidateCount = 1;
    st.hasCandidate = true;
    newStableBlocked = st.stableBlocked;
    return false;
  }

  if (st.candidateCount < 255) st.candidateCount++;

  if (st.candidateBlocked != st.stableBlocked && st.candidateCount >= g_usConfirmCount) {
    st.stableBlocked = st.candidateBlocked;
    st.candidateCount = 0;
    st.hasCandidate = false;
    newStableBlocked = st.stableBlocked;
    return true;
  }

  newStableBlocked = st.stableBlocked;
  return false;
}

void trackLongBlock(const __FlashStringHelper* which, bool stableBlocked,
                     unsigned long &blockedSinceMs, bool &warned, uint32_t seqForLog) {
  unsigned long now = millis();
  if (stableBlocked) {
    if (blockedSinceMs == 0) {
      blockedSinceMs = now;
      warned = false;
    } else if (!warned && (now - blockedSinceMs) > g_maxSingleDuckBlockMs) {
      warned = true;
      Serial.print(F("EV type=LONG_BLOCK_SUSPECTED sensor="));
      Serial.print(which);
      Serial.print(F(" ms="));
      Serial.println(now - blockedSinceMs);
    }
  } else {
    blockedSinceMs = 0;
    warned = false;
  }
}

void checkNoEchoAnomaly(const __FlashStringHelper* which, float cm,
                         uint32_t &count, unsigned long &lastWarnMs) {
  if (cm >= 0.0) return;

  count++;
  unsigned long now = millis();
  if (lastWarnMs != 0 && now - lastWarnMs < NOECHO_WARN_INTERVAL_MS) return;
  lastWarnMs = now;

  Serial.print(F("EV type=SENSOR_ANOMALY_NOECHO sensor="));
  Serial.print(which);
  Serial.print(F(" count="));
  Serial.println(count);
}

void processUs1() {
  float cm = measureDistanceCm(PIN_US1_TRIG, PIN_US1_ECHO);
  checkNoEchoAnomaly(F("LS1"), cm, us1NoEchoCount, us1LastNoEchoWarnMs);

  bool currentBlocked = us1Status.rawBlocked;
  bool valid = classifyReading(cm, currentBlocked);
  if (valid) {
    us1Status.rawBlocked = currentBlocked;
  }
  bool newState;
  bool changed = updateUsStatus(us1Status, currentBlocked, valid, newState);

  if (!us1EverSent || us1Status.rawBlocked != us1LastSentBlocked) {
    sendSensorEdge(F("LS1"), us1Status.rawBlocked, cm);
    us1LastSentBlocked = us1Status.rawBlocked;
    us1EverSent = true;
  }

  trackLongBlock(F("LS1"), us1Status.stableBlocked, us1BlockedSinceMs, us1LongBlockWarned, duck.seq);
  if (changed && newState) onLS1Falling();
}

void processUs2() {
  float cm = measureDistanceCm(PIN_US2_TRIG, PIN_US2_ECHO);
  checkNoEchoAnomaly(F("LS2"), cm, us2NoEchoCount, us2LastNoEchoWarnMs);

  bool currentBlocked = us2Status.rawBlocked;
  bool valid = classifyReading(cm, currentBlocked);
  if (valid) {
    us2Status.rawBlocked = currentBlocked;
  }
  bool newState;
  bool changed = updateUsStatus(us2Status, currentBlocked, valid, newState);

  if (!us2EverSent || us2Status.rawBlocked != us2LastSentBlocked) {
    sendSensorEdge(F("LS2"), us2Status.rawBlocked, cm);
    us2LastSentBlocked = us2Status.rawBlocked;
    us2EverSent = true;
  }

  trackLongBlock(F("LS2"), us2Status.stableBlocked, us2BlockedSinceMs, us2LongBlockWarned, duck.seq);
  if (changed && newState) onLS2Falling();
}

void updateUltrasonicSensors() {
  unsigned long now = millis();
  if (now - usLastMeasureMs < g_usIntervalMs) return;
  usLastMeasureMs = now;

  if (usNextSensor == 1) {
    processUs1();
    usNextSensor = 2;
  } else {
    processUs2();
    usNextSensor = 1;
  }
}

void recoverI2CBus() {
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(10);

  if (digitalRead(I2C_SDA_PIN) == LOW) {
    for (uint8_t i = 0; i < 10; i++) {
      pinMode(I2C_SCL_PIN, OUTPUT);
      digitalWrite(I2C_SCL_PIN, LOW);
      delayMicroseconds(10);
      pinMode(I2C_SCL_PIN, INPUT_PULLUP);
      delayMicroseconds(10);
      if (digitalRead(I2C_SDA_PIN) == HIGH) break;
    }
  }

  pinMode(I2C_SDA_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(10);
  pinMode(I2C_SCL_PIN, OUTPUT);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(10);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(10);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
}

void setupPn532() {
  if (TEST_MODE) { nfcReady = false; return; }

  recoverI2CBus();

  Wire.begin();
  Wire.setWireTimeout(1000000UL, true);

  Wire.beginTransmission(0x24);
  byte i2cError = Wire.endTransmission();

  if (i2cError == 0) {
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata) {
      nfc.SAMConfig();
      nfcReady = true;
      nfcPollState = NFC_POLL_IDLE;
    } else {
      nfcReady = false;
    }
  } else {
    nfcReady = false;
  }
}

void maybeRetryNfcInit() {
  if (TEST_MODE) return;
  if (nfcReady) return;

  unsigned long now = millis();
  if (now - lastNfcRetryMs < NFC_RETRY_INTERVAL_MS) return;
  lastNfcRetryMs = now;

  setupPn532();
  if (nfcReady) {
    sendEvent(F("NFC_RECOVERED"), 0);
  }
}

void pollNfc() {
  if (TEST_MODE) return;
  if (!nfcReady) return;

  bool wantPoll = (nfcMode == NFC_MODE_CONTINUOUS) || (duck.active && !duck.uidValid);

  if (nfcPollState == NFC_POLL_IDLE) {
    if (!wantPoll) return;

    bool started = nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);
    if (Wire.getWireTimeoutFlag()) {
      Wire.clearWireTimeoutFlag();
      nfcReady = false;
      sendEvent(F("NFC_TIMEOUT_DETECTED"), 0);
      return;
    }
    if (started) {
      nfcPollState = NFC_POLL_WAITING;
      nfcPollStartMs = millis();
    }
    return;
  }

  if (digitalRead(PN532_IRQ) == LOW) {
    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool success = nfc.readDetectedPassiveTargetID(uid, &uidLen);
    nfcPollState = NFC_POLL_IDLE;

    if (Wire.getWireTimeoutFlag()) {
      Wire.clearWireTimeoutFlag();
      nfcReady = false;
      sendEvent(F("NFC_TIMEOUT_DETECTED"), 0);
      return;
    }

    if (!success) return;

    char dec[24];
    uidToDecimal(uid, uidLen, dec, sizeof(dec));

    strncpy(lastUid, dec, sizeof(lastUid) - 1);
    lastUid[sizeof(lastUid) - 1] = '\0';
    sendTagEvent(dec);

    if (duck.active && !duck.uidValid) {
      strncpy(duck.uidHex, dec, sizeof(duck.uidHex) - 1);
      duck.uidHex[sizeof(duck.uidHex) - 1] = '\0';
      duck.uidValid = true;
    }
    return;
  }

  if (millis() - nfcPollStartMs > g_nfcTimeoutMs) {
    nfcPollState = NFC_POLL_IDLE;
  }
}

void handleCommand(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) return;

  long token = -1;
  char side = 'N';
  int fast = -1;
  int slow = -1;
  long servoUs = -1;
  char uid[24] = {0};
  char modeArg[12] = {0};
  char key[12] = {0};
  long value = -1;
  int switchFlag = -1;

  char* tok;
  while ((tok = strtok(nullptr, " ")) != nullptr) {
    if (strncmp(tok, "token=", 6) == 0) token = atol(tok + 6);
    else if (strncmp(tok, "side=", 5) == 0) side = tok[5];
    else if (strncmp(tok, "fast=", 5) == 0) fast = atoi(tok + 5);
    else if (strncmp(tok, "slow=", 5) == 0) slow = atoi(tok + 5);
    else if (strncmp(tok, "us=", 3) == 0) servoUs = atol(tok + 3);
    else if (strncmp(tok, "uid=", 4) == 0) {
      strncpy(uid, tok + 4, sizeof(uid) - 1);
      uid[sizeof(uid) - 1] = '\0';
    } else if (strncmp(tok, "mode=", 5) == 0) {
      strncpy(modeArg, tok + 5, sizeof(modeArg) - 1);
      modeArg[sizeof(modeArg) - 1] = '\0';
    } else if (strncmp(tok, "key=", 4) == 0) {
      strncpy(key, tok + 4, sizeof(key) - 1);
      key[sizeof(key) - 1] = '\0';
    } else if (strncmp(tok, "value=", 6) == 0) {
      value = atol(tok + 6);
    } else if (strncmp(tok, "switch=", 7) == 0) {
      switchFlag = atoi(tok + 7);
    }
  }

  if (strcmp(cmd, "PING") == 0) {
    Serial.print(F("PONG token=")); Serial.print(token);
    Serial.print(F(" ms=")); Serial.println(millis());
    return;
  }

  if (strcmp(cmd, "STATE?") == 0) {
    sendAck(token, F("STATE?"));
    sendState();
    return;
  }

  if (strcmp(cmd, "CFG?") == 0) {
    sendAck(token, F("CFG?"));
    sendCfg();
    return;
  }

  if (strcmp(cmd, "CONFIG") == 0) {
    if (key[0] == '\0' || value == -1) { sendNack(token, F("BAD_KEYVAL")); return; }
    bool ok = true;
    if (strcmp(key, "POSRESTL") == 0) g_posRestLeft = clampServo((int)value);
    else if (strcmp(key, "POSKICKL") == 0) g_posKickLeft = clampServo((int)value);
    else if (strcmp(key, "POSKICKR") == 0) g_posKickRight = clampServo((int)value);
    else if (strcmp(key, "POSRESTR") == 0) g_posRestRight = clampServo((int)value);
    else if (strcmp(key, "KDELAY") == 0) g_kloeppelDelay = (unsigned long)value;
    else if (strcmp(key, "KHOLD") == 0) g_kickHoldMs = (unsigned long)value;
    else if (strcmp(key, "RHOLD") == 0) g_returnHoldMs = (unsigned long)value;
    else if (strcmp(key, "RHOLDSW") == 0) g_returnWechselMs = (unsigned long)value;
    else if (strcmp(key, "INVERT") == 0) g_invertLogic = (value != 0);
    else if (strcmp(key, "USTHRESHMM") == 0) g_usThresholdMm = (unsigned long)value;
    else if (strcmp(key, "USCONFIRM") == 0) g_usConfirmCount = (uint8_t)constrain(value, 1, 20);
    else if (strcmp(key, "USINTERVAL") == 0) g_usIntervalMs = (unsigned long)constrain(value, 3, 500);
    else if (strcmp(key, "USECHOTIMEOUT") == 0) g_usEchoTimeoutUs = (unsigned long)constrain(value, 500, 8000);
    else if (strcmp(key, "MAXBLOCKMS") == 0) g_maxSingleDuckBlockMs = (unsigned long)value;
    else if (strcmp(key, "NFCTIMEOUT") == 0) g_nfcTimeoutMs = (unsigned long)constrain(value, 10, 2000);
    else if (strcmp(key, "NFCRETRIES") == 0) {
      g_nfcRetries = (uint8_t)constrain(value, 1, 255);
      if (nfcReady) nfc.setPassiveActivationRetries(g_nfcRetries);
    }
    else ok = false;

    if (!ok) { sendNack(token, F("UNKNOWN_KEY")); return; }
    sendAck(token, F("CONFIG"));
    sendCfg();
    return;
  }

  if (strcmp(cmd, "NFCMODE") == 0) {
    if (strcmp(modeArg, "CONTINUOUS") == 0) {
      nfcMode = NFC_MODE_CONTINUOUS;
      sendAck(token, F("NFCMODE"));
      sendState();
    } else if (strcmp(modeArg, "DUCKONLY") == 0) {
      nfcMode = NFC_MODE_DUCKONLY;
      sendAck(token, F("NFCMODE"));
      sendState();
    } else {
      sendNack(token, F("BAD_MODE"));
    }
    return;
  }

  if (strcmp(cmd, "ARM") == 0) {
    if (side == 'L' || side == 'R' || side == 'N') {
      armedSide = side;
      armedIsSwitch = (switchFlag == 1);
      sendAck(token, F("ARM"));
      sendState();
    } else {
      sendNack(token, F("BAD_SIDE"));
    }
    return;
  }

  if (strcmp(cmd, "MOTOR") == 0) {
    if (fast < 0 || fast > 255 || slow < 0 || slow > 255) {
      sendNack(token, F("BAD_PWM"));
      return;
    }
    pwmFast = (uint8_t)fast;
    pwmSlow = (uint8_t)slow;
    analogWrite(PIN_MOTOR_SCHNELL, pwmFast);
    analogWrite(PIN_MOTOR_LANGSAM, pwmSlow);
    sendAck(token, F("MOTOR"));
    sendState();
    return;
  }

  if (strcmp(cmd, "SERVOUS") == 0) {
    if (servoUs < 0) { sendNack(token, F("BAD_US")); return; }
    if (servoActionState != SERVO_IDLE || qCount > 0) {
      sendNack(token, F("SERVO_BUSY"));
      return;
    }
    driveServo(clampServo((int)servoUs));
    sendAck(token, F("SERVOUS"));
    return;
  }

  if (strcmp(cmd, "KICK") == 0) {
    if (!(side == 'L' || side == 'R')) { sendNack(token, F("BAD_SIDE")); return; }
    bool ok = enqueueKick(side, 999999, millis() + 50, false);
    if (!ok) { sendNack(token, F("QUEUE_FULL")); return; }
    sendAck(token, F("KICK"));
    return;
  }

  if (strcmp(cmd, "SIMTAG") == 0) {
    if (!TEST_MODE) { sendNack(token, F("TESTMODE_OFF")); return; }
    if (uid[0] == '\0') { sendNack(token, F("NO_UID")); return; }

    strncpy(lastUid, uid, sizeof(lastUid) - 1);
    lastUid[sizeof(lastUid) - 1] = '\0';
    sendAck(token, F("SIMTAG"));
    sendTagEvent(uid);

    if (duck.active && !duck.uidValid) {
      strncpy(duck.uidHex, uid, sizeof(duck.uidHex) - 1);
      duck.uidHex[sizeof(duck.uidHex) - 1] = '\0';
      duck.uidValid = true;
    }
    return;
  }

  if (strcmp(cmd, "LED") == 0) {
    digitalWrite(PIN_LED, side == 'L' ? HIGH : LOW);
    sendAck(token, F("LED"));
    return;
  }

  sendNack(token, F("UNKNOWN_CMD"));
}

void readSerialLines() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxLine[rxPos] = '\0';
      if (rxPos > 0) {
        handleCommand(rxLine);
        wdt_reset();
      }
      rxPos = 0;
      continue;
    }
    if (rxPos < sizeof(rxLine) - 1) rxLine[rxPos++] = c;
    else rxPos = 0;
  }
}

void setup() {
  wdt_disable();

  Serial.begin(115200);

  pinMode(PIN_US1_TRIG, OUTPUT);
  pinMode(PIN_US1_ECHO, INPUT);
  pinMode(PIN_US2_TRIG, OUTPUT);
  pinMode(PIN_US2_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  pinMode(PN532_IRQ, INPUT_PULLUP);

  digitalWrite(PIN_US1_TRIG, LOW);
  digitalWrite(PIN_US2_TRIG, LOW);
  digitalWrite(PIN_LED, LOW);

  analogWrite(PIN_MOTOR_SCHNELL, pwmFast);
  analogWrite(PIN_MOTOR_LANGSAM, pwmSlow);

  driveServo(g_posRestLeft);

  setupPn532();
  delay(100);
  sendHello();
  sendState();
  sendCfg();

  wdt_enable(WATCHDOG_TIMEOUT);
}

void loop() {
  wdt_reset();

  readSerialLines();
  updateUltrasonicSensors();
  maybeRetryNfcInit();
  pollNfc();
  updateKickExecutor();
  updateServoAutoDetach();
  checkDuckStuck();

  unsigned long now = millis();
  if (now - lastHelloMs >= 5000) {
    lastHelloMs = now;
    sendHello();
  }
}
