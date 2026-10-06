// Minimaltest fuer die beiden Ultraschallsensoren (HC-SR04-kompatibel).
// Gibt fortlaufend die gemessene Distanz beider Sensoren in cm aus, ohne
// jede weitere Projektlogik - reine Hardwarepruefung.
//
// Pinbelegung identisch zum Hauptprojekt:
//   Sensor 1 (LS1): TRIG = Pin 11, ECHO = Pin 12
//   Sensor 2 (LS2): TRIG = Pin 3,  ECHO = Pin 2
//
// Vorgehen:
// 1. Sketch auf den NEUEN Nano hochladen.
// 2. Serial Monitor bei 115200 Baud oeffnen.
// 3. Hand/Gegenstand vor jeweils EINEN Sensor halten und beobachten, ob
//    sich der zugehoerige cm-Wert plausibel aendert.
//
// Erwartetes Verhalten bei einem FUNKTIONSFAEHIGEN Sensor:
//   - Ohne Hindernis: Wert schwankt meist zwischen ca. 20 und 100+ cm
//     (je nach Umgebung), oder "kein Echo", wenn nichts in Reichweite ist.
//   - Mit Hindernis in ca. 5-30cm Abstand: Wert folgt der Handbewegung
//     nahezu in Echtzeit.
//
// Hinweis auf einen DEFEKTEN Sensor:
//   - Wert bleibt dauerhaft bei "kein Echo", unabhaengig vom Abstand.
//   - Wert ist dauerhaft festgefroren auf einer Zahl, die sich nie aendert.
//   - Wert springt willkuerlich/unplausibel (z.B. staendig 0.0 oder extrem
//     grosse Zahlen ohne jeden Bezug zur Realitaet).

#define PIN_US1_TRIG 11
#define PIN_US1_ECHO 12
#define PIN_US2_TRIG 3
#define PIN_US2_ECHO 2

// Grosszuegiger Timeout fuer den reinen Hardwaretest (deckt bis knapp 4m
// ab) - im Hauptprojekt ist dieser Wert wegen der montierten Schirme viel
// kleiner, hier soll aber unabhaengig von Schirmen getestet werden.
const unsigned long ECHO_TIMEOUT_US = 25000UL;

float measureDistanceCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long dauer = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);
  if (dauer == 0) return -1.0;
  return dauer / 58.0;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_US1_TRIG, OUTPUT);
  pinMode(PIN_US1_ECHO, INPUT);
  pinMode(PIN_US2_TRIG, OUTPUT);
  pinMode(PIN_US2_ECHO, INPUT);

  digitalWrite(PIN_US1_TRIG, LOW);
  digitalWrite(PIN_US2_TRIG, LOW);

  delay(200);
  Serial.println(F("US-Sensor-Minimaltest gestartet."));
}

void loop() {
  float cm1 = measureDistanceCm(PIN_US1_TRIG, PIN_US1_ECHO);
  delay(60); // kurze Pause, damit sich die beiden Sensoren nicht stoeren

  float cm2 = measureDistanceCm(PIN_US2_TRIG, PIN_US2_ECHO);
  delay(60);

  Serial.print(F("LS1 = "));
  if (cm1 < 0.0) Serial.print(F("kein Echo"));
  else Serial.print(cm1, 1);

  Serial.print(F(" cm   |   LS2 = "));
  if (cm2 < 0.0) Serial.print(F("kein Echo"));
  else Serial.print(cm2, 1);
  Serial.println(F(" cm"));

  delay(150);
}
