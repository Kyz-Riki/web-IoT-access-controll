#include <SPI.h>
#include <MFRC522.h>

// ---- Pin sesuai wiring kamu ----
#define SS_PIN     5   // SDA RFID
#define RST_PIN    4   // RST RFID
#define RELAY_PIN  22  // IN relay

MFRC522 mfrc522(SS_PIN, RST_PIN);

// Ganti ke true kalau relay kamu ternyata active LOW
// (kebanyakan modul relay murah 1-channel itu active LOW)
bool RELAY_ACTIVE_LOW = true;

void relayOn() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
}

void relayOff() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  SPI.begin();          // pakai default hardware SPI ESP32 (SCK=18, MISO=19, MOSI=23)
  mfrc522.PCD_Init();

  pinMode(RELAY_PIN, OUTPUT);
  relayOff();            // pastikan dropbolt default terkunci saat device baru nyala

  Serial.println("=================================");
  Serial.println("Test RFID -> Dropbolt siap.");
  Serial.println("Tap kartu untuk buka dropbolt...");
  Serial.println("=================================");
}

void loop() {
  // Cek ada kartu baru atau tidak
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // Print UID kartu ke Serial Monitor (buat debug, belum ada validasi UID)
  Serial.print("Kartu terdeteksi, UID:");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();

  // Trigger relay -> dropbolt unlock sesaat
  Serial.println(">> Unlock dropbolt (2 detik)...");
  relayOn();
  delay(2000);
  relayOff();
  Serial.println(">> Dropbolt terkunci lagi.");

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(1000); // jeda kecil biar gak double-trigger dari kartu yang sama
}
