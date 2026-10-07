#include "api_client.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================
//  Private Helpers
// ============================================

/// Buat full URL dari endpoint
static String _buildUrl(const char* endpoint) {
  return String(SERVER_BASE_URL) + String(endpoint);
}

/// Log HTTP error ke Serial
static void _logHttpError(const char* action, int httpCode) {
  Serial.print("[API] ❌ ");
  Serial.print(action);
  Serial.print(" gagal, HTTP ");
  Serial.println(httpCode);
}

// ============================================
//  Public Functions
// ============================================

bool apiGetWhitelist(String &jsonOut) {
  HTTPClient http;

  String url = _buildUrl(API_WHITELIST);
  url += "?esp_id=" + String(ESP_ID);

  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);

  Serial.print("[API] 📥 GET whitelist... ");
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    jsonOut = http.getString();
    Serial.println("✅ OK");
    http.end();
    return true;
  }

  _logHttpError("GET whitelist", httpCode);
  http.end();
  return false;
}

TapResponse apiSendTap(const String &uid) {
  TapResponse response;
  response.success = false;
  response.action = "deny";
  response.reason = "server_error";
  response.cardHolder = "";

  HTTPClient http;
  http.begin(_buildUrl(API_TAP));
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  // Buat JSON body
  DynamicJsonDocument doc(256);
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid;
  doc["door_name"] = DOOR_NAME;

  String body;
  serializeJson(doc, body);

  Serial.print("[API] 📤 POST tap (");
  Serial.print(uid);
  Serial.print(")... ");

  int httpCode = http.POST(body);

  if (httpCode == HTTP_CODE_OK) {
    String responseStr = http.getString();

    DynamicJsonDocument resDoc(512);
    DeserializationError error = deserializeJson(resDoc, responseStr);

    if (!error) {
      response.success = true;
      response.action = resDoc["action"].as<String>();
      response.reason = resDoc["reason"] | "";
      response.cardHolder = resDoc["card_holder"] | "";
      Serial.print("✅ ");
      Serial.println(response.action);
    } else {
      Serial.println("⚠️ Parse response gagal");
    }
  } else {
    _logHttpError("POST tap", httpCode);
  }

  http.end();
  return response;
}

bool apiSendTapLog(const String &uid, const String &status,
                   const String &reason, const String &accessLevel) {
  HTTPClient http;
  http.begin(_buildUrl(API_TAP));
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid;
  doc["status"] = status;
  doc["reason"] = reason;
  doc["access_level"] = accessLevel;
  doc["door_name"] = DOOR_NAME;
  doc["validated_locally"] = true;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  http.end();

  if (httpCode == HTTP_CODE_OK) {
    return true;
  }

  _logHttpError("POST tap log", httpCode);
  return false;
}

bool apiSendHeartbeat(const String &doorState) {
  HTTPClient http;
  http.begin(_buildUrl(API_HEARTBEAT));
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["esp_id"] = ESP_ID;
  doc["door_name"] = DOOR_NAME;
  doc["door_state"] = doorState;
  doc["uptime"] = millis() / 1000;  // uptime dalam detik
  doc["wifi_rssi"] = WiFi.RSSI();   // kekuatan sinyal WiFi

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  http.end();

  return httpCode == HTTP_CODE_OK;
}

bool apiSendDoorStatus(const String &state) {
  HTTPClient http;
  http.begin(_buildUrl(API_DOOR_STATUS));
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(128);
  doc["esp_id"] = ESP_ID;
  doc["state"] = state;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  http.end();

  return httpCode == HTTP_CODE_OK;
}

String apiCheckMode() {
  HTTPClient http;
  http.begin(_buildUrl(API_MODE));
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    http.end();

    DynamicJsonDocument doc(128);
    DeserializationError error = deserializeJson(doc, response);
    if (!error) {
      return doc["mode"].as<String>();
    }
  }

  http.end();
  return "normal";  // Default ke normal jika gagal
}

bool apiSendRegisterScan(const String &uid) {
  HTTPClient http;
  http.begin(_buildUrl(API_REGISTER_SCAN));
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(128);
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid;

  String body;
  serializeJson(doc, body);

  Serial.print("[API] 📤 POST register-scan (");
  Serial.print(uid);
  Serial.print(")... ");

  int httpCode = http.POST(body);
  http.end();

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("✅ OK");
    return true;
  }

  _logHttpError("POST register-scan", httpCode);
  return false;
}
