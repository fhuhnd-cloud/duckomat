#ifndef DUCK_TYPES_H
#define DUCK_TYPES_H

#include <Arduino.h>

enum NfcMode {
  NFC_MODE_CONTINUOUS,
  NFC_MODE_DUCKONLY
};

enum ServoActionState {
  SERVO_IDLE,
  SERVO_HIT,
  SERVO_RETURN
};

enum DuckResult {
  RES_KICK_L,
  RES_KICK_R,
  RES_QUEUE_FULL,
  RES_UNARMED,
  RES_UNREADABLE
};

struct UsStatus {
  bool blocked;
  bool valid;
  uint8_t confirmCounter;
  bool rawBlocked;
  bool anomalyNoEcho;
};

struct DuckCtx {
  bool active;
  uint32_t seq;
  unsigned long startTimeMs;
  unsigned long us2TimeMs;
  bool nfcFound;
  char uid[25];
};

struct KickJob {
  bool used;
  char side;
  uint32_t seq;
  unsigned long dueMs;
  bool isSwitch;
};

#endif // DUCK_TYPES_H