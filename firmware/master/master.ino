/*
  FOREST FIRE DETECTION - MASTER NODE
  Board: ESP32 DevKit V1 
  ESP32 Arduino Core: 3.x

  Receives node1/node2 by ESP-NOW and sends each node to the cloud dashboard
  (Node.js/Express app deployed on Render) by HTTP POST.
  Dashboard endpoint: /firedata

  IMPORTANT:
  1) Set Wi-Fi SSID/password below. The Master needs real internet access
     (a home router or a phone hotspot with internet, not just a local LAN)
     because it now posts to your Render URL over the public internet.
     Both Slaves still only need to be on the same Wi-Fi channel as the
     Master for ESP-NOW, so keep all three on the same network.
  2) Set NODE_RED_URL to your Render app URL ending in /firedata, e.g.
     "https://forest-fire-dashboard.onrender.com/firedata".
  3) Set API_KEY to the same value as the API_KEY environment variable
     configured on the Render service.
  4) The sketch uses setInsecure() so it can talk to any HTTPS host without
     embedding a CA certificate. This skips server certificate validation;
     it is fine for a hobby/demo project but not for a hardened deployment.
     For production, replace it with client.setCACert(ROOT_CA_CERT).
  5) Confirm MASTER_MAC in both Slaves matches this board's Wi-Fi STA MAC address.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>

// -------- Wi-Fi: fill in your own hotspot credentials --------
const char* WIFI_SSID = "Iphone 17 pro";
const char* WIFI_PASSWORD = "12345678";

// -------- Cloud dashboard settings (Render) --------
// Replace with your actual Render URL, must end in /firedata, and use https://
const char* NODE_RED_URL = "https://YOUR-APP-NAME.onrender.com/firedata";
// Must match the API_KEY environment variable set on the Render service
const char* API_KEY = "zmd9bfytghv3pu7cjear1x2s0in64wqol5k8";

// Demo thresholds only; calibrate sensors in the actual environment.
const float TEMP_THRESHOLD = 50.0;
const float HUMIDITY_THRESHOLD = 20.0;
const int SMOKE_THRESHOLD = 600;
const unsigned long WIFI_RETRY_MS = 10000;

// Must be byte-for-byte identical to the structure in both Slaves.
typedef struct __attribute__((packed)) {
  char nodeId[10];
  float temperature;
  float humidity;
  int32_t smoke_level;
  uint8_t fire_detected;
} SensorData;

struct NodeSlot {
  SensorData data;
  bool dirty;
  bool seen;
  unsigned long lastSeen;
};

NodeSlot node1Slot = {};
NodeSlot node2Slot = {};
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
bool espNowReady = false;
unsigned long lastWiFiRetry = 0;

bool isFire(const SensorData& d) {
  return d.temperature >= TEMP_THRESHOLD ||
         d.humidity <= HUMIDITY_THRESHOLD ||
         d.smoke_level >= SMOKE_THRESHOLD;
}

void onDataReceive(const esp_now_recv_info_t* info,
                   const uint8_t* incomingData, int len) {
  (void)info;
  if (len != (int)sizeof(SensorData)) return;

  SensorData d;
  memcpy(&d, incomingData, sizeof(d));
  d.nodeId[sizeof(d.nodeId) - 1] = '\0';

  if (strcmp(d.nodeId, "node1") != 0 && strcmp(d.nodeId, "node2") != 0) return;
  if (!isfinite(d.temperature) || !isfinite(d.humidity)) return;
  d.fire_detected = isFire(d) ? 1 : 0;

  portENTER_CRITICAL(&dataMux);
  NodeSlot* slot = (strcmp(d.nodeId, "node1") == 0) ? &node1Slot : &node2Slot;
  memcpy(&slot->data, &d, sizeof(d));
  slot->dirty = true;
  slot->seen = true;
  slot->lastSeen = millis();
  portEXIT_CRITICAL(&dataMux);
}

bool initESPNow() {
  if (espNowReady) return true;
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("ESP-NOW init failed: %s\n", esp_err_to_name(err));
    return false;
  }
  err = esp_now_register_recv_cb(onDataReceive);
  if (err != ESP_OK) {
    Serial.printf("Register receive callback failed: %s\n", esp_err_to_name(err));
    esp_now_deinit();
    return false;
  }
  espNowReady = true;
  Serial.printf("ESP-NOW ready; Wi-Fi channel %d\n", WiFi.channel());
  return true;
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!espNowReady) initESPNow();
    return;
  }

  if (espNowReady) {
    esp_now_deinit();
    espNowReady = false;
  }

  if (millis() - lastWiFiRetry >= WIFI_RETRY_MS) {
    lastWiFiRetry = millis();
    Serial.printf("Wi-Fi disconnected; reconnecting to %s\n", WIFI_SSID);
    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

bool postNodeData(const SensorData& d) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (String(NODE_RED_URL).indexOf("YOUR-APP-NAME") >= 0) {
    Serial.println("Set NODE_RED_URL to your real Render URL before sending data.");
    return false;
  }
  if (String(API_KEY) == "CHANGE_ME_LONG_RANDOM_KEY") {
    Serial.println("Set API_KEY and matching NODE_RED_API_KEY in .env.");
    return false;
  }

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"nodeId\":\"%s\",\"temperature\":%.2f,\"humidity\":%.2f,\"smoke_level\":%ld,\"fire_detected\":%s}",
           d.nodeId, d.temperature, d.humidity, (long)d.smoke_level,
           d.fire_detected ? "true" : "false");

  HTTPClient http;
  int httpCode = -1;
  const String url = NODE_RED_URL;

  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    // TESTING ONLY: skips server certificate validation. Replace with client.setCACert(...)
    // before deploying on a public network.
    client.setInsecure();
    if (!http.begin(client, url)) {
      Serial.println("HTTPS begin failed");
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", API_KEY);
    http.setTimeout(5000);
    httpCode = http.POST((uint8_t*)payload, strlen(payload));
    if (httpCode > 0) Serial.printf("Dashboard HTTP status: %d\n", httpCode);
    else Serial.printf("Dashboard HTTPS error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) {
      Serial.println("HTTP begin failed");
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", API_KEY);
    http.setTimeout(2000);
    httpCode = http.POST((uint8_t*)payload, strlen(payload));
    if (httpCode > 0) Serial.printf("Dashboard HTTP status: %d\n", httpCode);
    else Serial.printf("Dashboard HTTP error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
  }

  Serial.printf("POST %s | T=%.1f C H=%.1f%% Smoke=%ld Fire=%s\n",
                d.nodeId, d.temperature, d.humidity, (long)d.smoke_level,
                d.fire_detected ? "YES" : "NO");
  return httpCode >= 200 && httpCode < 300;
}

bool takeDirtyData(NodeSlot& slot, SensorData& out) {
  bool available = false;
  portENTER_CRITICAL(&dataMux);
  if (slot.dirty) {
    memcpy(&out, &slot.data, sizeof(out));
    slot.dirty = false;
    available = true;
  }
  portEXIT_CRITICAL(&dataMux);
  return available;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== FOREST FIRE MASTER: ESP-NOW -> HTTP POST ===");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiRetry = millis();

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Master IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Master STA MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("Wi-Fi channel: %d\n", WiFi.channel());
    initESPNow();
  } else {
    Serial.println("Wi-Fi not connected yet; will retry in loop().");
  }
  Serial.printf("Reset reason: %d\n", esp_reset_reason());
}

void loop() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED || !espNowReady) {
    delay(20);
    return;
  }

  SensorData d;
  if (takeDirtyData(node1Slot, d)) postNodeData(d);
  if (takeDirtyData(node2Slot, d)) postNodeData(d);
  delay(5);
}
