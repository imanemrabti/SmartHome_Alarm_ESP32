# SmartHome_Alarm_ESP32

Le système de sécurité anti-intrusion développé dans ce projet remplit l'ensemble des exigences du cahier des charges :
  •	Automate à 5 états entièrement implémenté et validé sur ESP32 / Wokwi.
  •	Deux capteurs de détection (PIR zone salon, HC-SR04 zone entrée) plus un contact de porte.
  •	Code PIN 4 digits sur clavier physique pour armement, désarmement et mode nuit.
  •	Signalisation multimodale : LED tricolore, LCD I2C, sirène graduée.
  •	Journal circulaire des 10 derniers événements avec sortie série.
  •	Intégration Blynk : état temps réel, notifications push, commande à distance, alerte nocturne.
  •	Extension mode nuit désactivant le PIR pour une utilisation nocturne sans fausse alarme.
