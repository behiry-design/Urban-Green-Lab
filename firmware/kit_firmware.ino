/* ============================================================================
   Smart Greenhouse Training Program — Kit Firmware (ESP32)
   ============================================================================
   What this does, every UPLOAD_INTERVAL_MS:
     1. Reads temperature + humidity (DHT11) and soil moisture (capacitive
        analog probe), and light level if you wired one up.
     2. Sends those numbers straight to your team's row in the shared
        platform by calling the "insert_reading" function over HTTPS.
     3. Repeats forever.

   ---- Everything YOUR team needs to edit is in the CONFIG block below. -----
   Everything after that block is the same for every kit — you shouldn't need
   to touch it, but reading through it is a good way to see how an ESP32
   talks to a web API.

   Libraries to install first (Arduino IDE: Tools > Manage Libraries):
     - "DHT sensor library" by Adafruit
     - "Adafruit Unified Sensor" (a dependency of the above)
     - "ArduinoJson" by Benoit Blanchon — version 7.x (this sketch uses the
       v7 JsonDocument API; installing via Library Manager gets you the
       latest 7.x by default)
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

// Given to your team on your kit card during training. Do not share these
// outside your team — device_token is effectively your kit's password.
const char* KIT_ID        = "K01";
const char* DEVICE_TOKEN  = "REPLACE_WITH_YOUR_TEAMS_TOKEN";

// Given by the instructor once (same for every team — this is the shared
// platform's address, not a secret).
const char* SUPABASE_URL      = "https://YOUR-PROJECT-REF.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";

// Which pin your soil moisture probe's analog output is wired to.
const int SOIL_PIN = 34;      // ESP32 ADC1 pin — change if you wired it elsewhere
const int DHT_PIN  = 4;       // DHT11 data pin
#define DHT_TYPE DHT11

// How often to send a reading. Start with 5 minutes during class so everyone
// can see live data quickly; feel free to lengthen it (e.g. 15-30 min) once
// your kit is running unattended after training, to save battery/bandwidth.
const unsigned long UPLOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 minutes

// ============================================================================
// You shouldn't need to change anything below this line.
// ============================================================================

DHT dht(DHT_PIN, DHT_TYPE);

// Dry-air / wet-soil calibration for the capacitive probe. These are typical
// raw ADC values for common ESP32 capacitive soil sensors, but every probe
// is slightly different — the CALIBRATION section of the training covers how
// to measure your own SOIL_DRY_RAW / SOIL_WET_RAW and replace these.
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

// Converts a raw analogRead() into an approximate 0-100% moisture value.
// Capacitive probes read LOWER when wet, HIGHER when dry — this flips that
// so the number matches what a person expects ("higher = wetter").
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
