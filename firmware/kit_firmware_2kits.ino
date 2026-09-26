/* ============================================================================
   Smart Greenhouse Training Program — Kit Firmware (ESP32, 2 kits per board)
   ============================================================================
   Since ESP32 boards are limited, this sketch drives TWO independent sensor
   stations — "A" and "B" — on one board, each with its own DHT11 + soil
   probe, reporting under its own kit_id / device_token. The platform just
   sees two kits; nothing on the dashboard or database side knows they share
   hardware.

   Every UPLOAD_INTERVAL_MS:
     1. Reads temp/humidity (DHT11) and soil moisture (resistive analog
        probe) for station A, sends it under KIT_ID_A.
     2. Does the same for station B, sent under KIT_ID_B.

   Edit the CONFIG block below for your board; nothing else needs to change.

   WIRING NOTE: ESP32's ADC2 pins (0, 2, 4, 12-15, 25-27) are unreliable for
   analog reads while WiFi is active — a known ESP32 quirk, not a bug here.
   Both soil probes must go on ADC1 pins (GPIO32-39); this sketch uses
   GPIO34 and GPIO35. The two DHT11s are on GPIO4/5 — ordinary digital pins,
   no ADC conflict, so any free GPIO works there. If your board doesn't
   break out these pins, swap them in CONFIG but keep the same category
   (ADC1 for soil, any digital pin for DHT).

   Libraries (Arduino IDE: Tools > Manage Libraries):
     - "DHT sensor library" by Adafruit
     - "Adafruit Unified Sensor" (dependency of the above)
     - "ArduinoJson" by Benoit Blanchon, v7.x (uses the JsonDocument API)
   Board: whichever ESP32 dev board you were given (Tools > Board).
   ========================================================================= */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ============================================================================
// CONFIG — edit this block for your board, nothing else in this file.
// ============================================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// From the instructor, same for every board — not a secret, just the platform URL.
const char* SUPABASE_URL      = "https://YOUR-PROJECT-REF.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";

// --- Station A: the first kit this board reports for --------------------
// From that kit's card. Keep private — device_token is basically the kit's password.
const char* KIT_ID_A       = "K01";
const char* DEVICE_TOKEN_A = "REPLACE_WITH_KIT_A_TOKEN";
const int   DHT_PIN_A      = 4;    // DHT11 data pin for station A
const int   SOIL_PIN_A     = 34;   // ESP32 ADC1 pin for station A's soil probe

// --- Station B: the second kit this board reports for --------------------
const char* KIT_ID_B       = "K02";
const char* DEVICE_TOKEN_B = "REPLACE_WITH_KIT_B_TOKEN";
const int   DHT_PIN_B      = 5;    // DHT11 data pin for station B
const int   SOIL_PIN_B     = 35;   // ESP32 ADC1 pin for station B's soil probe

#define DHT_TYPE DHT11

// 5 min is good for class visibility; bump to 15-30 min once the kits are
// running unattended after training, to save battery/bandwidth. Both
// stations are read and sent once per cycle, a few hundred ms apart.
const unsigned long UPLOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 minutes

// ============================================================================
// You shouldn't need to change anything below this line.
// ============================================================================

DHT dhtA(DHT_PIN_A, DHT_TYPE);
DHT dhtB(DHT_PIN_B, DHT_TYPE);

// Dry-air/wet-soil calibration, one pair per probe — every probe differs.
// See the CALIBRATION section of the training doc to measure and replace
// these four numbers.
const int SOIL_DRY_RAW_A = 3000;   // raw analogRead() value in dry air, probe A
const int SOIL_WET_RAW_A = 1200;   // raw analogRead() value in a cup of water, probe A
const int SOIL_DRY_RAW_B = 3000;   // raw analogRead() value in dry air, probe B
const int SOIL_WET_RAW_B = 1200;   // raw analogRead() value in a cup of water, probe B

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect failed — will retry next cycle.");
  }
}

// Probes read lower when wet, higher when dry — flip it so the number
// matches what people expect ("higher = wetter").
float soilRawToPercent(int raw, int dryRaw, int wetRaw) {
  float pct = 100.0 * (float)(dryRaw - raw) / (float)(dryRaw - wetRaw);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// kitId/token select which of the two kits this reading is for.
bool sendReading(const char* kitId, const char* token, float tempC, float humidityPct, float soilPct) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping upload — no WiFi.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();   // skip cert verification — fine for a short training deployment

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

// Reads one station and sends the reading under its own kit identity.
// Returns false (and skips the upload) if the DHT read failed.
bool readAndSendStation(const char* label, const char* kitId, const char* token,
                         DHT& dht, int soilPin, int dryRaw, int wetRaw) {
  float humidity = dht.readHumidity();
  float tempC    = dht.readTemperature();
  int soilRaw    = analogRead(soilPin);
  float soilPct  = soilRawToPercent(soilRaw, dryRaw, wetRaw);

  if (isnan(humidity) || isnan(tempC)) {
    Serial.printf("[%s/%s] DHT11 read failed — check wiring. Skipping this station this cycle.\n", label, kitId);
    return false;
  }

  Serial.printf("[%s/%s] Temp: %.1f C  Humidity: %.1f%%  Soil: %.1f%% (raw %d)\n",
                label, kitId, tempC, humidity, soilPct, soilRaw);
  return sendReading(kitId, token, tempC, humidity, soilPct);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);   // ESP32 default; raw values 0-4095
  dhtA.begin();
  dhtB.begin();
  connectWiFi();
}

void loop() {
  connectWiFi();

  readAndSendStation("A", KIT_ID_A, DEVICE_TOKEN_A, dhtA, SOIL_PIN_A, SOIL_DRY_RAW_A, SOIL_WET_RAW_A);
  delay(500);   // brief gap between the two uploads — no need to fire them back-to-back
  readAndSendStation("B", KIT_ID_B, DEVICE_TOKEN_B, dhtB, SOIL_PIN_B, SOIL_DRY_RAW_B, SOIL_WET_RAW_B);

  delay(UPLOAD_INTERVAL_MS);
}
