#ifndef DUCK_TYPES_H
#define DUCK_TYPES_H

#include <Arduino.h>

struct UsStatus {
  bool stableBlocked;
  bool candidateBlocked;
  uint8_t candidateCount;
  bool hasCandidate;
  bool rawBlocked;
};

struct DuckCtx {
  bool active;
  uint32_t seq;
  unsigned long startMs;
  unsigned long ls2Ms;
  bool uidValid;
  char uidHex[24];
};

struct KickJob {
  bool used;
  char side;
  uint32_t seq;
  unsigned long dueMs;
  bool isSwitch;  // NEU: true = letzter Kick vor Seitenwechsel -> kuerzere Rueckkehrzeit
};

enum ServoActionState { SERVO_IDLE, SERVO_HIT, SERVO_RETURN };
enum DuckResult { RES_KICK_L, RES_KICK_R, RES_QUEUE_FULL, RES_UNARMED, RES_UNREADABLE };
enum NfcMode { NFC_MODE_CONTINUOUS, NFC_MODE_DUCKONLY };

#endif
