#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <AccelStepper.h>
#include <ESPmDNS.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>

// === HARDWARE PINS (ESP32 GPIO numbers) ===
// ESP32 has no D1..D8/A0 labels like the NodeMCU; use raw GPIOs.
// Avoid strapping pins (0,2,12,15), flash pins (6-11) and input-only pins (34-39) for outputs.
#define PUMP1_PIN 25
#define PUMP2_PIN 26
#define LED1_PIN  27
#define LED2_PIN  14
#define WATER_LEVEL_PIN 34   // input-only ADC pin, fine for a level sensor
#define STEPPER_PIN1 16
#define STEPPER_PIN2 17
#define STEPPER_PIN3 18
#define STEPPER_PIN4 19
#define LED_PIN 23  // NeoPixel data pin

#define NUMPIXELS 24
Adafruit_NeoPixel neoPixel(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
uint32_t lastNeoColor = 0xFF0000; // default red

// === CONFIG ===
const char* MDNS_NAME = "smarttank";
const char* NTP_POOL = "pool.ntp.org";
const char* SCHEDULE_FILE = "/schedules.json";

const int DEVICE_COUNT = 6; // 0: pump1, 1: pump2, 2: led1, 3: uv/led2, 4: stepper, 5: NeoPixel
const int PWM_MAX = 1023;

// device names for UI/status
const char* deviceNames[DEVICE_COUNT] = { "Water pump", "Air pump", "LED", "UV", "Auto feeder", "Neo Pixel" };
int devicePins[DEVICE_COUNT] = { PUMP1_PIN, PUMP2_PIN, LED1_PIN, LED2_PIN, -1, -1 };

// runtime state
int deviceStates[DEVICE_COUNT]; // 0..PWM_MAX for PWM; for stepper we store last position
bool waterLevelHigh = false;

// Stepper
AccelStepper stepper(AccelStepper::FULL4WIRE, STEPPER_PIN1, STEPPER_PIN3, STEPPER_PIN2, STEPPER_PIN4);

// Web / networking
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

DNSServer dns;

// schedule structure
struct ScheduleEntry {
  uint8_t deviceId;   // which device
  uint8_t hour;       // 0..23
  uint8_t minute;     // 0..59
  String type;        // "on","off","value","color","stepper"
  String data;        // for "value" or "stepper" it's numeric string; for "color" it's "RRGGBB"
  uint8_t brightness; // used for color (0..255)
  bool enabled;
};
String scheduleBody;

std::vector<ScheduleEntry> schedules;

// timing
unsigned long lastWaterCheck = 0;
const unsigned long WATER_CHECK_INTERVAL = 2000; // ms
unsigned long lastScheduleApply = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL_MS = 1000; // check every second, trigger once per minute

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("SmartTank starting (clean build)...");

  // LittleFS init
  if (!LittleFS.begin()) {
    Serial.println("LittleFS.mount() failed");
  } else {
    Serial.println("LittleFS mounted");
  }

  // NeoPixel init
  neoPixel.begin();
  neoPixel.show(); // all off

  // PWM range
  analogWriteRange(PWM_MAX);

  // pin mode & init for PWM devices
  for (int i=0;i<DEVICE_COUNT;i++) {
    if (devicePins[i] >= 0) {
      pinMode(devicePins[i], OUTPUT);
      deviceStates[i] = 0;
      analogWrite(devicePins[i], 0);
    } else {
      deviceStates[i] = 0;
    }
  }
  pinMode(WATER_LEVEL_PIN, INPUT);

  // Stepper
  stepper.setMaxSpeed(500);
  stepper.setAcceleration(200);

  // Load schedules
  loadSchedulesFromFS();

  // WiFi manager (captive portal)
  AsyncWiFiManager wifiManager(&server, &dns);
  wifiManager.autoConnect("SmartTankESP32");

  // OTA
  ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA end"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { Serial.printf("OTA: %u%%\r", (progress * 100) / total); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA Error: %u\n", e); });
  ArduinoOTA.begin();

  // NTP
  setupNTP();

  // mDNS
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS responder started: http://%s.local/\n", MDNS_NAME);
  } else {
    Serial.println("mDNS failed");
  }

  // Routes
  setupRoutes();

  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
        // handle if needed
    });
    server.addHandler(&ws);
  server.begin();
  Serial.println("HTTP server started");
}


unsigned long checkTickMs = 0;
int lastMinuteSeen = -1;

void loop() {
  ArduinoOTA.handle();
  updateNeoPixelFade();

  unsigned long nowMs = millis();

  // Water level check (throttled)
  if (nowMs - lastWaterCheck >= WATER_CHECK_INTERVAL) {
    lastWaterCheck = nowMs;
    waterLevelHigh = (digitalRead(WATER_LEVEL_PIN) == HIGH);
    if (waterLevelHigh) {
      // disable pumps
      deviceStates[0] = deviceStates[1] = 0;
      if (devicePins[0] >= 0) analogWrite(devicePins[0], 0);
      if (devicePins[1] >= 0) analogWrite(devicePins[1], 0);
      Serial.println("Water level HIGH -> pumps disabled");
      // persist schedules/states optionally (we only persist schedules in FS)
    }
  }

  // Schedule checking: run every second, trigger on minute change
  if (nowMs - lastScheduleApply >= SCHEDULE_CHECK_INTERVAL_MS) {
    lastScheduleApply = nowMs;
    int nowMin = minuteOfDayNow();
    if (nowMin != lastMinuteSeen) {
      lastMinuteSeen = nowMin;
      // iterate schedules and trigger those matching current minute
      for (const auto &s: schedules) {
        if (!s.enabled) continue;
        if (s.hour == (nowMin / 60) && s.minute == (nowMin % 60)) {
          logMsg("Triggering schedule ID=" + String(s.deviceId) +
                       " type=" + s.type +
                       " value=" + s.data);
          executeScheduleEntry(s);
        }
      }
    }
  }

  // run stepper (non-blocking)
  stepper.run();

  // yield
  yield();
}
