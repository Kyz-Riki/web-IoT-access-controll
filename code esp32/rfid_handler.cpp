#include "rfid_handler.h"
#include "config.h"
#include <SPI.h>
#include <MFRC522.h>

static MFRC522 mfrc522(SS_PIN, RST_PIN);

void rfidInit() {
  SPI.begin();  // Default HW SPI ESP32: SCK=18, MISO=19, MOSI=23
  mfrc522.PCD_Init();
  delay(100);

  // Cek apakah reader terdeteksi
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("[RFID] ❌ Reader TIDAK terdeteksi! Cek wiring.");
  } else {
    Serial.print("[RFID] ✅ Reader terdeteksi, firmware v");
    Serial.print(version, HEX);
    Serial.println(version == 0x92 ? " (MFRC522 v2.0)" :
                   version == 0x91 ? " (MFRC522 v1.0)" : " (clone)");
  }
}

bool rfidCheckCard(String &uidOut) {
  // Cek apakah ada kartu baru
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return false;
  }

  // Coba baca serial kartu
  if (!mfrc522.PICC_ReadCardSerial()) {
    return false;
  }

  // Konversi UID byte array ke hex string
  uidOut = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (i > 0) uidOut += " ";
    if (mfrc522.uid.uidByte[i] < 0x10) uidOut += "0";
    uidOut += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidOut.toUpperCase();

  return true;
}

void rfidHaltCard() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}
