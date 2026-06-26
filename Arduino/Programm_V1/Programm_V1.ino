#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>

// --- PIN-DEFINITIONEN ---
#define PIN_LS1 2       
#define PIN_LS2 3       
#define PIN_SERVO 5     
#define PIN_LED 6       
#define PIN_MOTOR_SCHNELL 9   
#define PIN_MOTOR_LANGSAM 10  
#define PIN_RELAIS1 11        
#define PIN_RELAIS2 12        
#define PN532_IRQ   8   
#define PN532_RESET 7   

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
ServoTimer2 myServo; 

// --- PARAMETER & KONSTANTEN ---
const int pwmSchnell = 255;
const int pwmLangsam = 100;
const int kloeppelDelay = 640; 

const int POS_REST_LEFT   = 990;   
const int POS_KICK_LEFT   = 1330;  
const int POS_Mitte       = 1510;  
const int POS_KICK_RIGHT  = 1690;  
const int POS_REST_RIGHT  = 2030;  

const int timeKick = 280;          
const int timeReturnNormal = 280;  
const int timeReturnWechsel = 150; 

// --- SYSTEM ZUSTÄNDE ---
enum BeltState { STOPPED, RUNNING, CLEARING };
BeltState currentBeltState = STOPPED;

bool restPositionIsLeft = true; 
int badDucksCount = 0;
int goodDucksCount = 0;
unsigned long pauseStartTime = 0;
bool nfcAktiv = false; 

// Nachlauf Band 1 (Schnell)
unsigned long band1StopTime = 0;
const unsigned long BAND1_DELAY_MS = 2000; 

// --- UID TRACKING ---
uint8_t lastUID[7];
uint8_t lastUIDLen = 0;
unsigned long lastUIDTime = 0;

// --- WARTESCHLANGE (QUEUE) ---
#define MAX_DUCKS 10
struct Duck {
  bool active;
  unsigned long ls1Time;
  bool hasRFID;
  bool ls2Triggered;
  unsigned long ls2Time;
};
Duck queue[MAX_DUCKS];

// --- FUSSEL-FILTER & DEBOUNCE FÜR LICHTSCHRANKEN ---
int stateLS1 = 0; // 0=IDLE, 1=CHECK_FUZZ, 2=BLIND_WAIT
unsigned long timerLS1 = 0;

int stateLS2 = 0; 
unsigned long timerLS2 = 0;

const unsigned long FUZZ_FILTER_MS = 25;  // Millisekunden, die das Signal stabil LOW sein muss
const unsigned long DUCK_BLIND_MS = 400;  // Prellschutz nach einer erkannten Ente

int servoState = 0; 
unsigned long servoMoveTimer = 0;
unsigned long servoReturnWaitTime = 280;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  delay(1000); 

  pinMode(PIN_LS1, INPUT_PULLUP);
  pinMode(PIN_LS2, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  
  for(int i=0; i<MAX_DUCKS; i++) queue[i].active = false;

  myServo.attach(PIN_SERVO);
  myServo.write(POS_REST_LEFT);
  delay(500);
  myServo.detach();

  stopBelts();
  
  Serial.println(F("\n========================================="));
  Serial.println(F("BOOT-VORGANG LÄUFT..."));
  
  Serial.print(F("Prüfe Hardware I2C-Bus... "));
  Wire.begin();
  Wire.beginTransmission(0x24); 
  byte error = Wire.endTransmission();
  
  if (error == 0) {
    Serial.println(F("[OK] Gerät auf I2C Bus gefunden!"));
    Serial.print(F("Starte NFC Modul... "));
    
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata) {
      nfc.SAMConfig();
      Serial.print(F("[OK] Firmware: "));
      Serial.println((versiondata>>16) & 0xFF, HEX);
      nfcAktiv = true;
    } else {
      Serial.println(F("[FEHLER] Modul antwortet nicht richtig auf Software-Ebene."));
      nfcAktiv = false;
    }
  } else {
    Serial.println(F("\n[FEHLER] Kein I2C-Gerät gefunden! nfc.begin() blockiert."));
    nfcAktiv = false;
  }

  Serial.println(F("\nSORTIER-LOGIK V3.0 GESTARTET (FUSSEL-FILTER)"));
  Serial.println(F("Status: Bänder GESTOPPT."));
  Serial.println(F("========================================="));
}

void loop() {
  handleSerial();
  
  if (currentBeltState == CLEARING) {
    if (millis() >= band1StopTime) {
      analogWrite(PIN_MOTOR_SCHNELL, 0); 
      currentBeltState = STOPPED;
      pauseStartTime = millis(); 
      Serial.println(F("[INFO] Auswurfband (Schnell) wurde gestoppt. Anlage steht komplett."));
    }
  }
  
  if (currentBeltState == RUNNING || currentBeltState == CLEARING) {
    checkLS1();
    if (nfcAktiv) { checkRFID(); } 
    checkLS2();
    cleanupQueue(); 
    processQueue();
  }
  
  handleServoStateMachine();
}

// -----------------------------------------
// 0. GARBAGE COLLECTION 
// -----------------------------------------
void cleanupQueue() {
  for(int i=0; i<MAX_DUCKS; i++) {
    if (queue[i].active && !queue[i].ls2Triggered) {
      if (millis() - queue[i].ls1Time > 2000) {
        queue[i].active = false;
        Serial.print(F("[WARNUNG] Phantom-Ente in Slot "));
        Serial.print(i);
        Serial.println(F(" gelöscht (Timeout)!"));
      }
    }
  }
}

// -----------------------------------------
// 1. SERIELLE EINGABE (Überarbeitet für JSON vom Raspberry Pi)
// -----------------------------------------
void handleSerial() {
  if (Serial.available()) {
    // Liest die eingehende Zeile bis zum Zeilenumbruch ('\n')
    String payload = Serial.readStringUntil('\n');
    payload.trim(); // Entferne versehentliche Leerzeichen am Anfang und Ende

    // Fall 1: Altes System (Einzelne Buchstaben für die manuelle Steuerung über Terminal)
    if (payload.length() == 1) {
      char c = payload[0];
      if (c == 'n' || c == 'N') {
        Serial.println(F("\n[!!! NOTAUS !!!] Anlage wird hart gestoppt!"));
        stopBelts();
        for(int i=0; i<MAX_DUCKS; i++) { queue[i].active = false; }
        myServo.detach();
        servoState = 0;
        badDucksCount = 0;
        goodDucksCount = 0;
      }
      else if (c == 'b' || c == 'B') {
        if (currentBeltState != STOPPED) {
          Serial.println(F("\n[INFO] Bänder wurden MANUELL GESTOPPT."));
          stopBelts();
        } else {
          Serial.println(F("\n[INFO] Bänder werden GESTARTET..."));
          kompensiereTimer();
          startBelts();
        }
      }
      else if (c == 'g' || c == 'G') {
        Serial.println(F("\n[INFO] Sortierung wird FORTGESETZT..."));
        if (currentBeltState == STOPPED) { kompensiereTimer(); }
        goodDucksCount = 0;
        startBelts();
      }
    } 
    // Fall 2: Neues System (JSON-Befehle vom Raspberry Pi Frontend)
    else if (payload.startsWith("{")) {
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        String type = doc["type"];
        
        // --- AKTUATOREN (Motoren & Bänder) ---
        if (type == "actuator") {
          String name = doc["name"];
          int state = doc["state"];
          
          // Wenn das Kommando für einen der beiden Bänder-Motoren ist
          if (name == "motor_main" || name == "motor_feed") {
            // State 1 = START
            if (state == 1 && currentBeltState == STOPPED) {
              kompensiereTimer();
              startBelts(); 
              Serial.println(F("[JSON] Bänder GESTARTET über Frontend"));
            } 
            // State 0 = STOPP
            else if (state == 0 && currentBeltState != STOPPED) {
              stopBelts();  
              Serial.println(F("[JSON] Bänder GESTOPPT über Frontend"));
            }
          }
        }
        
        // --- EMERGENCY STOP (Not-Aus) ---
        else if (type == "emergency_stop") {
          stopBelts();
          for(int i=0; i<MAX_DUCKS; i++) { queue[i].active = false; }
          myServo.detach();
          servoState = 0;
          Serial.println(F("[JSON] NOT-AUS empfangen über Frontend!"));
        }

        // --- SET MODE (Optional: Inventurmodus etc.) ---
        else if (type == "set_mode") {
            String mode = doc["mode"];
            Serial.print(F("[JSON] Modus gewechselt zu: "));
            Serial.println(mode);
            // Hier könntest du später Code einfügen, wenn der Arduino
            // im Inventur-Modus anders reagieren soll als im Mapping-Modus.
        }

      } else {
        Serial.print(F("[JSON-FEHLER] Konnte Befehl vom Pi nicht lesen: "));
        Serial.println(error.c_str());
      }
    }
  }
}

// -----------------------------------------
// 2. LICHTSCHRANKE 1 (FUSSEL-FILTER)
// -----------------------------------------
void checkLS1() {
  bool currentLS1 = digitalRead(PIN_LS1);
  
  if (stateLS1 == 0) { // WARTEN AUF HINDERNIS
    if (currentLS1 == LOW) {
      stateLS1 = 1;
      timerLS1 = millis(); // Exakter Startzeitpunkt
    }
  } 
  else if (stateLS1 == 1) { // FUSSEL-PRÜFUNG
    if (currentLS1 == HIGH) {
      stateLS1 = 0; // War nur ein Fussel! Ignorieren und zurücksetzen.
    } else {
      if (millis() - timerLS1 >= FUZZ_FILTER_MS) {
        stateLS1 = 2; // Dauerhaftes Hindernis = Ente!
        
        for(int i=0; i<MAX_DUCKS; i++) {
          if(!queue[i].active) {
            // WICHTIG: timerLS1 eintragen, nicht millis()! 
            queue[i] = {true, timerLS1, false, false, 0};
            Serial.print(F("[LS1] Echte Ente angemeldet in Slot ")); Serial.println(i);
            break;
          }
        }
      }
    }
  } 
  else if (stateLS1 == 2) { // BLIND-PHASE (Körper-Prellen ignorieren)
    if (millis() - timerLS1 >= DUCK_BLIND_MS) {
      if (currentLS1 == HIGH) {
        stateLS1 = 0; // Ente ist komplett durch, bereit für die nächste
      }
    }
  }
}

// -----------------------------------------
// 3. RFID CHECK 
// -----------------------------------------
void checkRFID() {
  bool duckWaitingForRFID = false;
  for(int i=0; i<MAX_DUCKS; i++) {
    if(queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered) {
      duckWaitingForRFID = true;
      break;
    }
  }

  if (!duckWaitingForRFID) { return; }

  uint8_t uid[7];
  uint8_t uidLen;
  
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 45)) {
    bool isDuplicate = false;
    if (uidLen == lastUIDLen && (millis() - lastUIDTime < 1000)) {
      isDuplicate = true;
      for (uint8_t i = 0; i < uidLen; i++) {
        if (uid[i] != lastUID[i]) { isDuplicate = false; break; }
      }
    }

    if (!isDuplicate) {
      int targetIdx = -1;
      unsigned long oldest = 0xFFFFFFFF;
      
      for(int i=0; i<MAX_DUCKS; i++) {
        if(queue[i].active && !queue[i].hasRFID && !queue[i].ls2Triggered) {
          if(queue[i].ls1Time < oldest) {
            oldest = queue[i].ls1Time;
            targetIdx = i;
          }
        }
      }
      
      if (targetIdx != -1) {
        queue[targetIdx].hasRFID = true;
        Serial.print(F("[RFID] Tag gelesen! Ente in Slot ")); 
        Serial.print(targetIdx); Serial.println(F(" ist GÜLTIG."));
        
        lastUIDLen = uidLen;
        for (uint8_t i = 0; i < uidLen; i++) { lastUID[i] = uid[i]; }
        lastUIDTime = millis();
      }
    }
  }
}

// -----------------------------------------
// 4. LICHTSCHRANKE 2 (FUSSEL-FILTER)
// -----------------------------------------
void checkLS2() {
  bool currentLS2 = digitalRead(PIN_LS2);
  
  if (stateLS2 == 0) { 
    if (currentLS2 == LOW) {
      stateLS2 = 1;
      timerLS2 = millis(); 
    }
  } 
  else if (stateLS2 == 1) { 
    if (currentLS2 == HIGH) {
      stateLS2 = 0; 
    } else {
      if (millis() - timerLS2 >= FUZZ_FILTER_MS) {
        stateLS2 = 2; 
        
        int targetIdx = -1;
        unsigned long oldest = 0xFFFFFFFF;
        
        for(int i=0; i<MAX_DUCKS; i++) {
          if(queue[i].active && !queue[i].ls2Triggered) {
            // Prüfung: Startzeit der Ente an LS1 vergleichen mit timerLS2
            unsigned long travelTime = timerLS2 - queue[i].ls1Time;
            
            if (travelTime >= 150 && travelTime <= 1200) {
              if(queue[i].ls1Time < oldest) {
                oldest = queue[i].ls1Time;
                targetIdx = i;
              }
            }
          }
        }
        
        if (targetIdx != -1) {
          queue[targetIdx].ls2Triggered = true;
          queue[targetIdx].ls2Time = timerLS2; // Präziser Start des Kloeppel-Timers
          Serial.print(F("[LS2] Echte Ente in Slot ")); Serial.print(targetIdx);
          Serial.println(F(" passiert. Timer (430ms) startet..."));
        } else {
          Serial.println(F("[WARNUNG] LS2 durch Objekt ausgelöst, das nie LS1 passiert hat! (Ignoriert)"));
        }
      }
    }
  } 
  else if (stateLS2 == 2) { 
    if (millis() - timerLS2 >= DUCK_BLIND_MS) {
      if (currentLS2 == HIGH) {
        stateLS2 = 0; 
      }
    }
  }
}

// -----------------------------------------
// 5. QUEUE ABARBEITEN
// -----------------------------------------
void processQueue() {
  if (servoState != 0) return; 
  
  for(int i=0; i<MAX_DUCKS; i++) {
    if (queue[i].active && queue[i].ls2Triggered) {
      if (millis() - queue[i].ls2Time >= kloeppelDelay) {
        
        queue[i].active = false; 
        
        if (queue[i].hasRFID) {
          goodDucksCount++;
          Serial.print(F(">>> GUTE ENTE HAT BAND PASSIERT. Zähler: ")); 
          Serial.println(goodDucksCount);
          
          if (goodDucksCount >= 3 && currentBeltState == RUNNING) {
            analogWrite(PIN_MOTOR_LANGSAM, 0); 
            currentBeltState = CLEARING; 
            band1StopTime = millis() + BAND1_DELAY_MS;
            
            Serial.println(F("!!! 5 GUTE ENTEN PASSIERT !!!"));
            Serial.println(F("Vorband gestoppt. Auswurfband fährt nun leer..."));
          }
        } else {
          badDucksCount++;
          Serial.print(F(">>> DEFEKTE ENTE! Auswurf Nr. "));
          Serial.println(badDucksCount);
          
          servoState = 1; 
          servoMoveTimer = millis();
          
          myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT);
          myServo.attach(PIN_SERVO);
          
          if (restPositionIsLeft) {
             myServo.write(POS_KICK_RIGHT); 
          } else {
             myServo.write(POS_KICK_LEFT);  
          }
        }
        break; 
      }
    }
  }
}

// -----------------------------------------
// 6. SERVO STATE MACHINE
// -----------------------------------------
void handleServoStateMachine() {
  if (servoState == 1) {
    if (millis() - servoMoveTimer >= timeKick) {
      servoState = 2; 
      servoMoveTimer = millis();
      
      if (badDucksCount >= 3) {
        badDucksCount = 0;
        restPositionIsLeft = !restPositionIsLeft; 
        servoReturnWaitTime = timeReturnWechsel;  
        Serial.print(F("[INFO] 3. defekte Ente -> Seitenwechsel zu: "));
        Serial.println(restPositionIsLeft ? F("LINKS (990)") : F("RECHTS (2030)"));
      } else {
        servoReturnWaitTime = timeReturnNormal;   
      }
      
      myServo.write(restPositionIsLeft ? POS_REST_LEFT : POS_REST_RIGHT);
    }
  } 
  else if (servoState == 2) {
    if (millis() - servoMoveTimer >= servoReturnWaitTime) {
      myServo.detach();
      servoState = 0; 
    }
  }
}

// -----------------------------------------
// HILFSFUNKTIONEN
// -----------------------------------------
void startBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, pwmSchnell);
  analogWrite(PIN_MOTOR_LANGSAM, pwmLangsam);
  currentBeltState = RUNNING;
}

void stopBelts() {
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);
  if (currentBeltState != STOPPED) {
    pauseStartTime = millis(); 
    currentBeltState = STOPPED;
  }
}