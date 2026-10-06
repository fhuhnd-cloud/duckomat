// ============================================================
// Minimal-Testprogramm v5: I2C Bus-Recovery & Timeout-Absicherung
// Loest das Einfrieren in Schritt 2 ohne Kabelabziehen.
// ============================================================

#include <Wire.h>
#include <Adafruit_PN532.h>

#define PN532_RESET 7  // Dummy-Pin (physisch nicht verbunden)
#define PN532_IRQ   8  // Dummy-Pin (physisch nicht verbunden)

#define PIN_I2C_SDA A4
#define PIN_I2C_SCL A5

const unsigned long RFID_READ_TIMEOUT_MS = 35UL;
const unsigned long READ_PAUSE_MS = 500UL;

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
bool nfcAktiv = false;

// ------------------------------------------------------------
// Prueft den Bus und taktet eine haengende SDA-Leitung frei
// (Offizieller NXP I2C Bus-Clear Standard)
// ------------------------------------------------------------
void checkAndClearI2CBus() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(50);

  int sdaState = digitalRead(PIN_I2C_SDA);
  int sclState = digitalRead(PIN_I2C_SCL);

  Serial.print(F("[Bus-Check] Vor Init: SDA="));
  Serial.print(sdaState == HIGH ? F("HIGH (Frei)") : F("LOW (BLOCKIERT)"));
  Serial.print(F(" | SCL="));
  Serial.println(sclState == HIGH ? F("HIGH (Frei)") : F("LOW (BLOCKIERT)"));

  // Falls SDA oder SCL auf LOW haengen: Bus-Clear durchfuehren
  if (sdaState == LOW || sclState == LOW) {
    Serial.println(F("[Bus-Clear] Starte 9 Taktimpulse auf SCL zum Freiraeumen..."));
    pinMode(PIN_I2C_SCL, OUTPUT);

    for (uint8_t i = 0; i < 9; i++) {
      digitalWrite(PIN_I2C_SCL, LOW);
      delayMicroseconds(10);
      digitalWrite(PIN_I2C_SCL, HIGH);
      delayMicroseconds(10);
      if (digitalRead(PIN_I2C_SDA) == HIGH) {
        Serial.print(F("[Bus-Clear] SDA nach "));
        Serial.print(i + 1);
        Serial.println(F(" Takten freigegeben!"));
        break;
      }
    }

    // STOP-Bedingung generieren
    pinMode(PIN_I2C_SDA, OUTPUT);
    digitalWrite(PIN_I2C_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_I2C_SDA, HIGH);
    delayMicroseconds(10);

    // Zustaende wieder auf Input setzen
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);
    delayMicroseconds(50);

    Serial.print(F("[Bus-Check] Nach Bus-Clear: SDA="));
    Serial.println(digitalRead(PIN_I2C_SDA) == HIGH ? F("HIGH (Frei)") : F("IMMER NOCH LOW"));
  }
}

void setup() {
  Serial.begin(115200);
  delay(800); // Einschwingzeit fuer Serial und Stromversorgung

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("=== NFC Minimal-Test v5 (Clear & Timeout) ==="));
  Serial.println(F("========================================"));

  // 1. Physischen Bus pruefen und befreien
  checkAndClearI2CBus();

  // 2. Hardware-I2C starten
  Serial.println(F("[Schritt 1/4] Wire.begin() wird aufgerufen..."));
  Wire.begin();

  // Timeout auf 25 ms setzen (verhindert jedes Einfrieren in Wire-Aufrufen)
  #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(25000 /* us */, true /* reset_on_timeout */);
    Serial.println(F("[Schritt 1/4] Hardware-Wire-Timeout (25ms) aktiviert."));
  #endif
  Serial.println(F("[Schritt 1/4] Wire.begin() erfolgreich."));

  // 3. I2C Ping an PN532
  Serial.println(F("[Schritt 2/4] Sende I2C-Ping an Adresse 0x24..."));
  Wire.beginTransmission(0x24);
  byte i2cFehler = Wire.endTransmission();
  Serial.print(F("[Schritt 2/4] Ping abgeschlossen. Rueckgabewert: "));
  Serial.println(i2cFehler);

  if (i2cFehler == 0) {
    Serial.println(F("[Schritt 3/4] PN532 auf 0x24 erkannt. Rufe nfc.begin() auf..."));
    nfc.begin();
    Serial.println(F("[Schritt 3/4] nfc.begin() abgeschlossen."));

    Serial.println(F("[Schritt 4/4] Frage Firmware-Version ab..."));
    uint32_t versiondata = nfc.getFirmwareVersion();

    if (versiondata) {
      Serial.print(F("[Schritt 4/4] Chip erkannt! PN5"));
      Serial.println((versiondata >> 24) & 0xFF, HEX);

      nfc.SAMConfig();
      nfcAktiv = true;
      Serial.println(F("[STATUS] SAM konfiguriert. Reader ist betriebsbereit!"));
    } else {
      Serial.println(F("[FEHLER] PN532 antwortet, liefert aber keine Versionsdaten."));
    }
  } else {
    Serial.print(F("[FEHLER] Kein I2C-Ack bei 0x24. Fehlercode: "));
    Serial.println(i2cFehler);
    Serial.println(F("Bedeutung: 2=NACK Adr, 3=NACK Data, 4=Sonstiger Busfehler, 5=Timeout."));
  }

  Serial.println(F("========================================"));
  if (nfcAktiv) {
    Serial.println(F("Bereit. Halte einen Tag vor den Reader..."));
  } else {
    Serial.println(F("Initialisierung fehlgeschlagen."));
  }
}

void loop() {
  if (!nfcAktiv) {
    delay(1000);
    return;
  }

  uint8_t uid[7];
  uint8_t uidLen = 0;

  bool erfolg = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    uid,
    &uidLen,
    RFID_READ_TIMEOUT_MS
  );

  if (erfolg) {
    Serial.print(F("TAG ERKANNT! Laenge: "));
    Serial.print(uidLen, DEC);
    Serial.print(F(" Bytes | HEX: "));
    for (uint8_t i = 0; i < uidLen; i++) {
      if (uid[i] < 0x10) Serial.print('0');
      Serial.print(uid[i], HEX);
      if (i + 1 < uidLen) Serial.print(':');
    }

    // Dezimal-UID berechnen (Duckomat-Spezifikation)
    uint64_t decUid = 0;
    for (uint8_t i = 0; i < uidLen; i++) {
      decUid = (decUid << 8) | uid[i];
    }
    Serial.print(F(" | DEC: "));

    char decBuffer[25];
    uint64_t temp = decUid;
    uint8_t idx = 0;
    if (temp == 0) {
      decBuffer[idx++] = '0';
    } else {
      char rev[25];
      uint8_t r = 0;
      while (temp > 0) {
        rev[r++] = '0' + (temp % 10);
        temp /= 10;
      }
      while (r > 0) {
        decBuffer[idx++] = rev[--r];
      }
    }
    decBuffer[idx] = '\0';
    Serial.println(decBuffer);
  } else {
    Serial.println(F("... kein Tag erkannt ..."));
  }

  delay(READ_PAUSE_MS);
}