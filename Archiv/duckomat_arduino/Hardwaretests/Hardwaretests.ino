#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ServoTimer2.h>  // --- Alternative Servo-Bibliothek ---

// --- PIN-DEFINITIONEN ---
#define PIN_LS1 2       
#define PIN_LS2 3       
#define PIN_SERVO 5     
#define PIN_LED 6       

#define PIN_MOTOR_SCHNELL 9   // ENA (Schnelles Band) 
#define PIN_MOTOR_LANGSAM 10  // ENB (Langsames Band) 
#define PIN_RELAIS1 11        
#define PIN_RELAIS2 12        

#define PN532_IRQ   8   
#define PN532_RESET 7   

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
ServoTimer2 myServo; 

// --- GLOBALE VARIABLEN ---
int servoLinks = 990; // 45°
int servoMitte = 1510; // 90°
int servoRechts = 2030; // 135°

int speedSchnell = 0;   
int speedLangsam = 0;   
bool relais1An = false;
bool relais2An = false;
bool ledAn = true;     

// Variablen für den neuen Klöppel-Test
int kloeppelDelay = 680;     // Delay nach LS2 in ms
int kloeppelTarget = servoLinks;   // Startet mit "Links" 

// MotSchnell: 255 | MotLangsam: 110 | Delay LS2(ms): 420 | Target Position: 1690

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(PIN_LS1, INPUT_PULLUP);
  pinMode(PIN_LS2, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  
  pinMode(PIN_MOTOR_SCHNELL, OUTPUT);
  pinMode(PIN_MOTOR_LANGSAM, OUTPUT);
  pinMode(PIN_RELAIS1, OUTPUT);
  pinMode(PIN_RELAIS2, OUTPUT);

  digitalWrite(PIN_RELAIS1, HIGH); 
  digitalWrite(PIN_RELAIS2, HIGH); 
  digitalWrite(PIN_LED, LOW);
  
  analogWrite(PIN_MOTOR_SCHNELL, 0);
  analogWrite(PIN_MOTOR_LANGSAM, 0);

  Serial.println(F("\n========================================="));
  Serial.println(F("   HARDWARE-CHECK LÄUFT..."));
  Serial.println(F("========================================="));

  myServo.attach(PIN_SERVO);
  myServo.write(servoLinks);
  delay(500); // Kurz Zeit geben
  myServo.detach(); // Sofort detachen gegen Zucken
  Serial.println(F("[OK] PWM-Signal an D5 fuer Servo aktiviert (via Timer2)"));

  Serial.print(F("[OK] LS1 (D2) Status: ")); 
  Serial.println(digitalRead(PIN_LS1) == LOW ? F("BLOCKIERT") : F("FREI"));
  Serial.print(F("[OK] LS2 (D3) Status: ")); 
  Serial.println(digitalRead(PIN_LS2) == LOW ? F("BLOCKIERT") : F("FREI"));
  Serial.println(F("[OK] Motoren (D9/D10) & 2-Kanal Relais (D11/D12) initialisiert"));

  Serial.print(F("Pruefe RFID-Modul... "));
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (versiondata) {
    nfc.SAMConfig();
    Serial.print(F("[OK] Gefunden. Firmware: "));
    Serial.println((versiondata>>16) & 0xFF, HEX);
  } else {
    Serial.println(F("[FEHLER] PN532 antwortet nicht!"));
  }
  
  delay(1000);
  zeigeMenu();
}

void loop() {
  if (Serial.available() > 0) {
    char eingabe = Serial.read();
    switch (eingabe) {
      case '1': testLichtschrankenKalibrierung(); break;
      case '2': testLichtschrankenFrequenz(); break;
      case '3': testRfidFrequenz(); break;
      case '4': testServoKalibrierung(); break;
      case '5': testServoGeschwindigkeit(); break;
      case '6': testBandgeschwindigkeit(); break;
      case '7': testRfidLesen(); break;
      case '8': testMotoren(); break;
      case 'k': case 'K': testKloeppelTiming(); break; // NEUER TEST
      case '9': toggleRelais1(); break;
      case '0': toggleRelais2(); break;
      case 'l': case 'L': toggleLED(); break; 
      case '\n': case '\r': break;
      default: 
        Serial.println(F("Ungueltige Eingabe.")); 
        zeigeMenu(); 
        break;
    }
  }
}

void zeigeMenu() {
  Serial.println(F("\n========================================="));
  Serial.println(F("   HAUPTMENÜ: WERKSTATT-TESTS"));
  Serial.println(F("========================================="));
  Serial.println(F("[1] Lichtschranken kalibrieren"));
  Serial.println(F("[2] Lesefrequenz Lichtschranken"));
  Serial.println(F("[3] Lesefrequenz RFID-Reader"));
  Serial.println(F("[4] Servo kalibrieren"));
  Serial.println(F("[5] Servo-Geschwindigkeit messen"));
  Serial.println(F("[6] Bandgeschwindigkeit messen"));
  Serial.println(F("[7] RFID-Tags Zaehler"));
  Serial.println(F("[8] Motoren-Geschwindigkeit (D9/D10)"));
  Serial.println(F("[K] Klöppel-Timing Test (Auswurf über LS2)")); // NEU
  Serial.println(F("[9] Relais Kanal 1 (D11) umschalten"));
  Serial.println(F("[0] Relais Kanal 2 (D12) umschalten"));
  Serial.println(F("[L] Status-LED (D6) umschalten"));
  Serial.println(F("-----------------------------------------"));
  Serial.println(F("Sende Zeichen, um Aktion zu starten!"));
}

// --- NEUER TEST: KLÖPPEL TIMING ---
void testKloeppelTiming() {
  Serial.println(F("\n--- [K] KLÖPPEL-TIMING TEST ---"));
  
  // Grundposition für diesen Test einnehmen (Rechts)
  myServo.attach(PIN_SERVO);
  myServo.write(servoRechts);
  delay(500);
  myServo.detach();
  
  kloeppelTarget = servoRechts; // Reset auf "Links"

  druckeKloeppelConfig();
  Serial.println(F("\nBEDIENUNG:"));
  Serial.println(F("'q'/'a' = Speed Schnell +/-"));
  Serial.println(F("'w'/'s' = Speed Langsam +/-"));
  Serial.println(F("'e'/'d' = Delay nach LS2 +/- 10ms"));
  Serial.println(F("'r'/'f' = Auswurfposition (Target) +/- 20us"));
  Serial.println(F("'x'     = Beenden und Motoren stoppen"));
  Serial.println(F("\nLEGE NUN OBJEKTE AUF DAS BAND. WARTE AUF LS2..."));

  while(Serial.available()) Serial.read(); // Puffer leeren
  
  bool waitingForServo = false;
  unsigned long triggerTime = 0;

  while(true) {
    // 1. Serielle Eingaben prüfen (Parameter on the fly ändern)
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') { 
        analogWrite(PIN_MOTOR_SCHNELL, 0); 
        analogWrite(PIN_MOTOR_LANGSAM, 0); 
        zeigeMenu(); 
        return; 
      }
      else if (c == 'q') { speedSchnell += 10; if(speedSchnell > 255) speedSchnell = 255; druckeKloeppelConfig(); }
      else if (c == 'a') { speedSchnell -= 10; if(speedSchnell < 0) speedSchnell = 0; druckeKloeppelConfig(); }
      else if (c == 'w') { speedLangsam += 10; if(speedLangsam > 255) speedLangsam = 255; druckeKloeppelConfig(); }
      else if (c == 's') { speedLangsam -= 10; if(speedLangsam < 0) speedLangsam = 0; druckeKloeppelConfig(); }
      else if (c == 'e') { kloeppelDelay += 10; druckeKloeppelConfig(); }
      else if (c == 'd') { kloeppelDelay -= 10; if(kloeppelDelay < 0) kloeppelDelay = 0; druckeKloeppelConfig(); }
      else if (c == 'r') { kloeppelTarget += 20; if(kloeppelTarget > 2200) kloeppelTarget = 2200; druckeKloeppelConfig(); }
      else if (c == 'f') { kloeppelTarget -= 20; if(kloeppelTarget < 800) kloeppelTarget = 800; druckeKloeppelConfig(); }
      
      // Motoren sofort anpassen
      analogWrite(PIN_MOTOR_SCHNELL, speedSchnell);
      analogWrite(PIN_MOTOR_LANGSAM, speedLangsam);
    }

    // 2. Lichtschranke überwachen
    if (!waitingForServo) {
      if (digitalRead(PIN_LS2) == LOW) {
        delay(20); // Debounce
        if (digitalRead(PIN_LS2) == LOW) {
          triggerTime = millis();
          waitingForServo = true;
          Serial.println(F("[TRIGGER] LS2 blockiert. Warte auf Delay..."));
        }
      }
    } 
    // 3. Servo-Auswurf-Sequenz ausführen
    else {
      if (millis() - triggerTime >= kloeppelDelay) {
        Serial.println(F(" -> KLÖPPEL FEUER! (Auswerfen)"));
        
        myServo.attach(PIN_SERVO);
        myServo.write(kloeppelTarget); // Fahre zur Zielposition (Links)
        delay(280);                    // Warte exakt 280ms für vollen Weg
        
        Serial.println(F(" -> KLÖPPEL ZURÜCK!"));
        myServo.write(servoLinks);    // Zurück in die Ruheposition (Rechts)
        delay(280);                    // Warte Rückweg ab
        myServo.detach();              // Strom wegnehmen gegen Zucken
        
        waitingForServo = false;
        
        // Verhindern, dass eine lange Ente sofort wieder triggert (Warten bis LS2 wieder frei ist)
        while(digitalRead(PIN_LS2) == LOW) { delay(10); }
        Serial.println(F("--- BEREIT FÜR NÄCHSTE ENTE ---"));
      }
    }
  }
}

void druckeKloeppelConfig() {
  Serial.print(F("MotSchnell: ")); Serial.print(speedSchnell);
  Serial.print(F(" | MotLangsam: ")); Serial.print(speedLangsam);
  Serial.print(F(" | Delay LS2(ms): ")); Serial.print(kloeppelDelay);
  Serial.print(F(" | Target Position: ")); Serial.println(kloeppelTarget);
}

// --------------------------------------------------------
// Die restlichen Funktionen bleiben wie gehabt...
// --------------------------------------------------------

void toggleLED() {
  ledAn = !ledAn;
  digitalWrite(PIN_LED, ledAn ? HIGH : LOW);
  Serial.println(F("\n--- [L] STATUS-LED UMGESCHALTET ---"));
  Serial.print(F("Status D6: ")); Serial.println(ledAn ? F("AN (HIGH)") : F("AUS (LOW)"));
  zeigeMenu();
}

void testMotoren() {
  Serial.println(F("\n--- [8] MOTOREN STEUERUNG ---"));
  Serial.println(F("Schnelles Band (D9): 'q' (schneller), 'a' (langsamer)"));
  Serial.println(F("Langsames Band (D10): 'w' (schneller), 's' (langsamer)"));
  Serial.println(F("'e' = Beide stoppen | 'x' = Zurueck (Motoren laufen weiter!)"));
  
  while(Serial.available()) Serial.read(); 

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') { zeigeMenu(); return; }
      else if (c == 'q') { speedSchnell += 15; if(speedSchnell > 255) speedSchnell = 255; }
      else if (c == 'a') { speedSchnell -= 15; if(speedSchnell < 0) speedSchnell = 0; }
      else if (c == 'w') { speedLangsam += 15; if(speedLangsam > 255) speedLangsam = 255; }
      else if (c == 's') { speedLangsam -= 15; if(speedLangsam < 0) speedLangsam = 0; }
      else if (c == 'e') { speedSchnell = 0; speedLangsam = 0; } 
      
      if(c == 'q' || c == 'a' || c == 'w' || c == 's' || c == 'e') {
          analogWrite(PIN_MOTOR_SCHNELL, speedSchnell);
          analogWrite(PIN_MOTOR_LANGSAM, speedLangsam);
          Serial.print(F("PWM Schnell (D9): ")); Serial.print(speedSchnell);
          Serial.print(F(" | PWM Langsam (D10): ")); Serial.println(speedLangsam);
      }
    }
  }
}

void toggleRelais1() {
  relais1An = !relais1An;
  digitalWrite(PIN_RELAIS1, relais1An ? LOW : HIGH); 
  Serial.println(F("\n--- [9] RELAIS 1 UMGESCHALTET ---"));
  Serial.print(F("Status D11: ")); Serial.println(relais1An ? F("AN (LOW)") : F("AUS (HIGH)"));
  zeigeMenu();
}

void toggleRelais2() {
  relais2An = !relais2An;
  digitalWrite(PIN_RELAIS2, relais2An ? LOW : HIGH); 
  Serial.println(F("\n--- [0] RELAIS 2 UMGESCHALTET ---"));
  Serial.print(F("Status D12: ")); Serial.println(relais2An ? F("AN (LOW)") : F("AUS (HIGH)"));
  zeigeMenu();
}

void testLichtschrankenKalibrierung() {
  Serial.println(F("\n--- TEST 1: LS KALIBRIEREN ---"));
  Serial.println(F("Sende 'x' zum Beenden."));
  while (true) {
    if (Serial.available() && Serial.read() == 'x') { zeigeMenu(); return; }
    bool ls1 = (digitalRead(PIN_LS1) == LOW);
    bool ls2 = (digitalRead(PIN_LS2) == LOW);
    Serial.print(F("LS1: ")); Serial.print(ls1 ? F("BLOCKIERT [X]") : F("FREI [ ]"));
    Serial.print(F("  |  LS2: ")); Serial.println(ls2 ? F("BLOCKIERT [X]") : F("FREI [ ]"));
    digitalWrite(PIN_LED, (ls1 || ls2) ? HIGH : LOW);
    delay(200);
  }
}

void testLichtschrankenFrequenz() {
  Serial.println(F("\n--- TEST 2: LS FREQUENZ (Hz) ---"));
  delay(1000);
  unsigned long start = millis();
  unsigned long counter = 0;
  volatile int val1, val2; 
  while (millis() - start < 1000) {
    val1 = digitalRead(PIN_LS1);
    val2 = digitalRead(PIN_LS2);
    counter++;
  }
  Serial.print(F("Abfragen pro Sekunde (beide Sensoren): ")); Serial.println(counter);
  zeigeMenu();
}

void testRfidFrequenz() {
  Serial.println(F("\n--- TEST 3: RFID FREQUENZ (Hz) ---"));
  delay(1000);
  unsigned long start = millis();
  unsigned long counter = 0;
  uint8_t uid[] = { 0,0,0,0,0,0,0 };
  uint8_t uidLen;
  while (millis() - start < 1000) {
    nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 10);
    counter++;
  }
  Serial.print(F("RFID-Lesezyklen pro Sekunde: ")); Serial.println(counter);
  zeigeMenu();
}

void testServoKalibrierung() {
  Serial.println(F("\n--- TEST 4: SERVO KALIBRIEREN ---"));
  Serial.println(F("'a'=Links, 'b'=Mitte, 'c'=Rechts | '+' und '-' zum Justieren | 'p'=Code ausgeben | 'x'=Ende"));
  char pos = 'a'; 
  myServo.attach(PIN_SERVO);
  myServo.write(servoLinks);
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') { myServo.detach(); zeigeMenu(); return; }
      else if (c == 'a') { pos = 'a'; myServo.write(servoLinks); Serial.print(F("LINKS: ")); Serial.println(servoLinks); }
      else if (c == 'b') { pos = 'b'; myServo.write(servoMitte); Serial.print(F("MITTE: ")); Serial.println(servoMitte); }
      else if (c == 'c') { pos = 'c'; myServo.write(servoRechts); Serial.print(F("RECHTS: ")); Serial.println(servoRechts); }
      else if (c == '+') { 
        if (pos == 'a') { servoLinks+=10; myServo.write(servoLinks); Serial.println(servoLinks); }
        if (pos == 'b') { servoMitte+=10; myServo.write(servoMitte); Serial.println(servoMitte); }
        if (pos == 'c') { servoRechts+=10; myServo.write(servoRechts); Serial.println(servoRechts); }
      }
      else if (c == '-') {
        if (pos == 'a') { servoLinks-=10; myServo.write(servoLinks); Serial.println(servoLinks); }
        if (pos == 'b') { servoMitte-=10; myServo.write(servoMitte); Serial.println(servoMitte); }
        if (pos == 'c') { servoRechts-=10; myServo.write(servoRechts); Serial.println(servoRechts); }
      }
      else if (c == 'p') {
        Serial.println(F("int servoLinks = ")); Serial.print(servoLinks); Serial.println(F(";"));
        Serial.println(F("int servoMitte = ")); Serial.print(servoMitte); Serial.println(F(";"));
        Serial.println(F("int servoRechts = ")); Serial.print(servoRechts); Serial.println(F(";"));
      }
    }
  }
}

void testServoGeschwindigkeit() {
  Serial.println(F("\n--- TEST 5: SERVO GESCHWINDIGKEIT ---"));
  Serial.println(F("Pendelt zwischen servoLinks und servoRechts. '+' langsamer, '-' schneller, 'x' Ende."));
  int pauseMs = 400; 
  myServo.attach(PIN_SERVO);
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'x') { myServo.write(servoLinks); delay(300); myServo.detach(); zeigeMenu(); return; }
      else if (c == '+') { pauseMs += 20; }
      else if (c == '-') { pauseMs -= 20; if(pauseMs < 20) pauseMs = 20; }
    }
    Serial.print(F("Pausen-Zeit (ms): ")); Serial.println(pauseMs);
    myServo.write(servoRechts); delay(pauseMs);
    myServo.write(servoLinks); delay(pauseMs);
  }
}

void testBandgeschwindigkeit() {
  Serial.println(F("\n--- TEST 6: BANDGESCHWINDIGKEIT MESSEN ---"));
  Serial.println(F("Bitte Abstand der Lichtschranken in cm eingeben (z.B. 24) und Senden druecken:"));

  while(Serial.available()) Serial.read(); 

  float distance = 0;
  while(distance <= 0) {
    if(Serial.available()) {
      distance = Serial.parseFloat();
    }
  }

  Serial.print(F("=> Abstand gespeichert: ")); Serial.print(distance); Serial.println(F(" cm\n"));
  Serial.println(F("Lege ein Objekt auf das Band. Warte auf LS1 (D2)... Sende 'x' zum Abbrechen."));

  while(true) {
    if(Serial.available() && Serial.read() == 'x') { zeigeMenu(); return; }

    if(digitalRead(PIN_LS1) == LOW) {
      delay(20); 
      if(digitalRead(PIN_LS1) == HIGH) continue; 

      unsigned long t1 = millis();
      Serial.println(F("Objekt bei LS1 erkannt. Messe Zeit..."));

      delay(200); 
      
      bool timeout = false;
      unsigned long t2 = 0;
      
      while(true) {
        if(Serial.available() && Serial.read() == 'x') { zeigeMenu(); return; }
        
        if(digitalRead(PIN_LS2) == LOW) {
          delay(20); 
          if(digitalRead(PIN_LS2) == LOW) {
            t2 = millis(); 
            break; 
          }
        }
        
        if(millis() - t1 > 10000) {
          Serial.println(F("Timeout! Objekt hat LS2 nicht innerhalb von 10s erreicht."));
          timeout = true;
          break;
        }
      }
      
      if(!timeout) {
        float dauerS = (t2 - t1) / 1000.0;
        float speed = distance / dauerS;
        Serial.println(F("Objekt bei LS2 angekommen!"));
        Serial.print(F("Benötigte Zeit : ")); Serial.print(dauerS); Serial.println(F(" Sekunden"));
        Serial.print(F("Geschwindigkeit: ")); Serial.print(speed); Serial.println(F(" cm/s\n"));
      }
      
      while(digitalRead(PIN_LS2) == LOW) { delay(10); }
      Serial.println(F("Bereit fuer naechstes Objekt... Sende 'x' um ins Menue zurueckzukehren."));
      delay(1000); 
    }
  }
}

void testRfidLesen() {
  Serial.println(F("\n--- TEST 7: RFID ZAEHLER ---"));
  Serial.println(F("Zaehlt wie oft ein Tag am Stueck gelesen wird. Sende 'x' zum Beenden."));
  
  uint8_t uid[7];
  uint8_t uidLen;
  
  uint8_t lastUid[7];
  uint8_t lastUidLen = 0;
  unsigned long readCount = 0;
  unsigned long lastReadTime = 0;
  
  while (true) {
    if(Serial.available() && Serial.read() == 'x') { 
      if (readCount > 0) {
         Serial.print(F("Zuletzt gelesen: ")); Serial.print(readCount); Serial.println(F(" mal."));
      }
      zeigeMenu(); 
      return; 
    }
    
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 30);
    
    if (success) {
      bool match = (uidLen == lastUidLen);
      if (match) {
        for (uint8_t i = 0; i < uidLen; i++) {
          if (uid[i] != lastUid[i]) { match = false; break; }
        }
      }
      
      if (match) {
        readCount++;
        lastReadTime = millis();
      } else {
        if (readCount > 0) {
          Serial.print(F("  -> Wurde ")); Serial.print(readCount); Serial.println(F("x in Folge gelesen.\n"));
        }
        Serial.print(F("Neue UID erkannt:"));
        for (uint8_t i = 0; i < uidLen; i++) {
          Serial.print(F(" 0x")); Serial.print(uid[i], HEX);
          lastUid[i] = uid[i]; 
        }
        Serial.println();
        lastUidLen = uidLen;
        readCount = 1;
        lastReadTime = millis();
      }
    } else {
      if (readCount > 0 && (millis() - lastReadTime > 500)) {
         Serial.print(F("  -> Tag hat das Feld verlassen. Wurde ")); 
         Serial.print(readCount); 
         Serial.println(F("x in Folge gelesen.\n"));
         readCount = 0;
         lastUidLen = 0;
      }
    }
  }
}