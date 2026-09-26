/* ============================================================================
   Smart Greenhouse Training Program — Kit Firmware (ESP32)
   ============================================================================
   Every UPLOAD_INTERVAL_MS: reads temp/humidity (DHT11), soil moisture
   (capacitive analog probe), and light level if wired up, then POSTs them
   to the shared platform via the insert_reading RPC over HTTPS.

   Edit the CONFIG block below for your team; nothing else needs to change.

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
// CONFIG — edit these seven lines for your team, nothing else in this file.
// ============================================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// From your kit card. Keep private — device_token is basically your kit's password.
const char* KIT_ID        = "K01";
const char* DEVICE_TOKEN  = "REPLACE_WITH_YOUR_TEAMS_TOKEN";

// From the instructor, same for every team — not a secret, just the platform URL.
const char* SUPABASE_URL      = "https://YOUR-PROJECT-REF.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";

const int SOIL_PIN = 34;      // ESP32 ADC1 pin — change if you wired it elsewhere
const int DHT_PIN  = 4;       // DHT11 data pin
#define DHT_TYPE DHT11

// 5 min is good for class visibility; bump to 15-30 min once the kit is
// running unattended after training, to save battery/bandwidth.
const unsigned long UPLOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 minutes

// ============================================================================
// You shouldn't need to change anything below this line.
// ============================================================================

DHT dht(DHT_PIN, DHT_TYPE);

// Typical raw ADC values for these capacitive probes, but every probe
// differs — see the CALIBRATION section of the training doc to measure and
// replace your own SOIL_DRY_RAW / SOIL_WET_RAW.
const int SOIL_DRY_RAW = 3000;   // raw analogRead() value in dry air
const int SOIL_WET_RAW = 1200;   // raw analogRead() value in a cup of water

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

// Capacitive probes read lower when wet, higher when dry — flip it so the
// number matches what people expect ("higher = wetter").
float soilRawToPercent(int raw) {
  float pct = 100.0 * (float)(SOIL_DRY_RAW - raw) / (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

bool sendReading(float tempC, float humidityPct, float soilPct) {
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
  doc["p_kit_id"]   = KIT_ID;
  doc["p_token"]    = DEVICE_TOKEN;
  doc["p_temp_c"]   = tempC;
  doc["p_humidity"] = humidityPct;
  doc["p_soil"]     = soilPct;
  doc["p_light"]    = nullptr;   // set this if your kit has a light sensor
  doc["p_extra"]    = nullptr;   // e.g. {"co2_ppm": 480} if you add more sensors

  String body;
  serializeJson(doc, body);

  int status = http.POST(body);
  String response = http.getString();
  http.end();

  if (status == 200 || status == 204) {
    Serial.println("Reading uploaded OK.");
    return true;
  }
  Serial.printf("Upload failed (HTTP %d): %s\n", status, response.c_str());
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);   // ESP32 default; raw values 0-4095
  dht.begin();
  connectWiFi();
}

void loop() {
  connectWiFi();

  float humidity = dht.readHumidity();
  float tempC    = dht.readTemperature();
  int soilRaw    = analogRead(SOIL_PIN);
  float soilPct  = soilRawToPercent(soilRaw);

  if (isnan(humidity) || isnan(tempC)) {
    Serial.println("DHT11 read failed — check wiring. Skipping this cycle.");
  } else {
    Serial.printf("Temp: %.1f C  Humidity: %.1f%%  Soil: %.1f%% (raw %d)\n",
                  tempC, humidity, soilPct, soilRaw);
    sendReading(tempC, humidity, soilPct);
  }

  delay(UPLOAD_INTERVAL_MS);
}
