/*
 * ============================================================
 * Projet 7 – Système de sécurité anti-intrusion multi-zones
 * Master GLCC – FS Ibn Tofail Kénitra
 * Auteur  : Malak Jlaika/ Imane M'RABTI/ Aya Fatene
 * Matériel : ESP32 DevKit C v4
 * Simulateur : Wokwi (Wokwi-GUEST Wi-Fi)
 * Supervision : Blynk
 * ============================================================
 */

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
// Wi-Fi
// =====================

const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// Délai entre deux tentatives de reconnexion Wi-Fi (ms)
#define WIFI_RECONNECT_INTERVAL 10000

// Horodatage de la dernière tentative de reconnexion
unsigned long lastWifiRetry = 0;

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

// Intervalle de rafraîchissement LCD (ms) — évite lcd.clear() à chaque loop()
#define LCD_REFRESH_INTERVAL 300

// =====================
// LCD
// =====================

LiquidCrystal_I2C lcd(0x27, 16, 2);
unsigned long lastLcdRefresh = 0;

// =====================
// ÉTATS FSM
// =====================

/**
 * Énumération des états de la machine à états du système.
 * DESARME      : système inactif, saisie PIN possible.
 * DELAI_SORTIE : armement en cours (15 s pour quitter).
 * ARME         : surveillance active.
 * DELAI_ENTREE : présence détectée, 10 s pour désarmer.
 * ALARME       : intrusion confirmée, sirène active.
 */
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

const int armCode[4]   = {1, 2, 3, 4}; // Code armement / désarmement
const int nightCode[4] = {4, 3, 2, 1}; // Code activation mode nuit

bool lastBTN1 = HIGH;
bool lastBTN2 = HIGH;
bool lastBTN3 = HIGH;
bool lastBTN4 = HIGH;

// =====================
// VARIABLES GLOBALES
// =====================

String alarmZone     = "";  // Zone ayant déclenché l'alarme
bool nightMode       = false;  // Mode nuit : seule la zone entrée est active
bool doubleDetection = false;  // Deux zones détectées simultanément

unsigned long stateStartTime = 0; // Horodatage du début de l'état courant

// Clignotement LED jaune (non-bloquant)
unsigned long lastBlinkTime = 0;
bool yellowLedState = false;

// Sirène non-bloquante (3 fréquences en rotation)
unsigned long lastSirenTime = 0;
int sirenStep = 0;
const int sirenFreqs[3] = {1000, 1500, 2000};

// Drapeaux anti-rebond capteurs
bool pirHandled  = false;
bool doorHandled = false;

int alertCount = 0; // Compteur d'alertes depuis le dernier armement

// =====================
// JOURNAL D'ÉVÉNEMENTS
// =====================

String eventLog[10];  // Circulaire, 10 entrées max
int eventIndex = 0;

// Blynk timer (non utilisé directement, réservé pour extensions)
BlynkTimer timer;

// =====================================================
// addEvent()
// Ajoute un événement horodaté au journal circulaire
// et l'envoie vers Blynk V2.
// @param evt  Texte de l'événement (sans horodatage)
// =====================================================
void addEvent(String evt) {
  timeClient.update();

  // Construction de l'horodatage HH:MM:SS
  int h = timeClient.getHours();
  int m = timeClient.getMinutes();
  int s = timeClient.getSeconds();
  String ts = "";
  ts += (h < 10 ? "0" : "") + String(h) + ":";
  ts += (m < 10 ? "0" : "") + String(m) + ":";
  ts += (s < 10 ? "0" : "") + String(s);

  String fullEvt = "[" + ts + "] " + evt;

  // Stockage circulaire
  eventLog[eventIndex] = fullEvt;
  eventIndex = (eventIndex + 1) % 10;

  Serial.println("LOG : " + fullEvt);

  // Envoi Blynk uniquement si connecté
  if (Blynk.connected()) {
    Blynk.virtualWrite(V2, fullEvt + "\n");
  }
}

// =====================================================
// checkCode()
// Compare le code saisi avec un code de référence.
// @param code  Tableau de 4 entiers (code de référence)
// @return true si le code saisi correspond, false sinon
// =====================================================
bool checkCode(const int code[]) {
  for (int i = 0; i < 4; i++) {
    if (enteredCode[i] != code[i]) return false;
  }
  return true;
}

// =====================================================
// resetCode()
// Remet à zéro l'index de saisie du code PIN.
// =====================================================
void resetCode() {
  enteredIndex = 0;
}

// =====================================================
// measureDistance()
// Mesure la distance via le capteur ultrasonique HC-SR04.
// Note : pulseIn() peut bloquer jusqu'à 30 ms (timeout).
//        Acceptable en simulation ; à remplacer par une
//        lecture non-bloquante sur matériel réel.
// @return Distance en centimètres, 0 si hors portée.
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
// updateBlynkState()
// Envoie l'état courant vers Blynk V0 uniquement si
// l'état a changé depuis le dernier envoi.
// Valeurs : DESARME=0, DELAI_SORTIE=100, ARME=200,
//           DELAI_ENTREE=150, ALARME=255
// =====================================================
void updateBlynkState() {
  if (currentState == lastSentState) return;
  lastSentState = currentState;

  if (!Blynk.connected()) return;

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
// checkNightAlert()
// Envoie une alerte Blynk si un désarmement a lieu
// entre 23 h et 6 h (plage horaire suspecte).
// N'agit que si le système est en état DESARME.
// =====================================================
void checkNightAlert() {
  if (currentState != DESARME) return;

  timeClient.update();
  int heure = timeClient.getHours();

  if (heure >= 23 || heure < 6) {
    String msg = "Desarmement nocturne a " + String(heure) + "h";
    if (Blynk.connected()) {
      Blynk.logEvent("anomalie_horaire", msg);
    }
    addEvent("ALERTE NOCTURNE");
  }
}

// =====================================================
// desarmer()
// Désarme le système : réinitialise l'état, les drapeaux
// et les compteurs, puis vérifie l'alerte horaire.
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
// updateLCD()
// Rafraîchit l'affichage LCD selon l'état courant.
// Contient uniquement de l'affichage — aucune logique
// métier. Appelée à intervalle régulier (LCD_REFRESH_INTERVAL).
// =====================================================
void updateLCD() {
  lcd.clear();

  // Affichage prioritaire : double détection en alarme
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
// handleWifiReconnect()
// Vérifie périodiquement la connexion Wi-Fi et tente
// une reconnexion si nécessaire (non-bloquant).
// Doit être appelée dans loop().
// =====================================================
void handleWifiReconnect() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiRetry < WIFI_RECONNECT_INTERVAL) return;

  lastWifiRetry = now;
  Serial.println("Wi-Fi perdu — tentative de reconnexion...");

  // Affichage LCD temporaire
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Wi-Fi perdu");
  lcd.setCursor(0, 1); lcd.print("Reconnexion...");

  WiFi.disconnect();
  WiFi.begin(ssid, password);

  // Attente courte non-bloquante (max 3 s dans la reconnexion)
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 3000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi reconnecte.");
    Blynk.connect();
    addEvent("WIFI RECONNECTE");
  } else {
    Serial.println("Echec reconnexion Wi-Fi.");
  }
}

// =====================================================
// handlePinEntry()
// Traite la saisie des boutons et vérifie le code PIN.
// Appelée avec les états courants et précédents des 4 boutons.
// @param b1..b4      État courant des boutons (LOW = appuyé)
// =====================================================
void handlePinEntry(bool b1, bool b2, bool b3, bool b4) {
  // Enregistrement des chiffres sur front descendant
  if (lastBTN1 == HIGH && b1 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 1;
  if (lastBTN2 == HIGH && b2 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 2;
  if (lastBTN3 == HIGH && b3 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 3;
  if (lastBTN4 == HIGH && b4 == LOW && enteredIndex < 4) enteredCode[enteredIndex++] = 4;

  // Vérification quand 4 chiffres sont saisis
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
}

// =====================================================
// updateLeds()
// Met à jour les trois LEDs selon l'état courant.
// Gère le clignotement non-bloquant de la LED jaune.
// =====================================================
void updateLeds() {
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
}

// =====================================================
// updateSiren()
// Gère la sirène de façon non-bloquante.
// ALARME     : rotation de 3 fréquences toutes les 120 ms.
// DELAI_*    : tonalité fixe 1000 Hz.
// Autres     : silence.
// =====================================================
void updateSiren() {
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
}

// =====================================================
// handleDetections()
// Évalue les capteurs et applique les transitions FSM
// liées aux détections (double détection, PIR, porte,
// HC-SR04, expiration des délais).
// @param pirDetected   true si le PIR a détecté un mouvement
// @param doorOpen      true si la porte est ouverte
// @param entryDetected true si la distance HC-SR04 < 100 cm
// =====================================================
void handleDetections(bool pirDetected, bool doorOpen, bool entryDetected) {

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
    if (Blynk.connected()) {
      Blynk.logEvent("intrusion", "Double detection zones : " + zones);
    }

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

  // ── ARME : PIR seul → DELAI_ENTREE (désactivé en mode nuit) ──
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
      if (Blynk.connected()) {
        Blynk.logEvent("intrusion", "Intrusion zone : " + alarmZone);
      }
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
    if (Blynk.connected()) {
      Blynk.logEvent("intrusion", "Intrusion zone : PORTE");
    }
    updateBlynkState();
  }
  if (!doorOpen) doorHandled = false;
}

// =====================================================
// Bouton Blynk V1 : armement à distance
// Arme le système depuis l'application Blynk.
// Ne fait rien si le système n'est pas désarmé.
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
// Initialise les périphériques, la connexion Wi-Fi,
// Blynk et NTP.
// =====================================================
void setup() {
  Serial.begin(115200);

  // Configuration des broches
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

  // Initialisation LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Connexion WiFi");

  // Connexion Wi-Fi
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connecte");
  } else {
    Serial.println("\nWiFi non disponible — mode hors ligne");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi indispo");
    lcd.setCursor(0, 1); lcd.print("Mode hors ligne");
    delay(2000);
  }

  // Connexion Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(3000); // timeout 3 s pour ne pas bloquer

  // NTP
  timeClient.begin();
  timeClient.update();

  // État initial
  updateLCD();
  addEvent("SYSTEME DEMARRE");
  updateBlynkState();
}

// =====================================================
// LOOP PRINCIPAL
// Exécute en continu : Blynk, reconnexion Wi-Fi,
// lecture capteurs/boutons, FSM, LEDs, sirène, LCD.
// Aucun delay() — tout est géré via millis().
// =====================================================
void loop() {
  Blynk.run();
  timer.run();

  // ── Gestion reconnexion Wi-Fi (non-bloquant) ──
  handleWifiReconnect();

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

  // ── Saisie et vérification PIN ──
  handlePinEntry(b1, b2, b3, b4);

  // ── Transitions FSM et détections ──
  handleDetections(pirDetected, doorOpen, entryDetected);

  // ── Indicateurs visuels ──
  updateLeds();
  updateSiren();

  // ── Rafraîchissement LCD à intervalle régulier ──
  if (millis() - lastLcdRefresh >= LCD_REFRESH_INTERVAL) {
    lastLcdRefresh = millis();
    updateLCD();
  }

  // ── Mémorisation états boutons (anti-rebond front) ──
  lastBTN1 = b1;
  lastBTN2 = b2;
  lastBTN3 = b3;
  lastBTN4 = b4;
}
