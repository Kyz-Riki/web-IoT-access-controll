#include "api_client.h"
#include "config.h"
#include "wifi_manager.h"
#include "access_control.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// ============================================
//  Private Helpers (Stream & Buffer Static)
// ============================================

static void _logHttpError(const char* action, int httpCode) {
  Serial.printf("[API] ❌ %s gagal, HTTP %d\n", action, httpCode);
}

/// Helper koneksi TCP dengan fast timeout
static bool _connectClient(WiFiClient &client) {
  if (!wifiIsConnected()) {
    return false;
  }
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);
#if defined(ESP32)
  if (!client.connect(SERVER_HOST, SERVER_PORT, HTTP_CONNECT_TIMEOUT_MS)) {
    return false;
  }
#else
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    return false;
  }
#endif
  return true;
}

/// Kirim request line & header, tunggu status code, lewati header sampai body
static int _sendAndReadHeaders(WiFiClient &client, const char* method, const char* endpoint,
                               const char* jsonBody, size_t bodyLen) {
  // Kirim HTTP Header
  client.print(method);
  client.print(" ");
  client.print(endpoint);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(SERVER_HOST);
  client.println("Connection: close");

  if (bodyLen > 0) {
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(bodyLen);
    client.println();
    client.write((const uint8_t*)jsonBody, bodyLen);
  } else {
    client.println();
  }

  // Tunggu respon pertama datang
  unsigned long startWait = millis();
  while (!client.available() && client.connected()) {
    if (millis() - startWait > HTTP_TIMEOUT_MS) {
      client.stop();
      return -1;
    }
    delay(5);
  }

  // Baca Status Line: "HTTP/1.1 200 OK"
  char statusLine[36] = {0};
  size_t n = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
  statusLine[n] = '\0';

  int statusCode = 0;
  char *sp = strchr(statusLine, ' ');
  if (sp) {
    statusCode = atoi(sp + 1);
  }

  // Cari akhir HTTP header ("\r\n\r\n")
  if (!client.find("\r\n\r\n")) {
    client.stop();
    return -1;
  }

  return statusCode;
}

// ============================================
//  Public Functions
// ============================================

bool apiSyncWhitelist() {
  Serial.print("[API] 📥 Syncing whitelist via stream... ");

  WiFiClient client;
  if (!_connectClient(client)) {
    Serial.println("❌ Connect failed");
    return false;
  }

  char endpoint[64];
  snprintf(endpoint, sizeof(endpoint), "%s?esp_id=%s", API_WHITELIST, ESP_ID);

  int httpCode = _sendAndReadHeaders(client, "GET", endpoint, NULL, 0);
  if (httpCode != 200) {
    client.stop();
    _logHttpError("GET whitelist", httpCode);
    return false;
  }

  // Deserialize langsung dari socket stream
  int count = accessControlLoadFromStream(client);
  client.stop();

  if (count >= 0) {
    Serial.println("✅ OK");
    return true;
  }
  return false;
}

TapResponse apiSendTap(const char* uid) {
  TapResponse response;
  response.success = false;
  strncpy(response.action, "deny", sizeof(response.action));
  strncpy(response.reason, "server_error", sizeof(response.reason));
  response.cardHolder[0] = '\0';

  char body[256];
  StaticJsonDocument<256> doc;
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid ? uid : "";
  doc["door_name"] = DOOR_NAME;
  size_t bodyLen = serializeJson(doc, body, sizeof(body));

  Serial.printf("[API] 📤 POST tap (%s)... ", uid);

  WiFiClient client;
  if (!_connectClient(client)) {
    Serial.println("❌ Connect failed");
    return response;
  }

  int httpCode = _sendAndReadHeaders(client, "POST", API_TAP, body, bodyLen);
  if (httpCode == 200) {
    StaticJsonDocument<512> resDoc;
    DeserializationError error = deserializeJson(resDoc, client);
    if (!error) {
      response.success = true;
      const char* act = resDoc["action"] | "deny";
      const char* rsn = resDoc["reason"] | "";
      const char* hld = resDoc["card_holder"] | "";

      strncpy(response.action, act, sizeof(response.action) - 1);
      strncpy(response.reason, rsn, sizeof(response.reason) - 1);
      strncpy(response.cardHolder, hld, sizeof(response.cardHolder) - 1);

      Serial.printf("✅ %s\n", response.action);
    } else {
      Serial.println("⚠️ Parse response gagal");
    }
  } else {
    _logHttpError("POST tap", httpCode);
  }

  client.stop();
  return response;
}

TapResponse apiSendTap(const String &uid) {
  return apiSendTap(uid.c_str());
}

bool apiSendTapLog(const char* uid, const char* status,
                   const char* reason, const char* accessLevel) {
  char body[256];
  StaticJsonDocument<256> doc;
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid ? uid : "";
  doc["status"] = status ? status : "denied";
  doc["reason"] = reason ? reason : "";
  doc["access_level"] = accessLevel ? accessLevel : "level_1";
  doc["door_name"] = DOOR_NAME;
  doc["validated_locally"] = true;
  size_t bodyLen = serializeJson(doc, body, sizeof(body));

  WiFiClient client;
  if (!_connectClient(client)) {
    return false;
  }

  int httpCode = _sendAndReadHeaders(client, "POST", API_TAP, body, bodyLen);
  client.stop();

  if (httpCode == 200) {
    return true;
  }
  _logHttpError("POST tap log", httpCode);
  return false;
}

bool apiSendTapLog(const String &uid, const String &status,
                   const String &reason, const String &accessLevel) {
  return apiSendTapLog(uid.c_str(), status.c_str(), reason.c_str(), accessLevel.c_str());
}

bool apiSendHeartbeat(const char* doorState) {
  char body[320];
  StaticJsonDocument<320> doc;
  doc["esp_id"] = ESP_ID;
  doc["door_name"] = DOOR_NAME;
  doc["door_state"] = doorState ? doorState : "locked";
  doc["uptime"] = millis() / 1000;
  doc["connection"] = "wifi";
  doc["wifi_rssi"] = wifiGetRSSI();
  // Metrik kesehatan RAM untuk monitoring 24/7
  doc["free_heap"] = ESP.getFreeHeap();
  doc["max_alloc_heap"] = ESP.getMaxAllocHeap();

  size_t bodyLen = serializeJson(doc, body, sizeof(body));

  WiFiClient client;
  if (!_connectClient(client)) {
    return false;
  }

  int httpCode = _sendAndReadHeaders(client, "POST", API_HEARTBEAT, body, bodyLen);
  client.stop();

  return httpCode == 200;
}

bool apiSendHeartbeat(const String &doorState) {
  return apiSendHeartbeat(doorState.c_str());
}

bool apiSendDoorStatus(const char* state) {
  char body[128];
  StaticJsonDocument<128> doc;
  doc["esp_id"] = ESP_ID;
  doc["state"] = state ? state : "locked";
  size_t bodyLen = serializeJson(doc, body, sizeof(body));

  WiFiClient client;
  if (!_connectClient(client)) {
    return false;
  }

  int httpCode = _sendAndReadHeaders(client, "POST", API_DOOR_STATUS, body, bodyLen);
  client.stop();

  return httpCode == 200;
}

bool apiSendDoorStatus(const String &state) {
  return apiSendDoorStatus(state.c_str());
}

bool apiCheckMode(char* modeOut, size_t maxLen) {
  if (!modeOut || maxLen == 0) return false;
  strncpy(modeOut, "normal", maxLen - 1);

  WiFiClient client;
  if (!_connectClient(client)) {
    return false;
  }

  int httpCode = _sendAndReadHeaders(client, "GET", API_MODE, NULL, 0);
  if (httpCode == 200) {
    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, client);
    if (!error) {
      const char* m = doc["mode"] | "normal";
      strncpy(modeOut, m, maxLen - 1);
      client.stop();
      return true;
    }
  }

  client.stop();
  return false;
}

bool apiCheckPendingCommand(char* cmdOut, size_t maxLen) {
  if (!cmdOut || maxLen == 0) return false;
  strncpy(cmdOut, "none", maxLen - 1);

  WiFiClient client;
  if (!_connectClient(client)) {
    return false;
  }

  char endpoint[64];
  snprintf(endpoint, sizeof(endpoint), "%s?esp_id=%s", API_PENDING_COMMAND, ESP_ID);

  int httpCode = _sendAndReadHeaders(client, "GET", endpoint, NULL, 0);
  if (httpCode == 200) {
    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, client);
    if (!error) {
      const char* cmd = doc["command"] | "none";
      strncpy(cmdOut, cmd, maxLen - 1);
      client.stop();
      return true;
    }
  }

  client.stop();
  return false;
}

bool apiSendRegisterScan(const char* uid) {
  char body[128];
  StaticJsonDocument<128> doc;
  doc["esp_id"] = ESP_ID;
  doc["uid"] = uid ? uid : "";
  size_t bodyLen = serializeJson(doc, body, sizeof(body));

  Serial.printf("[API] 📤 POST register-scan (%s)... ", uid);

  WiFiClient client;
  if (!_connectClient(client)) {
    Serial.println("❌ Connect failed");
    return false;
  }

  int httpCode = _sendAndReadHeaders(client, "POST", API_REGISTER_SCAN, body, bodyLen);
  client.stop();

  if (httpCode == 200) {
    Serial.println("✅ OK");
    return true;
  }

  _logHttpError("POST register-scan", httpCode);
  return false;
}

bool apiSendRegisterScan(const String &uid) {
  return apiSendRegisterScan(uid.c_str());
}
