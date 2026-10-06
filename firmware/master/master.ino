/*
  FOREST FIRE DETECTION - MASTER NODE
  Board: ESP32 Dev Module
  ESP32 Arduino Core: 3.x

  Functions:
  - Receive node1 and node2 data through ESP-NOW
  - Calculate fire alert conditions
  - Send sensor data to Render Dashboard using HTTPS
  - Retry sending failed data every 10 seconds

  Dashboard endpoint: /firedata
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>

// ==================================================
// 1. Wi-Fi settings
// ==================================================

const char* WIFI_SSID = "Iphone 17 pro";
const char* WIFI_PASSWORD = "12345678";

// ==================================================
// 2. Render Dashboard settings
// ==================================================

const char* NODE_RED_URL =
  "https://forestfiredetection-l721.onrender.com/firedata";

// Must match the API_KEY environment variable on Render
const char* API_KEY = "zmd9bfytghv3pu7cjear1x2s0in64wqol5k8";

// ==================================================
// 3. Fire detection thresholds
// ==================================================

const float TEMP_THRESHOLD = 50.0;
const float HUMIDITY_THRESHOLD = 20.0;
const int SMOKE_THRESHOLD = 2000;

const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long POST_RETRY_MS = 10000;
const unsigned long HTTPS_TIMEOUT_MS = 30000;

// ==================================================
// 4. ESP-NOW data structure
// Must match the structure used by both Slaves
// ==================================================

typedef struct __attribute__((packed)) {
  char nodeId[10];
  float temperature;
  float humidity;
  int32_t smoke_level;
  uint8_t fire_detected;
} SensorData;

// ==================================================
// 5. Node data storage
// ==================================================

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
unsigned long lastNode1PostAttempt = 0;
unsigned long lastNode2PostAttempt = 0;

// ==================================================
// 6. Fire detection
// ==================================================

bool isFire(const SensorData& d) {
  return
    d.temperature >= TEMP_THRESHOLD ||
    d.humidity <= HUMIDITY_THRESHOLD ||
    d.smoke_level >= SMOKE_THRESHOLD;
}

// ==================================================
// 7. ESP-NOW receive callback
// ESP32 Arduino Core 3.x
// ==================================================

void onDataReceive(
  const esp_now_recv_info_t* info,
  const uint8_t* incomingData,
  int len
) {
  (void)info;

  if (len != (int)sizeof(SensorData)) {
    return;
  }

  SensorData d;
  memcpy(&d, incomingData, sizeof(d));

  // Ensure nodeId is null-terminated
  d.nodeId[sizeof(d.nodeId) - 1] = '\0';

  // Accept only node1 and node2
  if (
    strcmp(d.nodeId, "node1") != 0 &&
    strcmp(d.nodeId, "node2") != 0
  ) {
    return;
  }

  // Reject invalid sensor readings
  if (
    !isfinite(d.temperature) ||
    !isfinite(d.humidity)
  ) {
    return;
  }

  // Calculate fire status at Master
  d.fire_detected = isFire(d) ? 1 : 0;

  // Store latest data safely
  portENTER_CRITICAL(&dataMux);

  NodeSlot* slot =
    (strcmp(d.nodeId, "node1") == 0)
      ? &node1Slot
      : &node2Slot;

  memcpy(&slot->data, &d, sizeof(d));

  slot->dirty = true;
  slot->seen = true;
  slot->lastSeen = millis();

  portEXIT_CRITICAL(&dataMux);
}

// ==================================================
// 8. Initialize ESP-NOW
// ==================================================

bool initESPNow() {
  if (espNowReady) {
    return true;
  }

  esp_err_t err = esp_now_init();

  if (err != ESP_OK) {
    Serial.printf(
      "ESP-NOW init failed: %s\n",
      esp_err_to_name(err)
    );
    return false;
  }

  err = esp_now_register_recv_cb(onDataReceive);

  if (err != ESP_OK) {
    Serial.printf(
      "Register receive callback failed: %s\n",
      esp_err_to_name(err)
    );

    esp_now_deinit();
    return false;
  }

  espNowReady = true;

  Serial.printf(
    "ESP-NOW ready; Wi-Fi channel %d\n",
    WiFi.channel()
  );

  return true;
}

// ==================================================
// 9. Maintain Wi-Fi connection
// ==================================================

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!espNowReady) {
      initESPNow();
    }

    return;
  }

  // Reinitialize ESP-NOW after Wi-Fi disconnects
  if (espNowReady) {
    esp_now_deinit();
    espNowReady = false;
  }

  if (millis() - lastWiFiRetry >= WIFI_RETRY_MS) {
    lastWiFiRetry = millis();

    Serial.println(
      "Wi-Fi disconnected; reconnecting..."
    );

    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ==================================================
// 10. Send sensor data to Render
// ==================================================

bool postNodeData(const SensorData& d) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected; POST skipped");
    return false;
  }

  if (
    String(NODE_RED_URL).indexOf("YOUR-APP-NAME") >= 0 ||
    String(API_KEY) == "YOUR_RENDER_API_KEY"
  ) {
    Serial.println(
      "ERROR: Configure NODE_RED_URL and API_KEY first"
    );
    return false;
  }

  // Construct JSON payload
  char payload[256];

  int payloadLength = snprintf(
    payload,
    sizeof(payload),
    "{\"nodeId\":\"%s\","
    "\"temperature\":%.2f,"
    "\"humidity\":%.2f,"
    "\"smoke_level\":%ld,"
    "\"fire_detected\":%s}",
    d.nodeId,
    d.temperature,
    d.humidity,
    (long)d.smoke_level,
    d.fire_detected ? "true" : "false"
  );

  if (
    payloadLength < 0 ||
    payloadLength >= (int)sizeof(payload)
  ) {
    Serial.println("ERROR: JSON payload too large");
    return false;
  }

  HTTPClient http;
  int httpCode = -1;

  const String url = NODE_RED_URL;

  Serial.printf(
    "\nSending %s to Dashboard...\n",
    d.nodeId
  );

  Serial.printf("Payload: %s\n", payload);

  // ---------------- HTTPS ----------------

  if (url.startsWith("https://")) {
    WiFiClientSecure client;

    // TESTING ONLY:
    // Skips server certificate validation.
    // For production, configure a trusted CA certificate.
    client.setInsecure();
    client.setTimeout(HTTPS_TIMEOUT_MS);

    if (!http.begin(client, url)) {
      Serial.println("HTTPS begin failed");
      return false;
    }

    http.setConnectTimeout(20000);
http.setTimeout(30000);
http.setReuse(false);

    http.addHeader(
      "Content-Type",
      "application/json"
    );

    http.addHeader("x-api-key", API_KEY);

    httpCode = http.POST(
      (uint8_t*)payload,
      (size_t)payloadLength
    );

    if (httpCode > 0) {
      Serial.printf(
        "Dashboard HTTP status: %d\n",
        httpCode
      );

      String response = http.getString();

      Serial.printf(
        "Dashboard response: %s\n",
        response.c_str()
      );
    } else {
      Serial.printf(
        "Dashboard HTTPS error: %s (code %d)\n",
        http.errorToString(httpCode).c_str(),
        httpCode
      );
    }

    http.end();
  }

  // ---------------- HTTP ----------------

  else {
    WiFiClient client;

    if (!http.begin(client, url)) {
      Serial.println("HTTP begin failed");
      return false;
    }

    http.setConnectTimeout(20000);
http.setTimeout(30000);
http.setReuse(false);

    http.addHeader(
      "Content-Type",
      "application/json"
    );

    http.addHeader("x-api-key", API_KEY);

    httpCode = http.POST(
      (uint8_t*)payload,
      (size_t)payloadLength
    );

    if (httpCode > 0) {
      Serial.printf(
        "Dashboard HTTP status: %d\n",
        httpCode
      );

      String response = http.getString();

      Serial.printf(
        "Dashboard response: %s\n",
        response.c_str()
      );
    } else {
      Serial.printf(
        "Dashboard HTTP error: %s (code %d)\n",
        http.errorToString(httpCode).c_str(),
        httpCode
      );
    }

    http.end();
  }

  bool success =
    httpCode >= 200 && httpCode < 300;

  Serial.printf(
    "POST %s | T=%.1f C H=%.1f%% "
    "Smoke=%ld Fire=%s Result=%s\n",
    d.nodeId,
    d.temperature,
    d.humidity,
    (long)d.smoke_level,
    d.fire_detected ? "YES" : "NO",
    success ? "OK" : "FAILED"
  );

  return success;
}

// ==================================================
// 11. Read pending data without deleting it
// ==================================================

bool copyDirtyData(
  NodeSlot& slot,
  SensorData& out
) {
  bool available = false;

  portENTER_CRITICAL(&dataMux);

  if (slot.dirty) {
    memcpy(&out, &slot.data, sizeof(out));
    available = true;
  }

  portEXIT_CRITICAL(&dataMux);

  return available;
}

// ==================================================
// 12. Clear data only after successful POST
// Keep newer readings if they arrived during POST
// ==================================================

void clearSentData(
  NodeSlot& slot,
  const SensorData& sent
) {
  portENTER_CRITICAL(&dataMux);

  if (
    slot.dirty &&
    memcmp(&slot.data, &sent, sizeof(sent)) == 0
  ) {
    slot.dirty = false;
  }

  portEXIT_CRITICAL(&dataMux);
}

// ==================================================
// 13. Process each node with retry
// ==================================================

void processNodeData(
  NodeSlot& slot,
  unsigned long& lastAttempt
) {
  // Retry failed POST at most once every 10 seconds
  if (
    lastAttempt != 0 &&
    millis() - lastAttempt < POST_RETRY_MS
  ) {
    return;
  }

  SensorData d;

  if (!copyDirtyData(slot, d)) {
    return;
  }

  lastAttempt = millis();

  bool success = postNodeData(d);

  if (success) {
    clearSentData(slot, d);
  } else {
    Serial.printf(
      "%s data retained; retry in %lu seconds\n",
      d.nodeId,
      POST_RETRY_MS / 1000
    );
  }
}

// ==================================================
// 14. Setup
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println(
    "=== FOREST FIRE MASTER ==="
  );
  Serial.println(
    "ESP-NOW -> Render Dashboard"
  );

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lastWiFiRetry = millis();

  Serial.printf(
    "Connecting to Wi-Fi: %s\n",
    WIFI_SSID
  );

  unsigned long started = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - started < 20000
  ) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(
      "Master IP: %s\n",
      WiFi.localIP().toString().c_str()
    );

    Serial.printf(
      "Master STA MAC: %s\n",
      WiFi.macAddress().c_str()
    );

    Serial.printf(
      "Wi-Fi channel: %d\n",
      WiFi.channel()
    );

    initESPNow();
  } else {
    Serial.println(
      "Wi-Fi not connected yet; retrying in loop()"
    );
  }

  Serial.printf(
    "Reset reason: %d\n",
    (int)esp_reset_reason()
  );
}

// ==================================================
// 15. Main loop
// ==================================================

void loop() {
  ensureWiFi();

  if (
    WiFi.status() != WL_CONNECTED ||
    !espNowReady
  ) {
    delay(20);
    return;
  }

  processNodeData(
    node1Slot,
    lastNode1PostAttempt
  );

  processNodeData(
    node2Slot,
    lastNode2PostAttempt
  );

  delay(5);
}
