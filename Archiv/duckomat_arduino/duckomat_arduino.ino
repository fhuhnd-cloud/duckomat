#include <Wire.h>
#include <Servo.h>
#include <Adafruit_PN532.h>

// ==========================================
// PIN-BELEGUNG & HARDWARE KONFIGURATION
// ==========================================
#define PIN_LS 2       // Lichtschranke
#define PIN_SERVO 9    // Servo PWM Signal

// PN532 über I2C konfigurieren (A4=SDA, A5=SCL)
#define PN532_IRQ   (3)
#define PN532_RESET (4)
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// ==========================================
// SERVO & KICKER EINSTELLUNGEN
// ==========================================
Servo kickerServo;

// Winkel in Grad (0-180). Diese Werte musst du in der Praxis an deine Mechanik anpassen!
const int POS_UP_LEFT = 40;   // Klöppel ist sicher oben links geparkt
const int POS_UP_RIGHT = 130; // Klöppel ist sicher oben rechts geparkt
const int POS_CENTER = 90;    // Kick-Durchgang (Mitte)

int current_pos = POS_UP_RIGHT; // Startposition
int target_kick_pos = POS_UP_LEFT; 

// ==========================================
// TIMING & STATE MACHINE
// ==========================================
enum State { 
  IDLE, 
  READING_RFID, 
  WAITING_PI, 
  SWEEPING_SERVO, 
  TIMING_KICK, 
  KICKING 
};
State currentState = IDLE;

unsigned long stateTimer = 0;
unsigned long lastSweepTime = 0;
unsigned long lastReadAttempt = 0;

// WICHTIG: Die Zeit in Millisekunden, die die Ente von der 
// Lichtschranke bis unter den Klöppel braucht. (Muss getestet werden!)
const int DELAY_TILL_KICK_MS = 350; 
long seqId = 0;

void setup() {
  Serial.begin(115200);
  
  // Lichtschranke: PULLUP bedeutet, der Pin ist standardmäßig HIGH (5V). 
  // Löst die Lichtschranke aus, zieht das Modul den Pin auf LOW (0V).
  pinMode(PIN_LS, INPUT_PULLUP);
  
  // Servo initialisieren und in Startposition fahren
  kickerServo.attach(PIN_SERVO);
  kickerServo.write(current_pos);

  // PN532 initialisieren
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("SYS:ERROR:PN532_NOT_FOUND");
    while (1); // Endlosschleife, wenn Modul fehlt
  }
  
  // Konfiguriere PN532, um ISO14443A (NTAG213) Tags zu lesen
  nfc.SAMConfig(); 
  Serial.println("SYS:READY");
}

void loop() {
  switch(currentState) {
    
    // ----------------------------------------------------
    // ZUSTAND 1: Warten auf eine Ente
    // ----------------------------------------------------
    case IDLE:
      // LOW bedeutet Objekt erkannt (bei den meisten IR-Modulen)
      if (digitalRead(PIN_LS) == LOW) {
        seqId++;
        stateTimer = millis();
        currentState = READING_RFID;
      }
      break;

    // ----------------------------------------------------
    // ZUSTAND 2: PN532 intensiv abfragen
    // ----------------------------------------------------
    case READING_RFID: {
      uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
      uint8_t uidLength;
      
      // Lese-Versuch mit kurzem Timeout (z.B. 40ms pro Versuch)
      // Das blockiert den Arduino kurz, was hier aber gewollt ist.
      if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 40)) {
        
        // TAG GEFUNDEN! Sende an den Pi: REQ:1:04A1B2...
        Serial.print("REQ:"); Serial.print(seqId); Serial.print(":");
        for (uint8_t i=0; i < uidLength; i++) {
          if (uid[i] < 0x10) Serial.print("0"); // Führende Null für sauberes Hex-Format
          Serial.print(uid[i], HEX);
        }
        Serial.println(); // Zeilenumbruch markiert Ende der Nachricht
        
        stateTimer = millis();
        currentState = WAITING_PI;
        
      } else if (millis() - stateTimer > 200) {
        // TIMEOUT (200ms vergangen): Ente ist durch, aber kein Tag gefunden
        // Sende Fehler an den Pi, damit dieser den Kick_L/R Befehl geben kann.
        Serial.print("ERR:"); Serial.print(seqId); Serial.println(":NOTAG");
        stateTimer = millis();
        currentState = WAITING_PI;
      }
      break;
    }

    // ----------------------------------------------------
    // ZUSTAND 3: Warten auf Antwort vom Raspberry Pi
    // ----------------------------------------------------
    case WAITING_PI:
      if (Serial.available()) {
        String antwort = Serial.readStringUntil('\n');
        antwort.trim(); // Entferne Leerzeichen/Zeilenumbrüche
        
        if (antwort.indexOf("PASS") >= 0) {
          // Ente ist im System hinterlegt. Klöppel bleibt oben, Ente rutscht durch.
          // Wir warten kurz (Debounce), bis die Lichtschranke wieder frei ist.
          delay(200); 
          currentState = IDLE; 
        } 
        else if (antwort.indexOf("KICK_L") >= 0) {
          target_kick_pos = POS_UP_LEFT;
          // Wenn Servo aktuell rechts steht, fahre ihn erst schonend nach rechts rüber (Startposition für Kick)
          if (current_pos != POS_UP_RIGHT) {
            currentState = SWEEPING_SERVO;
          } else { 
            stateTimer = millis(); 
            currentState = TIMING_KICK; 
          }
        }
        else if (antwort.indexOf("KICK_R") >= 0) {
          target_kick_pos = POS_UP_RIGHT;
          if (current_pos != POS_UP_LEFT) {
            currentState = SWEEPING_SERVO;
          } else { 
            stateTimer = millis(); 
            currentState = TIMING_KICK; 
          }
        }
      }
      // Fallback: Pi ist abgestürzt oder antwortet nicht
      else if (millis() - stateTimer > 500) {
        // Sicherer Fallback: Versuche die Ente nach links wegzukicken
        target_kick_pos = POS_UP_LEFT;
        stateTimer = millis();
        currentState = TIMING_KICK;
      }
      break;

    // ----------------------------------------------------
    // ZUSTAND 4: Servo schonend in Position bringen (Materialschonung)
    // ----------------------------------------------------
    case SWEEPING_SERVO:
      // Fährt den Servo in 1-Grad-Schritten alle 4ms
      if (millis() - lastSweepTime > 4) {
        // Bestimme das Ziel der Parkposition (gegenüber der Ziel-Kick-Richtung)
        int park_pos = (target_kick_pos == POS_UP_LEFT) ? POS_UP_RIGHT : POS_UP_LEFT;
        int step = (current_pos < park_pos) ? 1 : -1; 
        
        current_pos += step;
        kickerServo.write(current_pos);
        lastSweepTime = millis();
        
        if (current_pos == park_pos) {
          stateTimer = millis();
          currentState = TIMING_KICK;
        }
      }
      break;

    // ----------------------------------------------------
    // ZUSTAND 5: Warten bis die Ente auf Kick-Höhe ist
    // ----------------------------------------------------
    case TIMING_KICK:
      if (millis() - stateTimer >= DELAY_TILL_KICK_MS) {
        // JETZT! Harter Kick!
        kickerServo.write(target_kick_pos); 
        current_pos = target_kick_pos;
        stateTimer = millis();
        currentState = KICKING;
      }
      break;

    // ----------------------------------------------------
    // ZUSTAND 6: Kick abschließen
    // ----------------------------------------------------
    case KICKING:
      if (millis() - stateTimer > 300) { // Gib dem Servo 300ms für die Bewegung
        // Kick beendet. Der Klöppel bleibt einfach in der neuen Endposition liegen
        // und wartet auf die nächste Ente.
        currentState = IDLE; 
      }
      break;
  }
}