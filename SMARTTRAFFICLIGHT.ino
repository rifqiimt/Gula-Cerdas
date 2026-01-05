#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "max6675.h"
#include <FirebaseESP32.h> // LIBRARY WAJIB: Install "Firebase ESP32 Client" by Mobizt

/* ================= KONFIGURASI FIREBASE & WIFI ================= */
#define FIREBASE_HOST "gulacerdas-default-rtdb.asia-southeast1.firebasedatabase.app" // Tanpa https://
#define FIREBASE_AUTH "ayaFAavrvxe2zQrRgBPKr9WoF6Ah6EMr1rxnz8z6"
const char* ssid     = "POCOEFSIK";
const char* password = "gambeteros";

/* ================= OBJEK FIREBASE ================= */
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
String pathData = "/monitoring";
String pathCmd  = "/control/command";

WebServer server(80);

/* ================= PIN ================= */
#define PIN_SCK   18
#define PIN_CS    5
#define PIN_SO    19
#define IN1       26
#define IN2       27
#define ENA       25
#define LED_MERAH 23

/* ================= SENSOR ================= */
Adafruit_INA219 ina219;
MAX6675 thermocouple(PIN_SCK, PIN_CS, PIN_SO);

/* ================= GLOBAL VARS ================= */
int pwmValue = 100;
bool motorSafe = true;
unsigned long lastFirebaseTime = 0; // Timer agar tidak spam firebase

/* ================= FUNGSI MOTOR ================= */
void motorStart() {
  motorSafe = true;
  digitalWrite(LED_MERAH, LOW);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  ledcWrite(0, pwmValue);
  Serial.println(">>> MOTOR START");
}

void motorStop() {
  motorSafe = false;
  digitalWrite(LED_MERAH, HIGH);
  ledcWrite(0, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  Serial.println(">>> MOTOR STOP");
}

void systemReset() {
  motorSafe = false;
  digitalWrite(LED_MERAH, LOW);
  ledcWrite(0, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  Serial.println(">>> SISTEM RESET");
}

/* ================= CORS (WEB SERVER LOKAL TETAP JALAN) ================= */
void sendCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

/* ================= API DATA (LOKAL) ================= */
void handleData() {
  StaticJsonDocument<256> doc;
  float suhuC    = thermocouple.readCelsius();
  float power_mW = ina219.getPower_mW();

  doc["temp"]  = suhuC;
  doc["power"] = power_mW;
  doc["motor"] = motorSafe ? "ON" : "OFF";
  String json;
  serializeJson(doc, json);
  sendCORS();
  server.send(200, "application/json", json);
}

/* ================= API COMMAND (LOKAL) ================= */
void handleCommand() {
  sendCORS();
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "CMD missing");
    return;
  }
  String cmd = server.arg("cmd");
  
  if (cmd == "START") motorStart();
  else if (cmd == "STOP") motorStop();
  else if (cmd == "RESET") systemReset();

  server.send(200, "text/plain", "OK");
}

/* ================= FIREBASE SYNC ================= */
void handleFirebase(float suhu, float power) {
  // 1. KIRIM DATA KE FIREBASE (Setiap 2 detik agar tidak limit)
  if (millis() - lastFirebaseTime > 2000) {
    lastFirebaseTime = millis();

    // Kirim Suhu
    if(Firebase.setFloat(firebaseData, pathData + "/temp", suhu)) {
      // Serial.println("Firebase: Suhu Terkirim");
    }
    // Kirim Daya
    Firebase.setFloat(firebaseData, pathData + "/power", power);
    // Kirim Status Motor
    Firebase.setString(firebaseData, pathData + "/motor", motorSafe ? "ON" : "OFF");
    
    // 2. BACA COMMAND DARI FIREBASE
    // Kita cek path: /control/command
    if (Firebase.getString(firebaseData, pathCmd)) {
      String cmdCloud = firebaseData.stringData();
      
      // Jika ada command baru (bukan IDLE)
      if (cmdCloud != "IDLE") {
        Serial.print("FIREBASE CMD: "); Serial.println(cmdCloud);

        if (cmdCloud == "START") motorStart();
        else if (cmdCloud == "STOP") motorStop();
        else if (cmdCloud == "RESET") systemReset();

        // Kembalikan status di Firebase ke IDLE agar tidak dieksekusi berulang
        Firebase.setString(firebaseData, pathCmd, "IDLE");
      }
    }
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  // INA219
  if (!ina219.begin()) {
    Serial.println("Gagal mendeteksi INA219!");
    while (1);
  }

  // MOTOR
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(LED_MERAH, OUTPUT);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(LED_MERAH, LOW);

  ledcSetup(0, 5000, 8);
  ledcAttachPin(ENA, 0);
  ledcWrite(0, 0);

  // WIFI
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nESP32 READY");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // INIT FIREBASE
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // INIT ROUTES WEB LOKAL
  server.on("/data", HTTP_GET, handleData);
  server.on("/command", HTTP_GET, handleCommand);
  server.on("/data", HTTP_OPTIONS, []() { sendCORS(); server.send(204); });
  server.on("/command", HTTP_OPTIONS, []() { sendCORS(); server.send(204); });
  server.begin();
}

/* ================= LOOP ================= */
void loop() {
  // Handle Web Server Lokal (Priority)
  server.handleClient();

  // Baca Sensor
  float suhuC    = thermocouple.readCelsius();
  float power_mW = ina219.getPower_mW();

  // Handle Firebase (Kirim & Terima Data Cloud)
  handleFirebase(suhuC, power_mW);

  // ⚠️ LOGIKA PENGAMAN (Safety Logic)
  
  // Alert Suhu (Print only)
  static unsigned long lastPrint = 0;
  if (suhuC >= 750 && millis() - lastPrint > 1000) {
    Serial.print("⚠️ ALERT: Suhu tinggi = "); Serial.println(suhuC);
    lastPrint = millis();
  }

  // Shutdown Daya Tinggi
  if (motorSafe && power_mW >= 2000) {
    motorStop();
    Serial.print("🚨 SHUTDOWN: Daya tinggi = "); Serial.println(power_mW);
    
    // Update status ke Firebase segera agar UI berubah
    Firebase.setString(firebaseData, pathData + "/motor", "OFF");
  }

  // CATATAN: delay(1000) dihapus diganti millis() di handleFirebase
  // agar Web Server tidak lemot.
}