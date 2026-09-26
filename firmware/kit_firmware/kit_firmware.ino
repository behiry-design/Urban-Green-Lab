#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>   // needs ArduinoJson v7 (uses JsonDocument)
#include <DHT.h>

// ============================================================================
// ESP8266 VERSION (ported from the ESP32 sketch)
//
// The ESP8266 has only ONE analog input (A0), so two analog soil probes need
// extra hardware. Choose how station B's soil probe is read:
//
//   SOIL_MODE 0 : no extra parts (default)
//       Station A soil: AO pin -> A0            (real 0-100 % from voltage)
//       Station B soil: DO pin -> D7            (only 0 % dry / 100 % wet,
//                                                threshold set by the blue pot)
//   SOIL_MODE 1 : ADS1115 ADC module (both probes analog)
//       ADS1115: VDD 3V3, GND GND, SDA D2, SCL D1, ADDR GND
//       Probe A AO -> ADS1115 A0,  Probe B AO -> ADS1115 A1
//       Library: "Adafruit ADS1X15"
//
// Power both soil modules and both DHT11s from 3V3 (never 5V on the analog
// outputs). DHT11 A data -> D5, DHT11 B data -> D6.
// ============================================================================
#define SOIL_MODE 0

#if SOIL_MODE == 1
  #include <Wire.h>
  #include <Adafruit_ADS1X15.h>
  Adafruit_ADS1115 ads;
  bool adsOk = false;
#endif

// ============================================================================
// CONFIG — edit this block for your board, nothing else in this file.
// ============================================================================

const char* WIFI_SSID     = "network_name";
const char* WIFI_PASSWORD = "password";

// Given by the instructor once (same for every board — this is the shared
// platform's address, not a secret).
const char* SUPABASE_URL      = "https://onrozqpzbaavvkiqqmkz.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im9ucm96cXB6YmFhdnZraXFxbWt6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODg4OTk4MzgsImV4cCI6MjEwNDQ3NTgzOH0.uufWko5ciOuoxzmzzM8eYlahDd4LFP-GIywzdAXprh8";

// --- Station A: the first kit this board reports for --------------------
// Given to you on that kit's card. Do not share outside that team —
// device_token is effectively the kit's password.
const char* KIT_ID_A       = "K01";
const char* DEVICE_TOKEN_A = "2453125ad31f8604";
const int   DHT_PIN_A      = 14;    // DHT11 data pin for station A

// --- Station B: the second kit this board reports for --------------------
const char* KIT_ID_B       = "K02";
const char* DEVICE_TOKEN_B = "f9af0cf8b59d1e0c";
const int   DHT_PIN_B      = 12;    // DHT11 data pin for station B

const int   SOIL_DO_PIN_B  = A0;    // SOIL_MODE 0 only: station B soil module DO pin

#define DHT_TYPE DHT11

// How often to send a reading. Start with 5 minutes during class so everyone
// can see live data quickly; feel free to lengthen it (e.g. 15-30 min) once
// your kits are running unattended after training, to save battery/bandwidth.
// Both stations are read and sent once per cycle, a few hundred ms apart.
const unsigned long UPLOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 minutes

// ============================================================================
// You shouldn't need to change anything below this line.
// ============================================================================

DHT dhtA(DHT_PIN_A, DHT_TYPE);
DHT dhtB(DHT_PIN_B, DHT_TYPE);

// Dry-air / wet-soil calibration in VOLTS, one pair per probe (index 0 = A,
// 1 = B). Resistive probe modules output a HIGH voltage when dry and a LOW
// voltage when wet. Measure yours: read the voltage in dry air and in a cup of
// water, then replace these numbers. (Station B is unused in SOIL_MODE 0.)
const float SOIL_DRY_V[2] = {3.20, 3.20};
const float SOIL_WET_V[2] = {1.20, 1.20};

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.printf("  waiting... WiFi status = %d\n", WiFi.status());
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect failed — will retry next cycle.");
  }
}

// Reads the analog soil voltage for probe idx (0 or 1). NAN if unavailable.
float readAnalogVolts(uint8_t idx) {
#if SOIL_MODE == 1
  if (!adsOk) return NAN;
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += ads.readADC_SingleEnded(idx);
    delay(2);
  }
  return ads.computeVolts(sum / 8);
#else
  (void)idx;   // only probe A is on A0 in this mode
  long sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(A0);
    delay(2);
  }
  // NodeMCU / Wemos D1 Mini: 3.3 V = 1023 (built-in divider).
  // On a bare ESP-12 module the full scale is 1.0 V instead.
  return (sum / 16) * 3.3f / 1023.0f;
#endif
}

// Converts a soil voltage into an approximate 0-100% moisture value using that
// probe's own calibration. Probes read HIGHER when dry and LOWER when wet —
// this flips that so the number matches what a person expects ("higher = wetter").
float soilVoltsToPercent(float volts, float dryV, float wetV) {
  float pct = 100.0 * (dryV - volts) / (dryV - wetV);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// Returns moisture % for station idx (0 = A, 1 = B), or NAN on failure.
// soilV is set to the measured voltage, or NAN when the reading is digital.
float readSoilPercent(uint8_t idx, float &soilV) {
  soilV = NAN;
#if SOIL_MODE == 0
  if (idx == 1) {
    // No second ADC: use the module's DO pin. HIGH = dry, LOW = wet.
    return digitalRead(SOIL_DO_PIN_B) == HIGH ? 0.0f : 100.0f;
  }
#endif
  soilV = readAnalogVolts(idx);
  if (isnan(soilV)) return NAN;
  return soilVoltsToPercent(soilV, SOIL_DRY_V[idx], SOIL_WET_V[idx]);
}

// kitId/token identify WHICH of the two kits this particular reading belongs
// to — that's the only thing that changes between the two calls each cycle.
bool sendReading(const char* kitId, const char* token, float tempC, float humidityPct, float soilPct) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping upload — no WiFi.");
    return false;
  }

  Serial.printf("[%s] Free heap before upload: %u bytes\n", kitId, ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();   // trust any TLS cert — simplest option for a short
                           // training deployment; see the setup guide for
                           // the (optional) hardened alternative.

  HTTPClient http;
  String endpoint = String(SUPABASE_URL) + "/rest/v1/rpc/insert_reading";
  if (!http.begin(client, endpoint)) {
    Serial.println("http.begin() failed.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);

  JsonDocument doc;
  doc["p_kit_id"]   = kitId;
  doc["p_token"]    = token;
  doc["p_temp_c"]   = tempC;
  doc["p_humidity"] = humidityPct;
  doc["p_soil"]     = soilPct;
  doc["p_light"]    = nullptr;   // set this if a kit has a light sensor
  doc["p_extra"]    = nullptr;   // e.g. {"co2_ppm": 480} if you add more sensors

  String body;
  serializeJson(doc, body);

  int status = http.POST(body);
  String response = http.getString();
  http.end();

  if (status == 200 || status == 204) {
    Serial.printf("[%s] Reading uploaded OK.\n", kitId);
    return true;
  }
  Serial.printf("[%s] Upload failed (HTTP %d): %s\n", kitId, status, response.c_str());
  return false;
}

// Reads one station's sensors and sends the reading under its own kit
// identity. Returns false (and skips the upload) if a sensor read failed.
bool readAndSendStation(const char* label, const char* kitId, const char* token,
                         DHT& dht, uint8_t idx) {
  float humidity = dht.readHumidity();
  float tempC    = dht.readTemperature();
  float soilV;
  float soilPct  = readSoilPercent(idx, soilV);

  if (isnan(humidity) || isnan(tempC)) {
    Serial.printf("[%s/%s] DHT11 read failed — check wiring. Skipping this station this cycle.\n", label, kitId);
    return false;
  }
  if (isnan(soilPct)) {
    Serial.printf("[%s/%s] Soil read failed — check wiring / ADS1115. Skipping this station this cycle.\n", label, kitId);
    return false;
  }

  if (isnan(soilV)) {
    Serial.printf("[%s/%s] Temp: %.1f C  Humidity: %.1f%%  Soil: %.0f%% (DO pin, dry/wet only)\n",
                  label, kitId, tempC, humidity, soilPct);
  } else {
    Serial.printf("[%s/%s] Temp: %.1f C  Humidity: %.1f%%  Soil: %.1f%% (%.2f V)\n",
                  label, kitId, tempC, humidity, soilPct, soilV);
  }
  return sendReading(kitId, token, tempC, humidity, soilPct);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== KIT FIRMWARE START ===");
  Serial.printf("Reset reason: %s | free heap: %u bytes\n",
                ESP.getResetReason().c_str(), ESP.getFreeHeap());
  dhtA.begin();
  dhtB.begin();
#if SOIL_MODE == 1
  Wire.begin();   // SDA = D2, SCL = D1
  adsOk = ads.begin(0x48);
  if (adsOk) {
    ads.setGain(GAIN_ONE);   // +/-4.096 V range
  } else {
    Serial.println("ADS1115 NOT found — check SDA/SCL wiring and address.");
  }
#else
  pinMode(SOIL_DO_PIN_B, INPUT);
#endif

connectWiFi();
}

void loop() {
  readAndSendStation("A", KIT_ID_A, DEVICE_TOKEN_A, dhtA, 0);
  delay(500);   // brief gap between the two uploads — no need to fire them back-to-back
  readAndSendStation("B", KIT_ID_B, DEVICE_TOKEN_B, dhtB, 1);

  Serial.printf("Cycle done. Sleeping %lu s until the next reading...\n", UPLOAD_INTERVAL_MS / 1000UL);
  delay(UPLOAD_INTERVAL_MS);
}