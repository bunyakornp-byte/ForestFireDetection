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

#include <Arduino.h> // เรียกใช้ฟังก์ชันพื้นฐานของ Arduino เช่น Serial และ delay
#include <WiFi.h> // เรียกใช้ไลบรารีเชื่อมต่อ Wi-Fi ของ ESP32
#include <esp_now.h> // เรียกใช้ไลบรารีสื่อสาร ESP-NOW ระหว่างบอร์ด
#include <esp_wifi.h> // เรียกใช้ฟังก์ชันระดับต่ำสำหรับ Wi-Fi
#include <esp_system.h> // เรียกใช้ข้อมูลระบบ ESP32 เช่นสาเหตุการรีเซ็ต
#include <HTTPClient.h> // เรียกใช้ไลบรารีส่ง HTTP/HTTPS request
#include <WiFiClient.h> // เรียกใช้ client สำหรับการเชื่อมต่อ HTTP แบบไม่เข้ารหัส
#include <WiFiClientSecure.h> // เรียกใช้ client สำหรับ HTTPS
#include <math.h> // เรียกใช้ฟังก์ชันคณิตศาสตร์ เช่น isfinite
#include <string.h> // เรียกใช้ฟังก์ชันจัดการข้อความ/หน่วยความจำ เช่น strcmp และ memcpy
#include <LittleFS.h> // ใช้ Flash ภายใน ESP32 เก็บคิวข้อมูลที่ยังส่งไม่ได้
#include <time.h> // อ่าน Unix time หลังซิงก์เวลา NTP

// ================================================== — หมายเหตุ: ==================================================
// 1. Wi-Fi settings — หมายเหตุ: 1. Wi-Fi settings
// ================================================== — หมายเหตุ: ==================================================

const char* WIFI_SSID = "Iphone 17 pro"; // กำหนดชื่อเครือข่าย Wi-Fi ที่ Master จะเชื่อมต่อ
const char* WIFI_PASSWORD = "12345678"; // กำหนดรหัสผ่าน Wi-Fi; ควรเปลี่ยนหากเป็นรหัสจริงที่เปิดเผย

// ================================================== — หมายเหตุ: ==================================================
// 2. Render Dashboard settings — หมายเหตุ: 2. Render Dashboard settings
// ================================================== — หมายเหตุ: ==================================================

const char* NODE_RED_URL = // ประกาศตัวแปรเก็บ URL ปลายทางของ API บน Render
  "https://forest-fire-dashboard-1sjj.onrender.com/firedata"; // กำหนด endpoint /firedata ที่รับข้อมูลเซนเซอร์

// Must match the API_KEY environment variable on Render — หมายเหตุ: Must match the API_KEY environment variable on Render
const char* API_KEY = "zmd9bfytghv3pu7cjear1x2s0in64wqol5k8"; // กำหนด API key สำหรับยืนยันสิทธิ์กับเซิร์ฟเวอร์ ควรเก็บเป็นความลับ

// ================================================== — หมายเหตุ: ==================================================
// 3. Fire detection thresholds — หมายเหตุ: 3. Fire detection thresholds
// ================================================== — หมายเหตุ: ==================================================

const float TEMP_THRESHOLD = 50.0; // กำหนดอุณหภูมิตั้งแต่ 50°C ให้เข้าเงื่อนไขแจ้งเตือนไฟ
const float HUMIDITY_THRESHOLD = 20.0; // กำหนดความชื้นไม่เกิน 20% ให้เข้าเงื่อนไขแจ้งเตือนไฟ
const int SMOKE_THRESHOLD = 2000; // กำหนดค่า smoke ตั้งแต่ 2000 ให้เข้าเงื่อนไขแจ้งเตือนไฟ

const unsigned long WIFI_RETRY_MS = 10000; // กำหนดช่วงเวลาลองเชื่อม Wi-Fi ใหม่ 10,000 มิลลิวินาที (10 วินาที)
const unsigned long POST_RETRY_MS = 10000; // กำหนดช่วงเวลารอก่อนส่งข้อมูลซ้ำ 10 วินาที
const unsigned long HTTPS_TIMEOUT_MS = 15000; // จำกัดเวลา HTTPS เพื่อให้ loop กลับมาประมวลผลข้อมูลได้เร็วขึ้น
const char* PENDING_FILE = "/pending.csv";
const size_t MAX_PENDING_RECORDS = 100; // จำกัดคิว Flash ไม่ให้โตไม่สิ้นสุด
size_t cachedPendingCount = 0;
bool queueCountInitialized = false;

// ================================================== — หมายเหตุ: ==================================================
// 4. ESP-NOW data structure — หมายเหตุ: 4. ESP-NOW data structure
// Must match the structure used by both Slaves — หมายเหตุ: Must match the structure used by both Slaves
// ================================================== — หมายเหตุ: ==================================================

typedef struct __attribute__((packed)) { // ประกาศโครงสร้างข้อมูล SensorData และ packed ลด padding ระหว่างฟิลด์
  char nodeId[10]; // เก็บชื่อโหนด เช่น node1 หรือ node2 เป็นข้อความสูงสุด 9 ตัวอักษรบวกตัวจบ
  float temperature; // เก็บค่าอุณหภูมิจากโหนดลูก
  float humidity; // เก็บค่าความชื้นจากโหนดลูก
  int32_t smoke_level; // เก็บค่าระดับควันที่เป็นจำนวนเต็ม 32 บิต
  uint8_t fire_detected; // เก็บสถานะตรวจพบไฟเป็นเลข 0 หรือ 1
} SensorData; // จบคำสั่ง C++ บรรทัดนี้

// ประกาศชนิดข้อมูลคิวออฟไลน์ไว้ก่อนฟังก์ชัน เพื่อให้ Arduino auto-prototype รู้จักชนิดนี้
struct PendingRecord {
  String recordId;
  uint64_t recordedAt;
  SensorData data;
};

struct RxRecord {
  SensorData data;
  uint64_t recordedAt;
  char recordId[81];
};
const size_t RX_QUEUE_CAPACITY = 32;
RxRecord rxQueue[RX_QUEUE_CAPACITY];
volatile size_t rxHead = 0, rxTail = 0, rxCount = 0;
volatile uint32_t rxDropped = 0;

// ================================================== — หมายเหตุ: ==================================================
// 5. Node data storage — หมายเหตุ: 5. Node data storage
// ================================================== — หมายเหตุ: ==================================================

struct NodeSlot { // ประกาศโครงสร้างเก็บข้อมูลล่าสุดและสถานะการส่งของแต่ละโหนด
  SensorData data; // เก็บข้อมูลเซนเซอร์ของโหนด
  bool dirty; // ระบุว่ามีข้อมูลใหม่ที่ยังรอส่งไป Dashboard หรือไม่
  bool seen; // ระบุว่าเคยได้รับข้อมูลจากโหนดนี้แล้วหรือยัง
  unsigned long lastSeen; // เก็บเวลาที่ได้รับข้อมูลล่าสุดจากโหนดนี้
}; // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

NodeSlot node1Slot = {}; // สร้างช่องเก็บข้อมูลของ node1 และเริ่มต้นสมาชิกเป็นศูนย์/false
NodeSlot node2Slot = {}; // สร้างช่องเก็บข้อมูลของ node2 และเริ่มต้นสมาชิกเป็นศูนย์/false

portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED; // สร้างตัวล็อกสำหรับป้องกันข้อมูลชนกันระหว่าง callback กับ loop

bool espNowReady = false; // เก็บสถานะว่าเริ่มต้น ESP-NOW สำเร็จแล้วหรือยัง

unsigned long lastWiFiRetry = 0; // เก็บเวลาที่ลองเชื่อม Wi-Fi ครั้งล่าสุด
unsigned long lastNode1PostAttempt = 0; // เก็บเวลาที่พยายามส่งข้อมูล node1 ครั้งล่าสุด
unsigned long lastNode2PostAttempt = 0; // เก็บเวลาที่พยายามส่งข้อมูล node2 ครั้งล่าสุด

// ================================================== — หมายเหตุ: ==================================================
// 6. Fire detection — หมายเหตุ: 6. Fire detection
// ================================================== — หมายเหตุ: ==================================================

bool isFire(const SensorData& d) { // ฟังก์ชันตรวจว่าข้อมูลเซนเซอร์เข้าเงื่อนไขไฟหรือไม่
  return // ส่งค่าผลลัพธ์กลับจากฟังก์ชัน
    d.temperature >= TEMP_THRESHOLD || // เป็นจริงเมื่ออุณหภูมิสูงกว่าหรือเท่าค่ากำหนด
    d.humidity <= HUMIDITY_THRESHOLD || // เป็นจริงเมื่อความชื้นต่ำกว่าหรือเท่าค่ากำหนด
    d.smoke_level >= SMOKE_THRESHOLD; // เป็นจริงเมื่อค่าควันสูงกว่าหรือเท่าค่ากำหนด
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ================================================== — หมายเหตุ: ==================================================
// 7. ESP-NOW receive callback — หมายเหตุ: 7. ESP-NOW receive callback
// ESP32 Arduino Core 3.x — หมายเหตุ: ESP32 Arduino Core 3.x
// ================================================== — หมายเหตุ: ==================================================

void onDataReceive( // ประกาศฟังก์ชันที่ไม่ส่งค่ากลับ
  const esp_now_recv_info_t* info, // ประกาศค่าคงที่ที่ไม่ต้องการให้แก้ไขระหว่างทำงาน
  const uint8_t* incomingData, // ประกาศค่าคงที่ที่ไม่ต้องการให้แก้ไขระหว่างทำงาน
  int len // ประกาศตัวแปรจำนวนเต็ม
) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
  (void)info; // ระบุว่าไม่ได้ใช้งานพารามิเตอร์ info เพื่อหลีกเลี่ยงคำเตือนจากคอมไพเลอร์

  if (len != (int)sizeof(SensorData)) { // ตรวจว่าขนาดแพ็กเก็ตตรงกับโครงสร้าง SensorData หรือไม่
    return; // ออกจากฟังก์ชันเพราะ Wi-Fi เชื่อมต่อแล้ว
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  SensorData d; // สร้างตัวแปรไว้รับสำเนาข้อมูลที่จะส่ง
  memcpy(&d, incomingData, sizeof(d)); // คัดลอกข้อมูลไบต์ที่รับมาเข้าสู่ตัวแปร SensorData

  // Ensure nodeId is null-terminated — หมายเหตุ: Ensure nodeId is null-terminated
  d.nodeId[sizeof(d.nodeId) - 1] = '\0'; // บังคับให้ตัวอักษรสุดท้ายของ nodeId เป็นตัวจบข้อความ ป้องกันการอ่านเกิน

  // Accept only node1 and node2 — หมายเหตุ: Accept only node1 and node2
  if ( // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
    strcmp(d.nodeId, "node1") != 0 && // ตรวจว่า nodeId ไม่ใช่ node1
    strcmp(d.nodeId, "node2") != 0 // ตรวจว่า nodeId ไม่ใช่ node2
  ) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    return; // ออกจากฟังก์ชันเพราะ Wi-Fi เชื่อมต่อแล้ว
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // Reject invalid sensor readings — หมายเหตุ: Reject invalid sensor readings
  if ( // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
    !isfinite(d.temperature) || // ปฏิเสธข้อมูลหากอุณหภูมิเป็น NaN หรือ infinity
    !isfinite(d.humidity) // ปฏิเสธข้อมูลหากความชื้นเป็น NaN หรือ infinity
  ) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    return; // ออกจากฟังก์ชันเพราะ Wi-Fi เชื่อมต่อแล้ว
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // Calculate fire status at Master — หมายเหตุ: Calculate fire status at Master
  d.fire_detected = isFire(d) ? 1 : 0; // คำนวณสถานะไฟใหม่ที่ Master แล้วเก็บเป็น 1 หรือ 0

  // Store latest data safely — หมายเหตุ: Store latest data safely
  portENTER_CRITICAL(&dataMux); // ล็อกส่วนวิกฤตเพื่อไม่ให้ task อื่นแก้ข้อมูลพร้อมกัน

  NodeSlot* slot = // ประกาศ pointer ไปยังช่องข้อมูลของโหนดที่ตรงกับ nodeId
    (strcmp(d.nodeId, "node1") == 0) // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
      ? &node1Slot // ถ้าเป็น node1 ให้เลือกช่อง node1Slot
      : &node2Slot; // มิฉะนั้นเลือกช่อง node2Slot

  memcpy(&slot->data, &d, sizeof(d)); // คัดลอกข้อมูลใหม่ลงช่องเก็บข้อมูลของโหนดที่เลือก

  slot->dirty = true; // ทำเครื่องหมายว่ามีข้อมูลใหม่รอส่งขึ้น Dashboard
  slot->seen = true; // บันทึกว่าได้รับข้อมูลจากโหนดนี้แล้ว
  slot->lastSeen = millis(); // บันทึกเวลา uptime ปัจจุบันที่ได้รับข้อมูลล่าสุด

  // Enqueue each received sample so blocking HTTPS cannot overwrite it.
  if (rxCount >= RX_QUEUE_CAPACITY) {
    rxHead = (rxHead + 1) % RX_QUEUE_CAPACITY;
    rxCount--;
    rxDropped++;
  }
  rxQueue[rxTail].data = d;
  time_t callbackTime = time(nullptr);
  rxQueue[rxTail].recordedAt = (callbackTime > 1700000000) ? (uint64_t)callbackTime * 1000ULL : 0;
  snprintf(rxQueue[rxTail].recordId, sizeof(rxQueue[rxTail].recordId), "%08lx-%lu-%08lx",
    (unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFF), (unsigned long)millis(), (unsigned long)esp_random());
  rxTail = (rxTail + 1) % RX_QUEUE_CAPACITY;
  rxCount++;

  portEXIT_CRITICAL(&dataMux); // ปลดล็อกส่วนวิกฤตหลังแก้ไขข้อมูลเสร็จ
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ================================================== — หมายเหตุ: ==================================================
// 8. Initialize ESP-NOW — หมายเหตุ: 8. Initialize ESP-NOW
// ================================================== — หมายเหตุ: ==================================================

bool initESPNow() { // ฟังก์ชันเริ่มต้นการสื่อสาร ESP-NOW และคืนค่าสำเร็จ/ไม่สำเร็จ
  if (espNowReady) { // หาก Wi-Fi หลุดและ ESP-NOW เคยพร้อม ให้ปิด ESP-NOW ก่อน
    return true; // คืนค่าสำเร็จ
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  esp_err_t err = esp_now_init(); // เริ่มต้นระบบ ESP-NOW และเก็บรหัสผลลัพธ์

  if (err != ESP_OK) { // ตรวจว่าการเริ่มต้น ESP-NOW ล้มเหลวหรือไม่
    Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
      "ESP-NOW init failed: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
      esp_err_to_name(err) // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  err = esp_now_register_recv_cb(onDataReceive); // ลงทะเบียนฟังก์ชัน callback ที่จะทำงานเมื่อมีข้อมูลเข้ามา

  if (err != ESP_OK) { // ตรวจว่าการเริ่มต้น ESP-NOW ล้มเหลวหรือไม่
    Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
      "Register receive callback failed: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
      esp_err_to_name(err) // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    esp_now_deinit(); // ปิด ESP-NOW เพื่อเตรียมเริ่มต้นใหม่หลัง Wi-Fi กลับมา
    return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  espNowReady = true; // บันทึกว่าสื่อสาร ESP-NOW พร้อมใช้งานแล้ว

  Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
    "ESP-NOW ready; Wi-Fi channel %d\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    WiFi.channel() // อ่านช่องสัญญาณ Wi-Fi ที่ใช้อยู่
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  return true; // คืนค่าสำเร็จ
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ================================================== — หมายเหตุ: ==================================================
// 9. Maintain Wi-Fi connection — หมายเหตุ: 9. Maintain Wi-Fi connection
// ================================================== — หมายเหตุ: ==================================================

void ensureWiFi() { // ฟังก์ชันตรวจสอบและพยายามเชื่อมต่อ Wi-Fi ใหม่
  if (WiFi.status() == WL_CONNECTED) { // ตรวจว่า Wi-Fi เชื่อมต่อสำเร็จอยู่หรือไม่
    static bool timeSyncRequested = false;
    if (!timeSyncRequested) {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      timeSyncRequested = true;
    }
    if (!espNowReady) { // หาก Wi-Fi ต่อแล้วแต่ ESP-NOW ยังไม่พร้อม ให้เริ่ม ESP-NOW
      initESPNow(); // เรียกฟังก์ชันเริ่มต้น ESP-NOW
    } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    return; // ออกจากฟังก์ชันเพราะ Wi-Fi เชื่อมต่อแล้ว
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // Keep ESP-NOW initialized while Internet/Wi-Fi reconnects. The radio must
  // remain on the same channel as the Slaves; see README for channel caveat.
  if (millis() - lastWiFiRetry >= WIFI_RETRY_MS) { // ตรวจว่าครบช่วงเวลาที่กำหนดให้ลองต่อ Wi-Fi ใหม่หรือยัง
    lastWiFiRetry = millis(); // ตั้งเวลาเริ่มต้นสำหรับการคำนวณช่วง retry

    Serial.println( // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
      "Wi-Fi disconnected; reconnecting..." // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    WiFi.disconnect(false, false); // ตัดการเชื่อมต่อ Wi-Fi ปัจจุบันโดยไม่ล้างการตั้งค่าถาวร
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // เริ่มเชื่อมต่อกับ Wi-Fi ที่กำหนด
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ================================================== — หมายเหตุ: ==================================================
// 10. Send sensor data to Render — หมายเหตุ: 10. Send sensor data to Render
// ================================================== — หมายเหตุ: ==================================================

bool postNodeData(const SensorData& d, const char* recordId = nullptr, uint64_t recordedAt = 0, bool replayed = false) { // ฟังก์ชันส่งข้อมูลเซนเซอร์หนึ่งโหนดไปยัง API และคืนผลสำเร็จ
  if (WiFi.status() != WL_CONNECTED) { // ถ้า Wi-Fi ยังไม่เชื่อมต่อ จะไม่ส่งข้อมูล
    Serial.println("Wi-Fi disconnected; POST skipped"); // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
    return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  if ( // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
    String(NODE_RED_URL).indexOf("YOUR-APP-NAME") >= 0 || // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    String(API_KEY) == "YOUR_RENDER_API_KEY" // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
  ) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    Serial.println( // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
      "ERROR: Configure NODE_RED_URL and API_KEY first" // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // Construct JSON payload — หมายเหตุ: Construct JSON payload
  char payload[384]; // จองบัฟเฟอร์ JSON รวม metadata สำหรับข้อมูลย้อนหลัง
  String generatedId;
  if (!recordId || !recordId[0]) {
    generatedId = String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX) + "-" + String(millis()) + "-" + String(esp_random(), HEX);
    recordId = generatedId.c_str();
  }
  if (recordedAt == 0 && !replayed) {
    time_t nowSeconds = time(nullptr);
    if (nowSeconds > 1700000000) recordedAt = (uint64_t)nowSeconds * 1000ULL;
  }

  int payloadLength = snprintf(
    payload, sizeof(payload),
    "{\"nodeId\":\"%s\","
    "\"temperature\":%.2f,"
    "\"humidity\":%.2f,"
    "\"smoke_level\":%ld,"
    "\"fire_detected\":%s,"
    "\"recordId\":\"%s\","
    "\"recordedAt\":%llu,"
    "\"replayed\":%s}",
    d.nodeId, d.temperature, d.humidity, (long)d.smoke_level,
    d.fire_detected ? "true" : "false", recordId,
    (unsigned long long)recordedAt, replayed ? "true" : "false"
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  if ( // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
    payloadLength < 0 || // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    payloadLength >= (int)sizeof(payload) // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
  ) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    Serial.println("ERROR: JSON payload too large"); // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
    return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  HTTPClient http; // สร้างออบเจ็กต์สำหรับส่ง HTTP request
  int httpCode = -1; // ตั้งรหัสผลตอบกลับเริ่มต้นเป็น -1 หมายถึงยังไม่ได้รับรหัส HTTP

  const String url = NODE_RED_URL; // แปลง URL ปลายทางเป็นชนิด String เพื่อใช้งานสะดวก

  Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
    "\nSending %s to Dashboard...\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    d.nodeId // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  Serial.printf("Payload: %s\n", payload); // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor

  // ---------------- HTTPS ---------------- — หมายเหตุ: ---------------- HTTPS ----------------

  if (url.startsWith("https://")) { // เลือกวิธีเชื่อมต่อแบบ HTTPS หาก URL ขึ้นต้นด้วย https://
    WiFiClientSecure client; // สร้าง client สำหรับการเชื่อมต่อ HTTPS

    // TESTING ONLY: — หมายเหตุ: TESTING ONLY:
    // Skips server certificate validation. — หมายเหตุ: Skips server certificate validation.
    // For production, configure a trusted CA certificate. — หมายเหตุ: For production, configure a trusted CA certificate.
    client.setInsecure(); // ข้ามการตรวจใบรับรอง TLS ใช้เพื่อทดสอบเท่านั้น ไม่ปลอดภัยสำหรับงานจริง
    client.setTimeout(HTTPS_TIMEOUT_MS); // กำหนดเวลารอของ secure client

    if (!http.begin(client, url)) { // เริ่มต้น HTTPClient ด้วย secure client และ URL หากไม่สำเร็จให้หยุด
      Serial.println("HTTPS begin failed"); // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
      return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
    } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.setConnectTimeout(20000); // กำหนดเวลารอเชื่อมต่อสูงสุด 20 วินาที
http.setTimeout(30000); // กำหนดเวลารอการทำงาน/รับข้อมูลสูงสุด 30 วินาที
http.setReuse(false); // ไม่ใช้การเชื่อมต่อเดิมซ้ำใน request ถัดไป

    http.addHeader( // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      "Content-Type", // ระบุชื่อ header ที่บอกชนิดข้อมูลใน request
      "application/json" // ระบุว่า body ที่ส่งเป็นข้อมูล JSON
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.addHeader("x-api-key", API_KEY); // เพิ่ม API key ใน header เพื่อให้เซิร์ฟเวอร์ตรวจสิทธิ์

    httpCode = http.POST( // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      (uint8_t*)payload, // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
      (size_t)payloadLength // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    if (httpCode > 0) { // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard HTTP status: %d\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        httpCode // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

      String response = http.getString(); // อ่านข้อความตอบกลับจากเซิร์ฟเวอร์

      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard response: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        response.c_str() // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    } else { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard HTTPS error: %s (code %d)\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        http.errorToString(httpCode).c_str(), // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
        httpCode // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.end(); // ปิด request และคืนทรัพยากรของ HTTPClient
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // ---------------- HTTP ---------------- — หมายเหตุ: ---------------- HTTP ----------------

  else { // ทำงานในกรณีที่เงื่อนไข if ก่อนหน้าไม่เป็นจริง
    WiFiClient client; // จบคำสั่ง C++ บรรทัดนี้

    if (!http.begin(client, url)) { // เริ่มต้น HTTPClient ด้วย secure client และ URL หากไม่สำเร็จให้หยุด
      Serial.println("HTTP begin failed"); // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
      return false; // จบฟังก์ชันและส่งค่ากลับ (ถ้ามี)
    } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.setConnectTimeout(20000); // กำหนดเวลารอเชื่อมต่อสูงสุด 20 วินาที
http.setTimeout(30000); // กำหนดเวลารอการทำงาน/รับข้อมูลสูงสุด 30 วินาที
http.setReuse(false); // ไม่ใช้การเชื่อมต่อเดิมซ้ำใน request ถัดไป

    http.addHeader( // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      "Content-Type", // ระบุชื่อ header ที่บอกชนิดข้อมูลใน request
      "application/json" // ระบุว่า body ที่ส่งเป็นข้อมูล JSON
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.addHeader("x-api-key", API_KEY); // เพิ่ม API key ใน header เพื่อให้เซิร์ฟเวอร์ตรวจสิทธิ์

    httpCode = http.POST( // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      (uint8_t*)payload, // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
      (size_t)payloadLength // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    if (httpCode > 0) { // ตรวจสอบเงื่อนไข; คำสั่งในบล็อกจะทำงานเมื่อเงื่อนไขเป็นจริง
      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard HTTP status: %d\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        httpCode // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

      String response = http.getString(); // อ่านข้อความตอบกลับจากเซิร์ฟเวอร์

      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard response: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        response.c_str() // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    } else { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
      Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
        "Dashboard HTTP error: %s (code %d)\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
        http.errorToString(httpCode).c_str(), // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
        httpCode // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
      ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    http.end(); // ปิด request และคืนทรัพยากรของ HTTPClient
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  bool success = // ประกาศตัวแปรบอกว่าการส่งสำเร็จหรือไม่
    httpCode >= 200 && httpCode < 300; // ถือว่าสำเร็จเมื่อรหัส HTTP อยู่ในช่วง 200–299

  Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
    "POST %s | T=%.1f C H=%.1f%% " // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    "Smoke=%ld Fire=%s Result=%s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    d.nodeId, // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    d.temperature, // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    d.humidity, // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    (long)d.smoke_level, // เป็นส่วนของรายการพารามิเตอร์หรือการเรียกฟังก์ชัน
    d.fire_detected ? "YES" : "NO", // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    success ? "OK" : "FAILED" // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  return success; // คืนผลสำเร็จ/ล้มเหลวของการส่งข้อมูล
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ---------- Flash-backed offline queue ----------
size_t pendingCount() {
  if (queueCountInitialized) return cachedPendingCount;
  cachedPendingCount = 0;
  if (LittleFS.exists(PENDING_FILE)) {
    File f = LittleFS.open(PENDING_FILE, "r");
    if (f) {
      while (f.available()) { String line = f.readStringUntil('\n'); if (line.length()) cachedPendingCount++; }
      f.close();
    }
  }
  queueCountInitialized = true;
  return cachedPendingCount;
}

bool appendPending(const SensorData& d, uint64_t capturedAt = 0, const char* capturedId = nullptr) {
  if (pendingCount() >= MAX_PENDING_RECORDS) {
    Serial.println("WARNING: Flash pending queue full; keep newest reading in RAM for retry");
    return false;
  }
  File f = LittleFS.open(PENDING_FILE, "a");
  if (!f) { Serial.println("ERROR: Cannot open Flash queue"); return false; }
  uint64_t recordedAt = capturedAt;
  if (recordedAt == 0) {
    time_t nowSeconds = time(nullptr);
    if (nowSeconds > 1700000000) recordedAt = (uint64_t)nowSeconds * 1000ULL;
  }
  String generatedId;
  if (!capturedId || !capturedId[0]) {
    generatedId = String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX) + "-" + String(millis()) + "-" + String(esp_random(), HEX);
    capturedId = generatedId.c_str();
  }
  char line[220];
  int n = snprintf(line, sizeof(line), "%s|%llu|%s|%.2f|%.2f|%ld|%u\n",
    capturedId, (unsigned long long)recordedAt, d.nodeId,
    d.temperature, d.humidity, (long)d.smoke_level, (unsigned)d.fire_detected);
  bool ok = n > 0 && n < (int)sizeof(line) && f.print(line) == (size_t)n;
  f.flush(); f.close();
  if (ok) {
    cachedPendingCount++;
    Serial.printf("Saved %s to LittleFS queue (%u/%u)\n", d.nodeId, (unsigned)pendingCount(), (unsigned)MAX_PENDING_RECORDS);
  }
  return ok;
}

bool readPendingHead(PendingRecord& out) {
  if (!LittleFS.exists(PENDING_FILE)) return false;
  File f = LittleFS.open(PENDING_FILE, "r");
  if (!f) return false;
  String line = f.readStringUntil('\n'); f.close();
  line.trim();
  if (!line.length()) return false;
  char id[81] = {0}, node[10] = {0};
  unsigned long long ts = 0;
  float temp = 0, hum = 0; long smoke = 0; unsigned fire = 0;
  int parsed = sscanf(line.c_str(), "%80[^|]|%llu|%9[^|]|%f|%f|%ld|%u", id, &ts, node, &temp, &hum, &smoke, &fire);
  if (parsed != 7 || (strcmp(node, "node1") != 0 && strcmp(node, "node2") != 0)) return false;
  out.recordId = id; out.recordedAt = (uint64_t)ts;
  memset(&out.data, 0, sizeof(out.data));
  strncpy(out.data.nodeId, node, sizeof(out.data.nodeId) - 1);
  out.data.temperature = temp; out.data.humidity = hum; out.data.smoke_level = smoke; out.data.fire_detected = fire ? 1 : 0;
  return true;
}

bool removePendingHead() {
  if (!LittleFS.exists(PENDING_FILE)) return true;
  File in = LittleFS.open(PENDING_FILE, "r");
  if (!in) return false;
  String first = in.readStringUntil('\n');
  File out = LittleFS.open("/pending.tmp", "w");
  if (!out) { in.close(); return false; }
  while (in.available()) { String line = in.readStringUntil('\n'); if (line.length()) out.println(line); }
  in.close(); out.flush(); out.close();
  LittleFS.remove(PENDING_FILE);
  bool renamed = LittleFS.rename("/pending.tmp", PENDING_FILE);
  if (renamed && cachedPendingCount > 0) cachedPendingCount--;
  return renamed;
}

bool flushPendingHead() {
  PendingRecord rec;
  if (!readPendingHead(rec)) return false;
  if (!postNodeData(rec.data, rec.recordId.c_str(), rec.recordedAt, true)) return false;
  return removePendingHead();
}

bool popRxRecord(RxRecord& out) {
  bool available = false;
  portENTER_CRITICAL(&dataMux);
  if (rxCount > 0) {
    out = rxQueue[rxHead];
    rxHead = (rxHead + 1) % RX_QUEUE_CAPACITY;
    rxCount--;
    available = true;
  }
  portEXIT_CRITICAL(&dataMux);
  return available;
}

void drainReceivedSamples() {
  // Process a limited batch per loop. Samples received during a blocking POST
  // remain in the RAM ring until the next loop iteration.
  for (size_t i = 0; i < 8; i++) {
    // Do not dequeue RAM samples when the persistent queue is full; keep them
    // buffered until a successful upload frees Flash queue space.
    if (pendingCount() >= MAX_PENDING_RECORDS) break;
    RxRecord rec;
    if (!popRxRecord(rec)) break;
    if (WiFi.status() != WL_CONNECTED || pendingCount() > 0) {
      if (!appendPending(rec.data, rec.recordedAt, rec.recordId)) {
        Serial.println("ERROR: Could not persist sample; Flash queue is full or unavailable");
      }
    } else if (!postNodeData(rec.data, rec.recordId, rec.recordedAt, false)) {
      if (!appendPending(rec.data, rec.recordedAt, rec.recordId)) {
        Serial.println("ERROR: POST failed and sample could not be queued in Flash");
      }
    }
  }
  if (rxDropped > 0) {
    static uint32_t lastReportedDrop = 0;
    if (rxDropped != lastReportedDrop) {
      Serial.printf("WARNING: RX RAM queue overflow; dropped total=%lu\n", (unsigned long)rxDropped);
      lastReportedDrop = rxDropped;
    }
  }
}

// ================================================== — หมายเหตุ: ==================================================
// 14. Setup — หมายเหตุ: 14. Setup
// ================================================== — หมายเหตุ: ==================================================

void setup() { // ประกาศฟังก์ชันที่ไม่ส่งค่ากลับ
  Serial.begin(115200); // เริ่ม Serial Monitor ที่ความเร็ว 115200 baud
  delay(800); // รอ 800 มิลลิวินาทีให้ระบบเริ่มทำงาน

  Serial.println(); // พิมพ์บรรทัดว่างเพื่อจัดรูปแบบ Serial Monitor
  Serial.println( // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
    "=== FOREST FIRE MASTER ===" // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
  Serial.println("ESP-NOW + LittleFS offline queue -> Render Dashboard");
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS mount failed; offline persistence unavailable");
  } else {
    Serial.printf("LittleFS ready; pending records=%u\n", (unsigned)pendingCount());
  }

  WiFi.mode(WIFI_STA); // ตั้ง Wi-Fi เป็นโหมด Station เพื่อเชื่อมกับเราเตอร์/ฮอตสปอต
  WiFi.setSleep(false); // ปิดโหมดประหยัดพลังงาน Wi-Fi เพื่อช่วยให้การสื่อสารต่อเนื่อง

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // เริ่มเชื่อมต่อกับ Wi-Fi ที่กำหนด

  lastWiFiRetry = millis(); // ตั้งเวลาเริ่มต้นสำหรับการคำนวณช่วง retry

  Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
    "Connecting to Wi-Fi: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    WIFI_SSID // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  unsigned long started = millis(); // บันทึกเวลาเริ่มต้นของช่วงรอเชื่อมต่อ

  while ( // เป็นส่วนหนึ่งของคำสั่ง C++ ในบล็อกนี้
    WiFi.status() != WL_CONNECTED && // วนรอขณะยังไม่เชื่อมต่อ Wi-Fi
    millis() - started < 20000 // จำกัดการรอเชื่อมต่อครั้งแรกไว้ไม่เกิน 20 วินาที
  ) { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    delay(300); // รอ 300 มิลลิวินาทีก่อนตรวจสถานะอีกครั้ง
    Serial.print("."); // พิมพ์จุดแสดงความคืบหน้าระหว่างรอเชื่อมต่อ
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  Serial.println(); // พิมพ์บรรทัดว่างเพื่อจัดรูปแบบ Serial Monitor

  if (WiFi.status() == WL_CONNECTED) { // ตรวจว่า Wi-Fi เชื่อมต่อสำเร็จอยู่หรือไม่
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
      "Master IP: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
      WiFi.localIP().toString().c_str() // แปลง IP ที่บอร์ดได้รับเป็นข้อความเพื่อพิมพ์
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
      "Master STA MAC: %s\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
      WiFi.macAddress().c_str() // แปลง MAC address ของ Wi-Fi เป็นข้อความเพื่อพิมพ์
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
      "Wi-Fi channel: %d\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
      WiFi.channel() // อ่านช่องสัญญาณ Wi-Fi ที่ใช้อยู่
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

    initESPNow(); // เรียกฟังก์ชันเริ่มต้น ESP-NOW
  } else { // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
    Serial.println( // พิมพ์ข้อความลง Serial Monitor แล้วขึ้นบรรทัดใหม่
      "Wi-Fi not connected yet; retrying in loop()" // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
  } // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

  // Initialize ESP-NOW even if Wi-Fi has not connected yet.
  if (!espNowReady) initESPNow();

  Serial.printf( // พิมพ์ข้อความพร้อมค่าตัวแปรลง Serial Monitor
    "Reset reason: %d\n", // เป็นส่วนหนึ่งของข้อความ/สตริงที่ส่งให้คำสั่งก่อนหน้า
    (int)esp_reset_reason() // อ่านสาเหตุการรีเซ็ตของ ESP32 แล้วแปลงเป็น int
  ); // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน
} // วงเล็บ/ปีกกานี้ใช้กำหนดขอบเขตของคำสั่งหรือฟังก์ชัน

// ================================================== — หมายเหตุ: ==================================================
// 15. Main loop — หมายเหตุ: 15. Main loop
// ================================================== — หมายเหตุ: ==================================================

void loop() {
  ensureWiFi();
  drainReceivedSamples();

  if (WiFi.status() == WL_CONNECTED && pendingCount() > 0) {
    static unsigned long lastQueueAttempt = 0;
    if (lastQueueAttempt == 0 || millis() - lastQueueAttempt >= POST_RETRY_MS) {
      lastQueueAttempt = millis();
      if (flushPendingHead()) {
        Serial.printf("Offline queue remaining: %u\n", (unsigned)pendingCount());
      }
    }
  }

  delay(5);
}
