/*
  FOREST FIRE SLAVE NODE 1  (fixed version)
  Board: ESP32-C3 SuperMini
  Arduino ESP32 Core: 3.x

  DHT22 DATA -> GPIO3
  MQ-2 AOUT  -> GPIO1

  Communication:
    ESP-NOW -> Master ESP32
  No WiFi.begin() on Slave.

  CHANGES:
    - Slave scans for the Master's hotspot (HOTSPOT_SSID) and uses the
      same Wi-Fi channel automatically (no fixed channel).
    - If sending fails 3 times in a row, it re-scans the channel.
    - peer.channel = 0 (follow the current channel).
    - Lower TX power option for ESP32-C3 SuperMini boards.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <DHT.h>
#include <string.h>

// ---------- Node ----------
#define NODE_ID "node1"

// ---------- Sensors ----------
#define DHTPIN 3
#define DHTTYPE DHT22
#define MQ2PIN 1

// ---------- Hotspot (same SSID that Master connects to) ----------
// Slave does NOT connect to it; it only scans to learn the channel.
const char* HOTSPOT_SSID = "Iphone 17 pro";

// Channel used if the hotspot is not found yet
uint8_t espnowChannel = 6;

// Lower TX power helps some ESP32-C3 SuperMini boards.
// Set to 0 if you don't need it.
#define USE_LOW_TX_POWER 1

// Re-scan the channel after this many consecutive send failures
const int MAX_FAILS_BEFORE_RESCAN = 3;

// ---------- Timing ----------
const unsigned long SEND_INTERVAL_MS = 5000;
const unsigned long ESPNOW_RETRY_MS = 3000;

// ---------- Master MAC (must be Master's STA MAC) ----------
uint8_t MASTER_MAC[] = {
  0xFC, 0xE8, 0xC0, 0xE1, 0x3A, 0x18
};

// ---------- Sensor data (must match master.ino) ----------
typedef struct __attribute__((packed)) {
  char nodeId[10];
  float temperature;
  float humidity;
  int32_t smoke_level;
  uint8_t fire_detected;
} SensorData;

DHT dht(DHTPIN, DHTTYPE);

// ---------- State ----------
bool espNowReady = false;
int failCount = 0;

unsigned long lastSend = 0;
unsigned long lastEspNowAttempt = 0;

volatile bool callbackPending = false;
volatile bool callbackSuccess = false;

// ---------- ESP-NOW callback: Arduino Core 3.x ----------
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  (void)info;
  callbackSuccess = (status == ESP_NOW_SEND_SUCCESS);
  callbackPending = true;
}

// ---------- Find hotspot channel by scanning ----------
bool syncChannel() {
  Serial.printf("Scanning for hotspot \"%s\"...\n", HOTSPOT_SSID);

  int n = WiFi.scanNetworks();
  bool found = false;

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == HOTSPOT_SSID) {
      espnowChannel = (uint8_t)WiFi.channel(i);
      found = true;
      break;
    }
  }
  WiFi.scanDelete();

  if (found) {
    Serial.printf("Hotspot found, channel = %d\n", espnowChannel);
  } else {
    Serial.printf(
      "Hotspot not found; keeping channel %d\n",
      espnowChannel
    );
  }

  esp_err_t err = esp_wifi_set_channel(
    espnowChannel,
    WIFI_SECOND_CHAN_NONE
  );
  if (err != ESP_OK) {
    Serial.printf("Set channel failed: %s\n", esp_err_to_name(err));
    return false;
  }
  return found;
}

// ---------- Initialize ESP-NOW ----------
bool initESPNow() {
  if (espNowReady) {
    return true;
  }

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("ESP-NOW init failed: %s\n", esp_err_to_name(err));
    return false;
  }

  err = esp_now_register_send_cb(onDataSent);
  if (err != ESP_OK) {
    Serial.printf("Register callback failed: %s\n", esp_err_to_name(err));
    esp_now_deinit();
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = 0;              // 0 = use current channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(MASTER_MAC)) {
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
      Serial.printf("Add Master peer failed: %s\n", esp_err_to_name(err));
      esp_now_deinit();
      return false;
    }
  }

  espNowReady = true;

  Serial.println("ESP-NOW initialized");
  Serial.printf("Node ID: %s\n", NODE_ID);
  Serial.printf("Slave MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Channel: %d\n", espnowChannel);

  return true;
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== FOREST FIRE SLAVE 1 ===");

  dht.begin();
  analogReadResolution(12);
  pinMode(MQ2PIN, INPUT);

  // Station mode only. Do not connect the Slave to the hotspot.
  WiFi.mode(WIFI_STA);
  delay(300);
  WiFi.setSleep(false);

#if USE_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif

  Serial.print("Slave STA MAC: ");
  Serial.println(WiFi.macAddress());

  // Learn the channel of the hotspot the Master is connected to.
  syncChannel();

  lastEspNowAttempt = millis();
  initESPNow();
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  // 1. Print send callback result and track failures.
  if (callbackPending) {
    bool success = callbackSuccess;
    callbackPending = false;

    Serial.printf(
      "ESP-NOW send status: %s\n",
      success ? "SUCCESS" : "FAILED"
    );

    if (success) {
      failCount = 0;
    } else if (++failCount >= MAX_FAILS_BEFORE_RESCAN) {
      Serial.println("Too many failures; re-scanning channel");
      syncChannel();
      failCount = 0;
    }
  }

  // 2. Retry ESP-NOW initialization if needed.
  if (!espNowReady) {
    if (now - lastEspNowAttempt >= ESPNOW_RETRY_MS) {
      lastEspNowAttempt = now;
      syncChannel();
      initESPNow();
    }
    delay(10);
    return;
  }

  // 3. Send sensor data every 5 seconds.
  if (now - lastSend < SEND_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSend = now;

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT22 read failed; packet skipped");
    return;
  }

  // 4. Prepare packet.
  SensorData data = {};
  strncpy(data.nodeId, NODE_ID, sizeof(data.nodeId) - 1);
  data.temperature = temperature;
  data.humidity = humidity;
  data.smoke_level = analogRead(MQ2PIN);
  data.fire_detected = 0;   // Master evaluates thresholds

  // 5. Send to Master.
  esp_err_t result = esp_now_send(
    MASTER_MAC,
    reinterpret_cast<const uint8_t*>(&data),
    sizeof(data)
  );

  Serial.printf(
    "%s | Temp: %.1f C | Humidity: %.1f %% | Smoke: %ld | Send: %s\n",
    NODE_ID,
    temperature,
    humidity,
    (long)data.smoke_level,
    result == ESP_OK ? "QUEUED" : esp_err_to_name(result)
  );
}
