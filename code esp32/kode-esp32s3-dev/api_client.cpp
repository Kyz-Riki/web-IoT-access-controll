#include "api_client.h"
#include "config.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// ============================================
//  Private Helpers
// ============================================

/// Log HTTP error ke Serial
static void _logHttpError(const char* action, int httpCode) {
  Serial.printf("[API] ❌ %s gagal, HTTP %d\n", action, httpCode);
}

/// Eksekusi HTTP request via WiFiClient
static int _executeHttpRequest(const char* method, const char* endpoint,
                                const String &body, String &responseOut) {
  responseOut = "";

  WiFiClient client;
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println("[API] ❌ Gagal connect ke server");
    return -1;
  }

  // Kirim HTTP request
  client.print(method);
  client.print(" ");
  client.print(endpoint);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(SERVER_HOST);

  if (body.length() > 0) {
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(body.length());
  }

  client.println("Connection: close");
  client.println();

  if (body.length() > 0) {
    client.print(body);
  }

  // Tunggu respon (timeout)
  unsigned long startWait = millis();
  while (!client.available()) {
    if (millis() - startWait > HTTP_TIMEOUT_MS) {
      client.stop();
      Serial.println("[API] ❌ Timeout menunggu response");
      return -1;
    }
    delay(10);
  }

  // Baca respon
  String rawResponse = "";
  while (client.available()) {
    rawResponse += (char)client.read();
  }
  client.stop();

  // Parse HTTP status code
  int statusCode = 0;
  int spaceIdx = rawResponse.indexOf(' ');
  if (spaceIdx > 0) {
    statusCode = rawResponse.substring(spaceIdx + 1, spaceIdx + 4).toInt();
  }

  // Pisahkan header dan body
  int bodyStart = rawResponse.indexOf("\r\n\r\n");
  if (bodyStart > 0) {
    responseOut = rawResponse.substring(bodyStart + 4);
  }

  return statusCode;
}

static int _httpGet(const char* endpoint, String &responseOut) {
  return _executeHttpRequest("GET", endpoint, "", responseOut);
}

static int _httpPost(const char* endpoint, const String &jsonBody, String &responseOut) {
  return _executeHttpRequest("POST", endpoint, jsonBody, responseOut);
}

// ============================================
//  Public Functions
// ============================================

bool apiGetWhitelist(String &jsonOut) {
  String path = String(API_WHITELIST) + "?esp_id=" + String(ESP_ID);

  Serial.print("[API] 📥 GET whitelist... ");

  int httpCode = _httpGet(path.c_str(), jsonOut);

  if (httpCode == 200) {
    Serial.println("✅ OK");
    return true;
  }

  _logHttpError("GET whitelist", httpCode);
  return false;
}

TapResponse apiSendTap(const String &uid) {
  TapResponse response;
  response.success = false;
  response.action = "deny";
  response.reason = "server_error";
  response.cardHolder = "";

  // Buat JSON body
  DynamicJsonDocument doc(256);
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid;
  doc["door_name"] = DOOR_NAME;

  String body;
  serializeJson(doc, body);

  Serial.printf("[API] 📤 POST tap (%s)... ", uid.c_str());

  String responseBody;
  int httpCode = _httpPost(API_TAP, body, responseBody);

  if (httpCode == 200) {
    DynamicJsonDocument resDoc(512);
    DeserializationError error = deserializeJson(resDoc, responseBody);

    if (!error) {
      response.success = true;
      response.action = resDoc["action"].as<String>();
      response.reason = resDoc["reason"] | "";
      response.cardHolder = resDoc["card_holder"] | "";
      Serial.printf("✅ %s\n", response.action.c_str());
    } else {
      Serial.println("⚠️ Parse response gagal");
    }
  } else {
    _logHttpError("POST tap", httpCode);
  }

  return response;
}

bool apiSendTapLog(const String &uid, const String &status,
                   const String &reason, const String &accessLevel) {
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

  String responseBody;
  int httpCode = _httpPost(API_TAP, body, responseBody);

  if (httpCode == 200) {
    return true;
  }

  _logHttpError("POST tap log", httpCode);
  return false;
}

bool apiSendHeartbeat(const String &doorState) {
  DynamicJsonDocument doc(256);
  doc["esp_id"] = ESP_ID;
  doc["door_name"] = DOOR_NAME;
  doc["door_state"] = doorState;
  doc["uptime"] = millis() / 1000;
  doc["connection"] = "wifi";
  doc["wifi_rssi"] = wifiGetRSSI();

  String body;
  serializeJson(doc, body);

  String responseBody;
  int httpCode = _httpPost(API_HEARTBEAT, body, responseBody);

  return httpCode == 200;
}

bool apiSendDoorStatus(const String &state) {
  DynamicJsonDocument doc(128);
  doc["esp_id"] = ESP_ID;
  doc["state"] = state;

  String body;
  serializeJson(doc, body);

  String responseBody;
  int httpCode = _httpPost(API_DOOR_STATUS, body, responseBody);

  return httpCode == 200;
}

String apiCheckMode() {
  String responseBody;
  int httpCode = _httpGet(API_MODE, responseBody);

  if (httpCode == 200) {
    DynamicJsonDocument doc(128);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (!error) {
      return doc["mode"].as<String>();
    }
  }

  return "normal";  // Default ke normal jika gagal
}

bool apiSendRegisterScan(const String &uid) {
  DynamicJsonDocument doc(128);
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid;

  String body;
  serializeJson(doc, body);

  Serial.printf("[API] 📤 POST register-scan (%s)... ", uid.c_str());

  String responseBody;
  int httpCode = _httpPost(API_REGISTER_SCAN, body, responseBody);

  if (httpCode == 200) {
    Serial.println("✅ OK");
    return true;
  }

  _logHttpError("POST register-scan", httpCode);
  return false;
}
