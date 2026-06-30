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
#include <OneWire.h>
#include <DallasTemperature.h>

// === HARDWARE PINS (ESP32 GPIO numbers) ===
// ESP32 has no D1..D8/A0 labels like the NodeMCU; use raw GPIOs.
// Avoid strapping pins (0,2,12,15), flash pins (6-11) and input-only pins (34-39) for outputs.
#define PUMP1_PIN 19
#define PUMP2_PIN 17
#define LED1_PIN  5
#define LED2_PIN  16
#define WATER_LEVEL_PIN 34   // input-only ADC pin, fine for a level sensor
#define STEPPER_PIN1 15
#define STEPPER_PIN2 2
#define STEPPER_PIN3 4
#define STEPPER_PIN4 18
#define LED_PIN 23  // NeoPixel 1 data pin
#define NEO2_PIN 22 // NeoPixel 2 data pin
#define ONE_WIRE_PIN 21 // DS18B20 data line (needs a 4.7k pull-up to 3.3V)

#define NUMPIXELS 16
#define NUMPIXELS2 30
Adafruit_NeoPixel neoPixel(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel neoPixel2(NUMPIXELS2, NEO2_PIN, NEO_GRB + NEO_KHZ800);

// NeoPixel strips as independent devices. Device id 5 -> strip 0, id 6 -> strip 1.
const uint8_t NEO_STRIP_COUNT = 2;
Adafruit_NeoPixel* neoStrips[NEO_STRIP_COUNT] = { &neoPixel, &neoPixel2 };
uint32_t lastNeoColor[NEO_STRIP_COUNT] = { 0xFF0000, 0xFF0000 }; // default red

// DS18B20 temperature sensor (OneWire)
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature tempSensor(&oneWire);
float currentTempC = NAN;             // last good reading; NaN until first read

// === CONFIG ===
const char* MDNS_NAME = "smarttank";
const char* NTP_POOL = "pool.ntp.org";
const char* SCHEDULE_FILE = "/schedules.json";

const int DEVICE_COUNT = 7; // 0: pump1, 1: pump2, 2: led1, 3: uv/led2, 4: stepper, 5: NeoPixel, 6: NeoPixel2
const int PWM_MAX = 1023;

// device names for UI/status
const char* deviceNames[DEVICE_COUNT] = { "Water pump", "Air pump", "LED", "UV", "Auto feeder", "Neo Pixel", "Neo Pixel 2" };
int devicePins[DEVICE_COUNT] = { PUMP1_PIN, PUMP2_PIN, LED1_PIN, LED2_PIN, -1, -1, -1 };

// runtime state
int deviceStates[DEVICE_COUNT]; // 0..PWM_MAX for PWM; for stepper we store last position
bool waterLevelHigh = false;

// Stepper
AccelStepper stepper(AccelStepper::FULL4WIRE, STEPPER_PIN1, STEPPER_PIN3, STEPPER_PIN2, STEPPER_PIN4);
bool stepperOutputsOn = false;

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

// DS18B20 non-blocking read timing
unsigned long lastTempRequest = 0;
bool tempConversionPending = false;
const unsigned long TEMP_INTERVAL_MS = 5000; // read every 5s
const unsigned long TEMP_CONVERSION_MS = 750; // 12-bit conversion time

// Start a non-blocking move and energize the coils. loop() de-energizes them
// once the move finishes, so the motor doesn't stay hot with 2 pins held HIGH.
void stepperMove(long steps) {
  stepper.enableOutputs();
  stepperOutputsOn = true;
  stepper.move(steps);
}

// Drive a PWM device from a 0..255 value. At max (255) we write the LEDC
// full-scale duty (PWM_MAX + 1 = 2^10), which the hardware holds constant-HIGH
// (true 100%); 1023 alone is only ~99.9% and won't fully open a marginal load.
void setDeviceLevel(int id, int value) {
  value = constrain(value, 0, 255);
  deviceStates[id] = map(value, 0, 255, 0, PWM_MAX);
  if (devicePins[id] >= 0) {
    ledcWrite(devicePins[id], value >= 255 ? (PWM_MAX + 1) : deviceStates[id]);
  }
}

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
  neoPixel2.begin();
  neoPixel2.show(); // all off

  // DS18B20 temperature sensor. Needs an external 4.7k pull-up from the data
  // line to 3.3V (the ESP32 internal pull-up is too weak for reliable OneWire).
  tempSensor.begin();
  tempSensor.setWaitForConversion(false); // don't block the loop ~750ms
  tempSensor.requestTemperatures();        // kick off the first conversion
  lastTempRequest = millis();
  tempConversionPending = true;
  Serial.printf("DS18B20 sensors found: %d\n", tempSensor.getDeviceCount());

  // pin mode & init for PWM devices
  for (int i=0;i<DEVICE_COUNT;i++) {
    if (devicePins[i] >= 0) {
      // ESP32 core 3.x PWM: attach the pin to an LEDC channel (5 kHz, 10-bit
      // -> duty 0..1023 = PWM_MAX). pinMode first to ensure the pad is an output.
      pinMode(devicePins[i], OUTPUT);
      ledcAttach(devicePins[i], 5000, 10);
      deviceStates[i] = 0;
      ledcWrite(devicePins[i], 0);
    } else {
      deviceStates[i] = 0;
    }
  }
  pinMode(WATER_LEVEL_PIN, INPUT);

  // Stepper
  // 28BYJ-48 on a ULN2003 slips/skips at higher speeds on 5V (silently dropping
  // steps so a "full" move falls short). 300 sps runs smoothly on this hardware.
  stepper.setMaxSpeed(300);
  stepper.setAcceleration(200);
  stepper.disableOutputs(); // start with coils off

  // Load schedules
  loadSchedulesFromFS();

  // WiFi manager (captive portal). It borrows `server` for the portal and
  // resets it when done, so our routes must be registered AFTER this returns.
  AsyncWiFiManager wifiManager(&server, &dns);
  wifiManager.autoConnect("SmartTankESP32");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect FAILED");
  }

  // Start the web server (after WiFiManager has released the shared server).
  setupRoutes();
  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
        // handle if needed
    });
  server.addHandler(&ws);
  server.begin();
  Serial.println("HTTP server started");

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
      if (devicePins[0] >= 0) ledcWrite(devicePins[0], 0);
      if (devicePins[1] >= 0) ledcWrite(devicePins[1], 0);
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

  // DS18B20 temperature: request, then read 750ms later (non-blocking)
  if (!tempConversionPending && nowMs - lastTempRequest >= TEMP_INTERVAL_MS) {
    tempSensor.requestTemperatures();
    lastTempRequest = nowMs;
    tempConversionPending = true;
  } else if (tempConversionPending && nowMs - lastTempRequest >= TEMP_CONVERSION_MS) {
    float t = tempSensor.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C) currentTempC = t;
    tempConversionPending = false;
  }

  // run stepper (non-blocking); de-energize coils once the move completes
  stepper.run();
  if (stepperOutputsOn && stepper.distanceToGo() == 0) {
    stepper.disableOutputs();
    stepperOutputsOn = false;
  }

  // yield
  yield();
}
