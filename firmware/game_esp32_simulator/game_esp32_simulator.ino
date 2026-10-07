/*
 * =================================================================================
 * DUMMY ESP32 GAME SIMULATOR FIRMWARE (Zero-Dependency JSON version)
 * =================================================================================
 * Requires ONLY PubSubClient library by Nick O'Leary!
 * No ArduinoJson required!
 * =================================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ---------------- CONFIGURATION ----------------
const char* WIFI_SSID     = "Swarup's S23";
const char* WIFI_PASS     = "google14";

const char* MQTT_SERVER   = "10.141.86.197"; // Replace with your laptop IP
const int   MQTT_PORT     = 1884;             // Port displayed by server (1884 or 1883)

const char* GAME_ID       = "game1";          // "game1" or "game2"
const char* ROOT_TOPIC    = "escaperoom";

// Global Objects
WiFiClient espClient;
PubSubClient mqtt(espClient);

char topicStatus[64];
char topicState[64];
char topicEvent[64];
char topicCmd[64];

enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
GameState currentState = READY;
int attemptCount = 0;
unsigned long lastAutoStep = 0;
const unsigned long AUTO_INTERVAL = 8000; // 8 seconds per automated state step
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds heartbeat

void setupWiFi();
void reconnectMQTT();
void publishState(GameState st);
void publishEvent(const char* evt);

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.printf("📩 Received Command: %s\n", message.c_str());

  if (message.equalsIgnoreCase("START")) {
    attemptCount++;
    currentState = STARTED;
    publishState(STARTED);
    publishEvent("STARTED");
  } else if (message.equalsIgnoreCase("STOP")) {
    currentState = STOPPED;
    publishState(STOPPED);
    publishEvent("STOPPED");
  } else if (message.equalsIgnoreCase("RESET")) {
    currentState = READY;
    publishState(READY);
    publishEvent("RESET");
  } else if (message.equalsIgnoreCase("SOLVE") || message.equalsIgnoreCase("OVERRIDE")) {
    currentState = COMPLETED;
    publishState(COMPLETED);
    publishEvent("COMPLETED");
  }
  lastAutoStep = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n🤖 ESP32 Auto-Test Simulator Starting...");

  snprintf(topicStatus, sizeof(topicStatus), "%s/%s/status", ROOT_TOPIC, GAME_ID);
  snprintf(topicState,  sizeof(topicState),  "%s/%s/state",  ROOT_TOPIC, GAME_ID);
  snprintf(topicEvent,  sizeof(topicEvent),  "%s/%s/event",  ROOT_TOPIC, GAME_ID);
  snprintf(topicCmd,    sizeof(topicCmd),    "%s/%s/cmd",    ROOT_TOPIC, GAME_ID);

  setupWiFi();

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(15);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  lastAutoStep = millis();
}

void loop() {
  // Non-blocking MQTT maintenance
  maintainMQTT();

  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    publishEvent("HEARTBEAT");
  }

  // Automatic state machine step for testing dashboard & audio playback
  if (millis() - lastAutoStep > AUTO_INTERVAL) {
    lastAutoStep = millis();
    runAutoTestSequence();
  }
}

void runAutoTestSequence() {
  switch (currentState) {
    case READY:
      attemptCount++;
      currentState = STARTED;
      Serial.println("▶ Simulator Event: STARTED");
      publishState(STARTED);
      publishEvent("STARTED");
      break;

    case STARTED:
      if (attemptCount % 2 == 1) {
        currentState = FAILED;
        Serial.println("❌ Simulator Event: FAILED");
        publishState(FAILED);
        publishEvent("FAILED");
      } else {
        currentState = COMPLETED;
        Serial.println("🎉 Simulator Event: COMPLETED");
        publishState(COMPLETED);
        publishEvent("COMPLETED");
      }
      break;

    case FAILED:
      attemptCount++;
      currentState = STARTED;
      Serial.println("↻ Simulator Event: RESTART / STARTED");
      publishState(STARTED);
      publishEvent("STARTED");
      break;

    case COMPLETED:
    case STOPPED:
      currentState = READY;
      Serial.println("⟲ Simulator Event: RESET to READY");
      publishState(READY);
      publishEvent("RESET");
      break;
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi...");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(300);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n⚠️ Wi-Fi Timeout. Simulator will retry in background.");
  }
}

unsigned long lastWiFiRetry = 0;
unsigned long lastMqttRetry = 0;
int mqttFailCount = 0;

void maintainMQTT() {
  // Non-blocking WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiRetry > 10000) {
      lastWiFiRetry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttRetry > 5000) {
    lastMqttRetry = now;
    String clientId = "SimESP32-" + String(GAME_ID);
    if (mqtt.connect(clientId.c_str(), topicStatus, 1, true, "offline")) {
      mqtt.publish(topicStatus, "online", true);
      mqtt.subscribe(topicCmd);
      publishState(currentState);
      Serial.println("✅ Connected to MQTT Broker!");
      mqttFailCount = 0;
    } else {
      mqttFailCount++;
      if (mqttFailCount >= 3) {
        mqttFailCount = 0;
      }
    }
  }
}

void publishState(GameState st) {
  const char* stateStr = "READY";
  switch (st) {
    case READY: stateStr = "READY"; break;
    case STARTED: stateStr = "STARTED"; break;
    case FAILED: stateStr = "FAILED"; break;
    case COMPLETED: stateStr = "COMPLETED"; break;
    case STOPPED: stateStr = "STOPPED"; break;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"attempt\":%d}", stateStr, attemptCount);
  mqtt.publish(topicState, buf, true);
}

void publishEvent(const char* evt) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"event\":\"%s\",\"attempt\":%d}", evt, attemptCount);
  mqtt.publish(topicEvent, buf, false);
}