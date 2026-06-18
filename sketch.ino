#define BLYNK_TEMPLATE_ID   "TMPL2-f1aG56U"
#define BLYNK_TEMPLATE_NAME "Securite Maison"
#define BLYNK_AUTH_TOKEN    "UWgv50afdMITJ2vUBYQEmO0byQfT-4br"

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// =====================
// WiFi
// =====================

const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// =====================
// NTP (heure réelle)
// =====================
WiFiUDP udp;
NTPClient timeClient(udp, "pool.ntp.org", 3600, 60000); // UTC+1

// =====================
// PINS
// =====================

#define GREEN_LED_PIN  27
#define YELLOW_LED_PIN 25
#define RED_LED_PIN    26

#define PIR_PIN  14

#define TRIG_PIN 5
#define ECHO_PIN 18

#define BUZZER_PIN 12

#define DOOR_PIN 19

#define BTN1 32
#define BTN2 33
#define BTN3 4
#define BTN4 15

// =====================
// TIMING
// =====================

#define EXIT_DELAY  15000
#define ENTRY_DELAY 10000

// =====================
// LCD
// =====================

LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================
// ÉTATS FSM
// =====================

enum SystemState {
  DESARME,
  DELAI_SORTIE,
  ARME,
  DELAI_ENTREE,
  ALARME
};

SystemState currentState  = DESARME;
SystemState lastSentState = DESARME;

// =====================
// CODE PIN
// =====================

int enteredCode[4];
int enteredIndex = 0;

const int armCode[4]   = {1, 2, 3, 4};
const int nightCode[4] = {4, 3, 2, 1};

bool lastBTN1 = HIGH;
bool lastBTN2 = HIGH;
bool lastBTN3 = HIGH;
bool lastBTN4 = HIGH;

// =====================
// VARIABLES GLOBALES
// =====================

String alarmZone     = "";
bool nightMode       = false;
bool doubleDetection = false;

unsigned long stateStartTime = 0;

// Clignotement LED jaune
unsigned long lastBlinkTime = 0;
bool yellowLedState = false;

// Sirène non-bloquante
unsigned long lastSirenTime = 0;
int sirenStep = 0;
const int sirenFreqs[3] = {1000, 1500, 2000};

// Anti-rebond capteurs
bool pirHandled  = false;
bool doorHandled = false;

int alertCount = 0;

// =====================
// JOURNAL D'ÉVÉNEMENTS
// =====================

String eventLog[10];
int eventIndex = 0;

// Blynk timer
BlynkTimer timer;

// =====================================================
// Ajouter un événement au journal
// =====================================================

void addEvent(String evt) {
  timeClient.update();
  String horodatage = "";
  int h = timeClient.getHours();
  int m = timeClient.getMinutes();
  int s = timeClient.getSeconds();
  horodatage += (h < 10 ? "0" : "") + String(h) + ":";
  horodatage += (m < 10 ? "0" : "") + String(m) + ":";
  horodatage += (s < 10 ? "0" : "") + String(s);

  String fullEvt = "[" + horodatage + "] " + evt;

  eventLog[eventIndex] = fullEvt;
  eventIndex = (eventIndex + 1) % 10;

  Serial.println("LOG : " + fullEvt);
  Blynk.virtualWrite(V2, fullEvt + "\n");
}

// =====================================================
// Vérifier le code PIN saisi
// =====================================================

bool checkCode(const int code[]) {
  for (int i = 0; i < 4; i++) {
    if (enteredCode[i] != code[i]) return false;
  }
  return true;
}

// =====================================================
// Réinitialiser la saisie PIN
// =====================================================

void resetCode() {
  enteredIndex = 0;
}

// =====================================================
// Mesure de distance ultrasonique
// =====================================================

long measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 0;
  return duration * 0.034 / 2;
}

// =====================================================
// Mise à jour état Blynk V0
// =====================================================

void updateBlynkState() {
  if (currentState == lastSentState) return;
  lastSentState = currentState;

  int val = 0;
  switch (currentState) {
    case DESARME:      val = 0;   break;
    case DELAI_SORTIE: val = 100; break;
    case ARME:         val = 200; break;
    case DELAI_ENTREE: val = 150; break;
    case ALARME:       val = 255; break;
  }

  Blynk.virtualWrite(V0, val);
}

// =====================================================
// Vérification alerte nocturne (23h–6h)
// =====================================================

void checkNightAlert() {
  if (currentState != DESARME) return;

  timeClient.update();
  int heure = timeClient.getHours();

  if (heure >= 23 || heure < 6) {
    String msg = "Desarmement nocturne a " + String(heure) + "h";
    Blynk.logEvent("anomalie_horaire", msg);
    addEvent("ALERTE NOCTURNE");
  }
}

// =====================================================
// Désarmement unifié
// =====================================================

void desarmer() {
  currentState = DESARME;
  alarmZone    = "";
  alertCount   = 0;
  pirHandled   = false;
  doorHandled  = false;
  addEvent("DESARMEMENT");
  checkNightAlert();
  updateBlynkState();
}

// =====================================================
// Mise à jour LCD
// — Ne contient QUE l'affichage, aucune logique métier
// =====================================================

void updateLCD() {
  lcd.clear();

  // Affichage prioritaire si double détection active
  if (doubleDetection && currentState == ALARME) {
    lcd.setCursor(0, 0); lcd.print("DOUBLE ALERTE");
    lcd.setCursor(0, 1); lcd.print(alarmZone.substring(0, 16));
    return;
  }

  switch (currentState) {
    case DESARME:
      lcd.setCursor(0, 0); lcd.print("DESARME");
      lcd.setCursor(0, 1); lcd.print(nightMode ? "MODE NUIT" : "NORMAL");
      break;

    case DELAI_SORTIE:
      lcd.setCursor(0, 0); lcd.print("ARMEMENT");
      lcd.setCursor(0, 1);
      lcd.print((EXIT_DELAY - (millis() - stateStartTime)) / 1000);
      lcd.print(" sec");
      break;

    case ARME:
      lcd.setCursor(0, 0); lcd.print("SYSTEME ARME");
      lcd.setCursor(0, 1);
      lcd.print("Alertes:");
      lcd.print(alertCount);
      break;

    case DELAI_ENTREE:
      lcd.setCursor(0, 0); lcd.print("DESARME PIN");
      lcd.setCursor(0, 1);
      lcd.print((ENTRY_DELAY - (millis() - stateStartTime)) / 1000);
      lcd.print(" sec");
      break;

    case ALARME:
      lcd.setCursor(0, 0); lcd.print("!!! ALARME !!!");
      lcd.setCursor(0, 1); lcd.print(alarmZone.substring(0, 16));
      break;
  }
}

// =====================================================
// Bouton Blynk V1 : armement à distance
// =====================================================

BLYNK_WRITE(V1) {
  int val = param.asInt();

  if (val == 1 && currentState == DESARME) {
    currentState   = DELAI_SORTIE;
    stateStartTime = millis();
    addEvent("ARMEMENT BLYNK");
    updateBlynkState();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  pinMode(GREEN_LED_PIN,  OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN,    OUTPUT);
  pinMode(PIR_PIN,        INPUT);
  pinMode(TRIG_PIN,       OUTPUT);
  pinMode(ECHO_PIN,       INPUT);
  pinMode(BUZZER_PIN,     OUTPUT);
  pinMode(DOOR_PIN,       INPUT_PULLUP);
  pinMode(BTN1,           INPUT_PULLUP);
  pinMode(BTN2,           INPUT_PULLUP);
  pinMode(BTN3,           INPUT_PULLUP);
  pinMode(BTN4,           INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0); lcd.print("Connexion WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connecte");

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  timeClient.begin();
  timeClient.update();

  updateLCD();
  addEvent("SYSTEME DEMARRE");
  updateBlynkState();
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

void loop() {
  Blynk.run();
  timer.run();

  // ── Lecture boutons ──
  bool b1 = digitalRead(BTN1);
  bool b2 = digitalRead(BTN2);
  bool b3 = digitalRead(BTN3);
  bool b4 = digitalRead(BTN4);

  // ── Lecture capteurs ──
  bool pirDetected   = digitalRead(PIR_PIN);
  bool doorOpen      = (digitalRead(DOOR_PIN) == LOW);
  long distance      = measureDistance();
  bool entryDetected = (distance > 0 && distance < 100);

  // ── Saisie PIN ──
  if (lastBTN1 == HIGH && b1 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 1;
  if (lastBTN2 == HIGH && b2 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 2;
  if (lastBTN3 == HIGH && b3 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 3;
  if (lastBTN4 == HIGH && b4 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 4;

  // ── Vérification PIN ──
  if (enteredIndex >= 4) {
    if (checkCode(armCode)) {
      if (currentState == DESARME) {
        currentState   = DELAI_SORTIE;
        stateStartTime = millis();
        addEvent("ARMEMENT");
        updateBlynkState();
      } else {
        desarmer();
      }
    }
    else if (checkCode(nightCode)) {
      nightMode = !nightMode;
      addEvent(nightMode ? "MODE NUIT ON" : "MODE NUIT OFF");
    }
    else {
      addEvent("PIN INCORRECT");
    }
    resetCode();
  }

  // ── Calcul double détection ──
  doubleDetection = (pirDetected && entryDetected)
                 || (pirDetected && doorOpen)
                 || (entryDetected && doorOpen);

  // ── PRIORITÉ : Double détection → ALARME immédiate ──
  if (currentState == ARME && doubleDetection) {
    String zones = "";
    if (pirDetected)   zones += "SALON ";
    if (entryDetected) zones += "ENTREE ";
    if (doorOpen)      zones += "PORTE ";
    zones.trim();

    alarmZone    = zones;
    alertCount++;
    currentState = ALARME;
    pirHandled   = true;
    doorHandled  = true;
    addEvent("DOUBLE DETECTION : " + zones);
    Blynk.logEvent("intrusion", "Double detection zones : " + zones);
    updateBlynkState();
  }

  // ── DELAI_SORTIE → ARME ──
  if (currentState == DELAI_SORTIE) {
    if (millis() - stateStartTime >= EXIT_DELAY) {
      currentState = ARME;
      pirHandled   = false;
      doorHandled  = false;
      addEvent("SYSTEME ARME");
      updateBlynkState();
    }
  }

  // ── ARME : HC-SR04 seul → DELAI_ENTREE ──
  if (currentState == ARME && entryDetected && !doubleDetection) {
    currentState   = DELAI_ENTREE;
    stateStartTime = millis();
    alarmZone      = "ENTREE";
    addEvent("PRESENCE ENTREE");
    updateBlynkState();
  }

  // ── ARME : PIR seul → DELAI_ENTREE ──
  if (currentState == ARME && pirDetected && !nightMode && !pirHandled && !doubleDetection) {
    currentState   = DELAI_ENTREE;
    stateStartTime = millis();
    alarmZone      = "SALON";
    pirHandled     = true;
    addEvent("PRESENCE SALON");
    updateBlynkState();
  }
  if (!pirDetected) pirHandled = false;

  // ── DELAI_ENTREE : expiration → ALARME ──
  if (currentState == DELAI_ENTREE) {
    if (millis() - stateStartTime >= ENTRY_DELAY) {
      currentState = ALARME;
      alertCount++;
      addEvent("INTRUSION " + alarmZone);
      Blynk.logEvent("intrusion", "Intrusion zone : " + alarmZone);
      updateBlynkState();
    }
  }

  // ── ARME : porte seule → ALARME directe ──
  if (currentState == ARME && doorOpen && !doorHandled && !doubleDetection) {
    alarmZone    = "PORTE";
    alertCount++;
    currentState = ALARME;
    doorHandled  = true;
    addEvent("PORTE OUVERTE");
    Blynk.logEvent("intrusion", "Intrusion zone : PORTE");
    updateBlynkState();
  }
  if (!doorOpen) doorHandled = false;

  // ── LEDs ──
  digitalWrite(GREEN_LED_PIN, currentState == ARME);
  digitalWrite(RED_LED_PIN,   currentState == ALARME);

  if (currentState == DELAI_SORTIE || currentState == DELAI_ENTREE) {
    if (millis() - lastBlinkTime > 500) {
      lastBlinkTime  = millis();
      yellowLedState = !yellowLedState;
      digitalWrite(YELLOW_LED_PIN, yellowLedState);
    }
  } else {
    digitalWrite(YELLOW_LED_PIN, LOW);
    yellowLedState = false;
  }

  // ── Sirène non-bloquante ──
  if (currentState == ALARME) {
    if (millis() - lastSirenTime >= 120) {
      lastSirenTime = millis();
      tone(BUZZER_PIN, sirenFreqs[sirenStep]);
      sirenStep = (sirenStep + 1) % 3;
    }
  }
  else if (currentState == DELAI_SORTIE || currentState == DELAI_ENTREE) {
    tone(BUZZER_PIN, 1000);
  }
  else {
    noTone(BUZZER_PIN);
    sirenStep = 0;
  }

  // ── LCD ──
  updateLCD();

  // ── Mémorisation boutons ──
  lastBTN1 = b1;
  lastBTN2 = b2;
  lastBTN3 = b3;
  lastBTN4 = b4;
}