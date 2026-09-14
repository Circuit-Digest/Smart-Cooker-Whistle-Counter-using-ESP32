/*
  Cooker Whistle Counter with WhatsApp Alert - ESP32 + MAX4466
  CircuitDigest Cloud (no display, headless)

  Dashboard:
    analog-input-1 (Slider, 0-20, direction: output->device)  -> Set target whistle count
    analog-input-2 (Toggle/Switch, direction: output->device) -> Start/Stop listening (turn ON to arm, turns itself OFF once target whistle count is reached and alert is sent)
    analog-input-3 (Gauge/Number, direction: input->dashboard, optional) -> Live whistle count while listening
    analog-input-4 (Gauge/Number, direction: input->dashboard) -> Live dB level, updated continuously (~1x/sec)

  Detection logic:
    A whistle is a sharp dB spike that stays loud for a short burst then dies away. We track an envelope dB level, and count one whistle every time the level rises above WHISTLE_ON_DB, stays above it for at least MIN_WHISTLE_MS, then falls back below WHISTLE_OFF_DB. A cooldown after each detected whistle prevents one long whistle from being counted twice.

  On every power-up/reset: whistle count, target, listening state, and the live dB/count dashboard widgets are all forced back to zero/off, so the dashboard never shows a stale value left over from before the reset.

  Wiring (Generic ESP32 Dev Kit / DevKitC):
    MAX4466 VCC -> 3.3V
    MAX4466 GND -> GND
    MAX4466 OUT -> GPIO34   (ADC1_CH6, input-only, safe from Serial/USB/WiFi)

  NOTE: GPIO1 (used in some XIAO ESP32-S3 reference sketches as "A0") is actually the UART TX0 pin on a generic ESP32 DevKitC — it's reserved for USB-serial communication. Reading the mic there conflicts with Serial and causes garbage output / boot issues. GPIO34 avoids that entirely, and also stays reliable with WiFi active (ADC2 pins can get flaky when WiFi is on; GPIO34 is ADC1).
*/

#include <CircuitDigestCloud.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ── Fill in your credentials ────────────────────────────────────────────
#define WIFI_SSID      "Semicon ACT"
#define WIFI_PASS      "cracksen1605"
#define DEVICE_ID      "8b004f85-ce3b-40c6-a9c5-a1679e575f97"
#define CONNECTION_KEY "de71717c7463c778fd105334ba686a5f"
#define API_KEY        "cd_moh_240626_11WynF"
#define PHONE_NUMBER   "917539912128"

#define KEY_TARGET_SET   "analog-input-1"   // slider: desired whistle count
#define KEY_LISTEN_SW    "analog-input-2"   // switch: start/stop listening
#define KEY_LIVE_COUNT   "analog-input-3"   // live whistle count readback
#define KEY_LIVE_DB      "analog-input-4"   // live dB level readback

const char* host = "www.circuitdigest.cloud";
// ──────────────────────────────────────────────────────────────────────

CircuitDigestCloud CDcloud;

// ---------- Mic Config ----------
#define AUDIO_PIN 34             // GPIO34 (ADC1_CH6) — generic ESP32 Dev Kit / DevKitC
#define SAMPLE_WINDOW 20         // ms per envelope sample (fast, for bursts)
#define ADC_RESOLUTION 4095.0
#define V_REF 3.3

// ---------- Whistle Detection ----------
#define WHISTLE_ON_DB       60.0    // dB level that counts as "whistle sound"
#define WHISTLE_OFF_DB      50.0    // must drop below this to end the whistle
#define MIN_WHISTLE_MS      100UL   // must stay loud at least this long
#define WHISTLE_COOLDOWN_MS 10000UL // ignore new whistles for this long after one ends

// ---------- State ----------
bool     isListening      = false;
int      targetWhistles   = 0;      // starts at 0 until dashboard slider sets it
int      whistleCount     = 0;

bool     inWhistle        = false;  // currently inside a loud burst
unsigned long whistleStartMs = 0;
unsigned long cooldownUntilMs = 0;

#define CLOUD_PUBLISH_MS 1000UL     // how often live dB is pushed to the dashboard
unsigned long lastCloudPublish = 0;

// Raw diagnostics from the last readDBLevel() call — printed in loop() so
// you can see WHY dB is stuck instead of just the final number.
unsigned int lastSignalMax = 0;
unsigned int lastSignalMin = 0;
unsigned int lastPeakToPeak = 0;

// Raw (unclamped) dB from the last readDBLevel() call — lets you see real
// gradations below the 30.0 floor for calibration. The clamped/floored
// value is still what's used for whistle detection and cloud publishing.
float lastRawDB = 30.0;
bool  lastReadingValid = true;

// A genuinely disconnected/floating mic tends to read a FLAT, unchanging
// value (near-zero peakToPeak) persistently over many samples in a row —
// unlike a loud transient (snap/whistle), which has large peakToPeak and
// only lasts briefly. We only flag a fault after several consecutive
// flatlined samples, so a real loud sound is never mistaken for a fault.
#define FLATLINE_P2P_MAX     3      // peakToPeak this low counts as "flat"
#define FLATLINE_FAULT_COUNT 15     // consecutive flat samples (~300ms) before flagging fault
unsigned int flatlineStreak = 0;

// ── Read instantaneous envelope dB over SAMPLE_WINDOW ───────────────────
float readDBLevel() {
  unsigned long startMillis = millis();
  unsigned int signalMax = 0;
  unsigned int signalMin = 4095;

  while (millis() - startMillis < SAMPLE_WINDOW) {
    unsigned int sample = analogRead(AUDIO_PIN);
    if (sample < 4095) {
      if (sample > signalMax) signalMax = sample;
      if (sample < signalMin) signalMin = sample;
    }
  }

  unsigned int peakToPeak = signalMax - signalMin;
  lastSignalMax = signalMax;
  lastSignalMin = signalMin;
  lastPeakToPeak = peakToPeak;

  // Fault detection: only a SUSTAINED flatline counts as disconnected.
  // A loud transient (snap/whistle) has large peakToPeak and resets the
  // streak immediately, so it's never mistaken for a fault.
  if (peakToPeak <= FLATLINE_P2P_MAX) {
    if (flatlineStreak < 65000) flatlineStreak++;
  } else {
    flatlineStreak = 0;
  }
  lastReadingValid = (flatlineStreak < FLATLINE_FAULT_COUNT);

  float voltage = (peakToPeak * V_REF) / ADC_RESOLUTION;
  if (voltage < 0.01) voltage = 0.01;

  float rawDb = (41.52 * log10(voltage)) + 64.02;
  lastRawDB = rawDb;

  float db = rawDb;
  if (db < 30.0) db = 30.0;

  // Invalid (persistently flatlined/disconnected) readings never count as
  // loud — report them as silence rather than letting a stuck pin trigger
  // a whistle. A real loud sound always has nonzero peakToPeak, so it is
  // never affected by this.
  if (!lastReadingValid) db = 30.0;

  return db;
}

// ── WhatsApp alert ───────────────────────────────────────────────────────
void sendWhatsAppAlert(int completedWhistles) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);

  Serial.println("Attempting WhatsApp alert connection...");

  if (!client.connect(host, 443)) {
    Serial.println("WhatsApp API Connection FAILED - could not reach host");
    return;
  }

  Serial.println("Connected to CircuitDigest Cloud host. Sending request...");

  // NOTE: "threshold_violation_alert" is reused here since it's a generic
  // template with device/parameter/measured_value/limit/location fields.
  // If your CircuitDigest Cloud account has a dedicated whistle-alert
  // template, swap template_id below to that instead.
  String payload =
    "{\"phone_number\":\"" + String(PHONE_NUMBER) + "\","
    "\"template_id\":\"threshold_violation_alert\","
    "\"variables\":{"
    "\"device_name\":\"ESP32 Cooker Whistle Counter\","
    "\"parameter\":\"Whistle Count\","
    "\"measured_value\":\"" + String(completedWhistles) + " whistles\","
    "\"limit\":\"" + String(completedWhistles) + " whistles\","
    "\"location\":\"Kitchen\"}}";

  client.println("POST /api/v1/whatsapp/send HTTP/1.1");
  client.println("Host: www.circuitdigest.cloud");
  client.println("X-API-Key: " + String(API_KEY));
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.print("Content-Length: ");
  client.println(payload.length());
  client.println();
  client.print(payload);

  unsigned long responseStart = millis();
  while (client.connected() && !client.available()) {
    if (millis() - responseStart > 5000) {
      Serial.println("WhatsApp API response TIMEOUT - no reply from server");
      client.stop();
      return;
    }
    delay(10);
  }

  Serial.println("---- Server Response ----");
  String responseBody = "";
  while (client.connected() || client.available()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      Serial.println(line);
      responseBody += line;
    }
  }
  Serial.println("--------------------------");

  if (responseBody.indexOf("200") > 0 || responseBody.indexOf("201") > 0) {
    Serial.println("WhatsApp Alert appears to have SENT successfully.");
  } else {
    Serial.println("WhatsApp Alert may have FAILED - check response above for error details.");
  }

  client.stop();
}

// Publishes a value, checking the actual success/failure return instead of
// firing-and-forgetting. Retries a few times with the loop pumped in
// between, since a publish can fail if the cloud session isn't fully
// settled yet (e.g. right after boot).
void publishWithRetry(const char* key, float value) {
  for (int attempt = 1; attempt <= 5; attempt++) {
    bool ok = CDcloud.publish(key, value);
    Serial.print("Publish ");
    Serial.print(key);
    Serial.print(" = ");
    Serial.print(value);
    Serial.print(" (attempt ");
    Serial.print(attempt);
    Serial.print("): ");
    Serial.println(ok ? "OK" : "FAILED");

    if (ok) return;

    CDcloud.loop();
    delay(300);
  }
  Serial.print("Giving up publishing ");
  Serial.println(key);
}

// ── Cloud → device callbacks ─────────────────────────────────────────────
void onTargetSlider(float v) {
  targetWhistles = (int)v;
  if (targetWhistles < 1) targetWhistles = 1;
  Serial.print("Target whistle count set to: ");
  Serial.println(targetWhistles);
}

void onListenSwitch(float v) {
  bool turningOn = (bool)v;

  if (turningOn && !isListening) {
    // Arm fresh: reset count, clear any in-progress whistle state
    isListening = true;
    whistleCount = 0;
    inWhistle = false;
    cooldownUntilMs = 0;
    CDcloud.publish(KEY_LIVE_COUNT, 0.0f);
    Serial.println("Listening STARTED - whistle count reset to 0");
  } else if (!turningOn && isListening) {
    isListening = false;
    Serial.println("Listening STOPPED by user");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(AUDIO_PIN, INPUT);
  analogSetAttenuation(ADC_11db);

  // ---- Force a clean slate on every power-up/reset ----
  isListening    = false;
  whistleCount   = 0;
  targetWhistles = 0;
  inWhistle      = false;
  cooldownUntilMs = 0;

  CDcloud.subscribe(KEY_TARGET_SET, onTargetSlider);
  CDcloud.subscribe(KEY_LISTEN_SW, onListenSwitch);

  Serial.println("Connecting to Wi-Fi & CircuitDigest Cloud...");
  if (!CDcloud.begin(WIFI_SSID, WIFI_PASS, DEVICE_ID, CONNECTION_KEY, API_KEY)) {
    Serial.println("CDcloud begin() failed — check credentials. Rebooting...");
    delay(2000);
    ESP.restart();
  } else {
    Serial.println("CDcloud initialized successfully.");
  }

  // CDcloud.begin() succeeding only means WiFi + the initial handshake
  // worked — the cloud session isn't necessarily ready to accept publishes
  // the instant it returns. Pump the loop for a bit so it fully settles
  // before we push the reset values, or they can silently get dropped.
  for (int i = 0; i < 15; i++) {
    CDcloud.loop();
    delay(200);
  }

  // Push the reset state to the dashboard so it never shows a stale value
  // from before this boot (switch OFF, count/db back to zero). Each
  // publish is retried a few times since a single attempt can still land
  // before the connection is fully ready.
  publishWithRetry(KEY_LISTEN_SW, 0.0f);
  publishWithRetry(KEY_LIVE_COUNT, 0.0f);
  publishWithRetry(KEY_LIVE_DB, 0.0f);

  Serial.println("System Ready. Set target on analog-input-1, arm with analog-input-2.");
}

void loop() {
  CDcloud.loop();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi Disconnected! Restarting system...");
    delay(2000);
    ESP.restart();
  }

  // Always sample the mic (even when not armed) so you can watch live dB
  // in Serial Monitor and tune WHISTLE_ON_DB / WHISTLE_OFF_DB before relying
  // on the automatic detection.
  float db = readDBLevel();

  static unsigned long lastDbPrint = 0;
  unsigned long nowDb = millis();
  if (nowDb - lastDbPrint >= 200) {
    lastDbPrint = nowDb;
    Serial.print("Live dB: ");
    Serial.print(db, 1);
    Serial.print(" (raw: ");
    Serial.print(lastRawDB, 1);
    Serial.print(")  | ADC min: ");
    Serial.print(lastSignalMin);
    Serial.print(" max: ");
    Serial.print(lastSignalMax);
    Serial.print(" p2p: ");
    Serial.print(lastPeakToPeak);
    Serial.print(" | mic: ");
    Serial.println(lastReadingValid ? "OK" : "DISCONNECTED?");
  }

  if (isListening) {
    unsigned long now = millis();

    if (!inWhistle) {
      // Waiting for a whistle to start (respect cooldown after last one)
      if (db > WHISTLE_ON_DB && now > cooldownUntilMs) {
        inWhistle = true;
        whistleStartMs = now;
      }
    } else {
      // Currently inside a whistle burst — has it ended?
      if (db < WHISTLE_OFF_DB) {
        unsigned long whistleDuration = now - whistleStartMs;
        inWhistle = false;

        if (whistleDuration >= MIN_WHISTLE_MS) {
          whistleCount++;
          cooldownUntilMs = now + WHISTLE_COOLDOWN_MS;

          Serial.print("Whistle detected! Count: ");
          Serial.print(whistleCount);
          Serial.print(" / ");
          Serial.println(targetWhistles);

          CDcloud.publish(KEY_LIVE_COUNT, (float)whistleCount);

          if (whistleCount >= targetWhistles) {
            Serial.println("Target whistle count reached! Sending WhatsApp alert...");
            sendWhatsAppAlert(whistleCount);

            // Auto-disarm so it doesn't keep counting/alerting
            isListening = false;
            CDcloud.publish(KEY_LISTEN_SW, 0.0f); // sync dashboard switch back to OFF
          }
        }
        // else: too short, was just noise — ignored, no count
      }
    }
  }

  // ---- Publish live dB to the dashboard every second ----
  unsigned long nowPub = millis();
  if (nowPub - lastCloudPublish >= CLOUD_PUBLISH_MS) {
    lastCloudPublish = nowPub;
    CDcloud.publish(KEY_LIVE_DB, db);
  }

  // ---- Periodic serial debug ----
  static unsigned long lastDebug = 0;
  unsigned long now2 = millis();
  if (now2 - lastDebug >= 500) {
    lastDebug = now2;
    Serial.print("Listening: ");
    Serial.print(isListening ? "YES" : "NO");
    Serial.print(" | Count: ");
    Serial.print(whistleCount);
    Serial.print(" / ");
    Serial.println(targetWhistles);
  }
}
