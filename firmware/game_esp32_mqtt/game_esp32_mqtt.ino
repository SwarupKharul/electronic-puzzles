/*
 * =================================================================================
 * ESCAPE ROOM GAME PROP ESP32 FIRMWARE (Dual-Core FreeRTOS Template)
 * =================================================================================
 * Generic template for any single-sensor puzzle prop (reed switch, button, etc.)
 *
 * Features:
 *   - Dual-Core Isolation:
 *       • Core 1 (APP_CPU): 100% dedicated to sensor sampling, relay actuation,
 *                           hardware reset button, and LCD. Pure hardware determinism
 *                           with ZERO network code, delays, or blocking.
 *       • Core 0 (PRO_CPU): Dedicated background network task. Retries Wi-Fi, mDNS, and
 *                           MQTT indefinitely every 5s with zero impact on the game loop.
 *   - Thread-Safe Inter-Core Queues (FreeRTOS xQueue):
 *       • cmdQueue: Transfers incoming GM commands (START, RESET, STOP, SOLVE) to Core 1.
 *       • telemetryQueue: Posts outgoing state/event changes from Core 1 to Core 0.
 *   - Automatic power-on puzzle start (works 100% offline even if Wi-Fi/Broker is dead).
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic.
 *   - I2C 16x2 LCD Display with I2C bus timeout protection.
 *   - Non-blocking hardware reset button debounce.
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT).
 *
 * ---------------------------------------------------------------------------------
 * WIRING GUIDE:
 * ---------------------------------------------------------------------------------
 *   Relay Module IN/SIG  ---> GPIO 26 (RELAY_PIN)
 *   Relay Module VCC     ---> VIN (5V)
 *   Relay Module GND     ---> GND
 *
 *   Puzzle Sensor        ---> GPIO 27 (SENSOR_PIN, active LOW, internal pullup)
 *   Reset Button         ---> GPIO 14 (RESET_BTN_PIN, active LOW, internal pullup)
 *
 *   I2C LCD SDA          ---> GPIO 21 (LCD_SDA_PIN)
 *   I2C LCD SCL          ---> GPIO 22 (LCD_SCL_PIN)
 *
 * NOTE: Adjust pin assignments below to avoid conflicts for your specific wiring.
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 1

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =================================================================================
// 1. HARDWARE CONFIGURATION & PIN DEFINITIONS
// =================================================================================

// I2C LCD (Address 0x27, 16 cols, 2 rows) — Comment out LCD lines if not using one
LiquidCrystal_I2C lcd(0x27, 16, 2);
const int LCD_SDA_PIN     = 21;  // Default I2C SDA
const int LCD_SCL_PIN     = 22;  // Default I2C SCL

// Sensors & Actuators
const int RELAY_PIN       = 26;  // Solenoid / Maglock Relay
const int SENSOR_PIN      = 27;  // Puzzle Sensor Input (Button / Reed Switch)
const int RESET_BTN_PIN   = 14;  // Physical Manual Reset Button (active LOW)

// Relay Trigger Logic (Most 3-pin relay modules are ACTIVE-LOW)
const bool RELAY_ACTIVE_LOW = true;

// Auto-relock pulse duration for Solenoid locks (0 = stay unlocked until RESET/START)
const unsigned long AUTO_RELOCK_DELAY_MS = 0;
unsigned long unlockedAt = 0;

// Set to true to start puzzle immediately on boot (no WiFi/laptop needed)
const bool AUTO_START_ON_BOOT = true;

// =================================================================================
// 2. NETWORK & mDNS / MQTT CONFIGURATION (ZERO-IP SETUP)
// =================================================================================
const char* WIFI_SSID     = "operations_404";
const char* WIFI_PASS     = "Mytplink2020";

// const char* WIFI_SSID     = "Airtel_anjo_4056";
// const char* WIFI_PASS     = "air38409";

// mDNS Configuration - The ESP32 discovers the server automatically!
const char* MDNS_HOST_ESCAPEROOM = "escaperoom"; // Will query 'escaperoom.local'
const char* MDNS_HOST_LAPTOP     = "pop-os";     // Native Linux hostname fallback
const char* MQTT_SERVER_FALLBACK = "192.168.1.9"; // Fallback only if router blocks multicast

const int   MQTT_PORT_DEFAULT    = 1884;          // Default port (auto-discovered via mDNS)
int         activeMqttPort       = MQTT_PORT_DEFAULT;
IPAddress   activeMqttIP;
bool        serverDiscovered     = false;

const char* GAME_ID       = "game1";          // Matches "id" in games.json
const char* ROOT_TOPIC    = "escaperoom";     // Matches "rootTopic" in games.json

// =================================================================================
// 3. FREE-RTOS DUAL-CORE INTER-THREAD COMMUNICATION & GLOBALS
// =================================================================================

struct CommandMsg {
  char cmd[16]; // e.g. "START", "RESTART", "STOP", "RESET", "SOLVE"
};

struct TelemetryMsg {
  char kind;     // 'S' = State, 'E' = Event
  char text[32]; // e.g. "STARTED", "COMPLETED", "FAILED"
  int attempt;
};

QueueHandle_t cmdQueue = NULL;
QueueHandle_t telemetryQueue = NULL;
TaskHandle_t networkTaskHandle = NULL;

WiFiClient espClient;
PubSubClient mqtt(espClient);

enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
volatile GameState currentState = READY;
volatile int attemptNumber = 0;

const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds

// Button & Sensor Debounce State (Core 1)
bool lastResetBtnState = HIGH;
unsigned long lastResetBtnTime = 0;

bool lastSensorState = HIGH;
unsigned long lastSensorTime = 0;

// MQTT Topics & Connection Tracking (Core 0)
char topicStatus[64];
char topicState[64];
char topicEvent[64];
char topicCmd[64];

unsigned long lastMqttRetry = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastMdnsRetry = 0;
bool wasWiFiConnected = false;

// Forward Declarations
void networkTask(void* pvParameters);
void setupWiFi();
void maintainWiFi();
bool discoverMQTTServer();
void maintainMQTT();
void postState(GameState state);
void postEvent(const char* eventName);
void publishState(GameState state);
void publishEvent(const char* eventName);
void handleCommand(String cmd);
void updateLcdDisplay();
void lockDoor();
void unlockDoor();
void checkPuzzleLogic();

// =================================================================================
// 4. RELAY & ACTUATOR CONTROL (Core 1)
// =================================================================================
void lockDoor() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  unlockedAt = 0;
  Serial.println("🔒 [RELAY] Door Locked (Relay DE-ENERGIZED)");
}

void unlockDoor() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
  unlockedAt = millis();
  Serial.println("🔓 [RELAY] Door Unlocked (Relay ENERGIZED)");
}

// =================================================================================
// 5. THREAD-SAFE STATE & EVENT POSTING (Core 1 -> Core 0)
// =================================================================================
void postState(GameState state) {
  const char* stateStr = "READY";
  switch (state) {
    case READY:     stateStr = "READY";     break;
    case STARTED:   stateStr = "STARTED";   break;
    case FAILED:    stateStr = "FAILED";    break;
    case COMPLETED: stateStr = "COMPLETED"; break;
    case STOPPED:   stateStr = "STOPPED";   break;
  }
  if (telemetryQueue != NULL) {
    TelemetryMsg msg;
    msg.kind = 'S';
    strncpy(msg.text, stateStr, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    msg.attempt = attemptNumber;
    xQueueSend(telemetryQueue, &msg, 0); // Non-blocking
  }
}

void postEvent(const char* eventName) {
  if (telemetryQueue != NULL) {
    TelemetryMsg msg;
    msg.kind = 'E';
    strncpy(msg.text, eventName, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    msg.attempt = attemptNumber;
    xQueueSend(telemetryQueue, &msg, 0); // Non-blocking
  }
}

// Transparent aliases for existing callers
void publishState(GameState state) {
  postState(state);
}

void publishEvent(const char* eventName) {
  postEvent(eventName);
}

// =================================================================================
// 6. COMMAND DISPATCHER (Core 1)
// =================================================================================
void handleCommand(String cmd) {
  if (cmd.equalsIgnoreCase("START") || cmd.equalsIgnoreCase("RESTART")) {
    attemptNumber++;
    currentState = STARTED;
    lockDoor();
    postState(STARTED);
    postEvent("STARTED");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("STOP")) {
    currentState = STOPPED;
    lockDoor();
    postState(STOPPED);
    postEvent("STOPPED");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("RESET")) {
    currentState = READY;
    attemptNumber = 0;
    lockDoor();
    postState(READY);
    postEvent("RESET");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
    Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
    currentState = COMPLETED;
    unlockDoor();
    postState(COMPLETED);
    postEvent("COMPLETED");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUZZLE SOLVED!");
    lcd.setCursor(0, 1);
    lcd.print("DOOR UNLOCKED");
  }
}

// =================================================================================
// 7. LCD DISPLAY HELPER (Core 1)
// =================================================================================
void updateLcdDisplay() {
  lcd.clear();
  switch (currentState) {
    case READY:
      lcd.setCursor(0, 0);
      lcd.print("PUZZLE PROP     ");
      lcd.setCursor(0, 1);
      lcd.print("READY TO PLAY   ");
      break;

    case STARTED:
      lcd.setCursor(0, 0);
      lcd.print("GAME ACTIVE     ");
      lcd.setCursor(0, 1);
      lcd.printf("ATTEMPT #%d      ", attemptNumber);
      break;

    case FAILED:
      lcd.setCursor(0, 0);
      lcd.print("WRONG ANSWER!   ");
      lcd.setCursor(0, 1);
      lcd.print("TRY AGAIN...    ");
      break;

    case COMPLETED:
      lcd.setCursor(0, 0);
      lcd.print("PUZZLE SOLVED!  ");
      lcd.setCursor(0, 1);
      lcd.print("DOOR UNLOCKED   ");
      break;

    case STOPPED:
      lcd.setCursor(0, 0);
      lcd.print("GAME PAUSED /   ");
      lcd.setCursor(0, 1);
      lcd.print("STOPPED BY GM   ");
      break;
  }
}

// =================================================================================
// 8. PUZZLE SENSOR LOGIC (Core 1 — Non-Blocking)
// =================================================================================
void checkPuzzleLogic() {
  if (currentState != STARTED && currentState != READY) return;

  // Non-blocking sensor read with software debounce
  bool currentSensorState = digitalRead(SENSOR_PIN);
  unsigned long now = millis();

  if (currentSensorState == LOW && lastSensorState == HIGH) {
    if (now - lastSensorTime > 250) { // 250ms debounce
      lastSensorTime = now;

      if (currentState == READY) {
        currentState = STARTED;
        attemptNumber++;
        postState(STARTED);
        postEvent("STARTED");
      }

      Serial.println("🎉 Puzzle Solved! Opening lock...");
      currentState = COMPLETED;
      unlockDoor();
      postState(COMPLETED);
      postEvent("COMPLETED");

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("PUZZLE SOLVED!");
      lcd.setCursor(0, 1);
      lcd.print("DOOR UNLOCKED");
    }
  }
  lastSensorState = currentSensorState;
}

// =================================================================================
// 9. SETUP (Core 1)
// =================================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==========================================");
  Serial.println("🚀 Escape Room Prop Initializing (Dual-Core)...");
  Serial.println("==========================================");

  // Initialize Relay (start locked) & Reset Button
  pinMode(RELAY_PIN, OUTPUT);
  lockDoor();

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  // Initialize I2C Bus & LCD with bus timeout protection against inductive noise
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setTimeOut(100);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PUZZLE PROP");
  lcd.setCursor(0, 1);
  lcd.print("BOOTING...");

  // Construct MQTT Topics
  snprintf(topicStatus, sizeof(topicStatus), "%s/%s/status", ROOT_TOPIC, GAME_ID);
  snprintf(topicState,  sizeof(topicState),  "%s/%s/state",  ROOT_TOPIC, GAME_ID);
  snprintf(topicEvent,  sizeof(topicEvent),  "%s/%s/event",  ROOT_TOPIC, GAME_ID);
  snprintf(topicCmd,    sizeof(topicCmd),    "%s/%s/cmd",    ROOT_TOPIC, GAME_ID);

  // Create FreeRTOS Queues for Thread-Safe Inter-Core Communication
  cmdQueue = xQueueCreate(10, sizeof(CommandMsg));
  telemetryQueue = xQueueCreate(16, sizeof(TelemetryMsg));

  // Spawn Independent Background Network & MQTT Task on CPU Core 0
  xTaskCreatePinnedToCore(
    networkTask,
    "NetworkTask",
    8192,
    NULL,
    1, // Priority 1 (low, so Core 1 game loop is never preempted)
    &networkTaskHandle,
    0  // Core 0
  );

  if (AUTO_START_ON_BOOT) {
    attemptNumber = 1;
    currentState = STARTED;
    postState(STARTED);
    postEvent("STARTED");
  } else {
    currentState = READY;
    postState(READY);
    postEvent("READY");
  }

  updateLcdDisplay();
}

// =================================================================================
// 10. MAIN LOOP (Core 1 — 100% Non-Blocking & Pure Sensor Sampling)
// =================================================================================
void loop() {
  unsigned long now = millis();

  // 1. Process Incoming Commands from Core 0 (Zero-latency queue drain)
  if (cmdQueue != NULL) {
    CommandMsg incoming;
    while (xQueueReceive(cmdQueue, &incoming, 0) == pdTRUE) {
      handleCommand(String(incoming.cmd));
    }
  }

  // 2. Non-Blocking Physical Manual Reset Button check
  bool currentBtnState = digitalRead(RESET_BTN_PIN);
  if (currentBtnState == LOW && lastResetBtnState == HIGH) {
    if (now - lastResetBtnTime > 250) { // 250ms debounce
      lastResetBtnTime = now;
      Serial.println("🔘 Hardware Reset Pressed");
      handleCommand("RESET");
    }
  }
  lastResetBtnState = currentBtnState;

  // 3. PRIORITY #1: Handle Puzzle Sensor Logic (Zero Latency)
  checkPuzzleLogic();

  // 4. Non-blocking Auto-Relock Timer check (for Solenoid locks)
  if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
    if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
      Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
      lockDoor();
    }
  }
}

// =================================================================================
// 11. BACKGROUND NETWORK & MQTT TASK (Core 0 — Independent Thread)
// =================================================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[16];
  unsigned int len = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
  memcpy(message, payload, len);
  message[len] = '\0';

  for (int i = (int)len - 1; i >= 0 && (message[i] == ' ' || message[i] == '\r' || message[i] == '\n'); i--) {
    message[i] = '\0';
  }

  Serial.printf("📩 [MQTT Core 0] Command Received: %s\n", message);

  if (cmdQueue != NULL) {
    CommandMsg msg;
    strncpy(msg.cmd, message, sizeof(msg.cmd) - 1);
    msg.cmd[sizeof(msg.cmd) - 1] = '\0';
    xQueueSend(cmdQueue, &msg, 0);
  }
}

bool discoverMQTTServer() {
  Serial.println("\n🔍 [mDNS Core 0] Discovering Escape Room Control Server...");

  if (!MDNS.begin("ESP32-Prop")) {
    Serial.println("⚠️ [mDNS Core 0] Responder init failed, querying...");
  } else {
    Serial.println("📡 [mDNS Core 0] Responder active ('ESP32-Prop.local')");
  }

  // 1. Try DNS-SD Service Discovery
  int n = MDNS.queryService("mqtt", "tcp");
  if (n > 0) {
    activeMqttIP = MDNS.address(0);
    activeMqttPort = MDNS.port(0);
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS Core 0] Discovered via DNS-SD: %s:%d\n", activeMqttIP.toString().c_str(), activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 2. Try resolving 'escaperoom.local'
  activeMqttIP = MDNS.queryHost(MDNS_HOST_ESCAPEROOM);
  if (activeMqttIP != IPAddress(0, 0, 0, 0)) {
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS Core 0] Resolved '%s.local' -> %s:%d\n", MDNS_HOST_ESCAPEROOM, activeMqttIP.toString().c_str(), activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 3. Try resolving laptop OS hostname 'pop-os.local'
  activeMqttIP = MDNS.queryHost(MDNS_HOST_LAPTOP);
  if (activeMqttIP != IPAddress(0, 0, 0, 0)) {
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS Core 0] Resolved '%s.local' -> %s:%d\n", MDNS_HOST_LAPTOP, activeMqttIP.toString().c_str(), activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 4. Fallback to hardcoded IP
  Serial.printf("  ⚠️ [mDNS Core 0] Using fallback IP: %s:%d\n", MQTT_SERVER_FALLBACK, activeMqttPort);
  activeMqttIP.fromString(MQTT_SERVER_FALLBACK);
  mqtt.setServer(activeMqttIP, activeMqttPort);
  return false;
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("📶 [Core 0] Connecting to Wi-Fi");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    vTaskDelay(pdMS_TO_TICKS(250));
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wasWiFiConnected = true;
    Serial.println("\n✅ [Core 0] Wi-Fi Connected!");
    Serial.print("   Prop IP Address: ");
    Serial.println(WiFi.localIP());

    discoverMQTTServer();
  } else {
    Serial.println("\n⚠️ [Core 0] Initial Wi-Fi timeout. Retrying in background continuously...");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWiFiConnected) {
      wasWiFiConnected = true;
      Serial.println("\n✅ [Core 0] Wi-Fi Connected!");
      Serial.print("   Prop IP Address: ");
      Serial.println(WiFi.localIP());
      if (!serverDiscovered) {
        discoverMQTTServer();
      }
    }
  } else {
    wasWiFiConnected = false;
    unsigned long now = millis();
    if (now - lastWiFiRetry > 10000) {
      lastWiFiRetry = now;
      Serial.println("📶 [Core 0] Wi-Fi reconnecting in background...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  // If server has not yet been discovered via mDNS, retry discovery periodically
  if (!serverDiscovered) {
    unsigned long now = millis();
    if (now - lastMdnsRetry > 10000) {
      lastMdnsRetry = now;
      discoverMQTTServer();
    }
  }

  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttRetry > 5000) { // Retries indefinitely every 5s on Core 0!
    lastMqttRetry = now;
    Serial.printf("🔌 [Core 0] Connecting to MQTT Broker at %s:%d...\n",
                  activeMqttIP.toString().c_str(), activeMqttPort);

    String clientId = "ESP32-Prop-" + String(GAME_ID);

    if (mqtt.connect(clientId.c_str(), topicStatus, 1, true, "offline")) {
      Serial.println("✅ [Core 0] Connected to MQTT Broker!");
      mqtt.publish(topicStatus, "online", true);
      mqtt.subscribe(topicCmd);

      // Publish current live state immediately upon reconnect
      const char* stateStr = "READY";
      switch (currentState) {
        case READY:     stateStr = "READY";     break;
        case STARTED:   stateStr = "STARTED";   break;
        case FAILED:    stateStr = "FAILED";    break;
        case COMPLETED: stateStr = "COMPLETED"; break;
        case STOPPED:   stateStr = "STOPPED";   break;
      }
      char buf[128];
      snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"attempt\":%d}", stateStr, attemptNumber);
      mqtt.publish(topicState, buf, true);
    } else {
      Serial.printf("⚠️ [Core 0] MQTT Failed (rc=%d). Retrying in 5 seconds in background...\n", mqtt.state());
    }
  }
}

void networkTask(void* pvParameters) {
  Serial.printf("🌐 [Core 0] Background Network Task running on Core %d\n", xPortGetCoreID());

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(1);
  mqtt.setCallback(mqttCallback);

  setupWiFi();

  unsigned long lastHb = 0;

  while (true) {
    maintainWiFi();
    maintainMQTT();

    if (mqtt.connected()) {
      mqtt.loop();

      // Drain all outgoing telemetry from Core 1
      if (telemetryQueue != NULL) {
        TelemetryMsg msg;
        while (xQueueReceive(telemetryQueue, &msg, 0) == pdTRUE) {
          if (msg.kind == 'S') {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"attempt\":%d}", msg.text, msg.attempt);
            mqtt.publish(topicState, buf, true);
          } else if (msg.kind == 'E') {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"event\":\"%s\",\"attempt\":%d}", msg.text, msg.attempt);
            mqtt.publish(topicEvent, buf, false);
          }
        }
      }

      // Periodic Heartbeat every 3 seconds
      unsigned long now = millis();
      if (now - lastHb > HEARTBEAT_INTERVAL) {
        lastHb = now;
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"event\":\"HEARTBEAT\",\"attempt\":%d}", attemptNumber);
        mqtt.publish(topicEvent, buf, false);
      }
    } else {
      // Offline: drain queue so stale messages don't accumulate while disconnected
      if (telemetryQueue != NULL) {
        TelemetryMsg discard;
        while (xQueueReceive(telemetryQueue, &discard, 0) == pdTRUE);
      }
    }

    // Crucial: yield 15ms to FreeRTOS scheduler on Core 0
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
