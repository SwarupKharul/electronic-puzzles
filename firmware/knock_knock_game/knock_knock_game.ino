/*
 * =================================================================================
 * ESCAPE ROOM - KNOCK KNOCK RHYTHM PUZZLE PROP (Dual-Core FreeRTOS Architecture)
 * =================================================================================
 * Features:
 *   - Dual-Core Isolation:
 *       • Core 1 (APP_CPU): 100% dedicated to Piezo ADC knock sensing & rhythm analysis.
 *                           Runs at 10,000+ Hz with ZERO network code, delays, or blocking.
 *       • Core 0 (PRO_CPU): Dedicated background network task. Retries Wi-Fi, mDNS, and
 *                           MQTT indefinitely every 5s with zero impact on the game loop.
 *   - Thread-Safe Inter-Core Queues (FreeRTOS xQueue):
 *       • cmdQueue: Transfers incoming GM commands (START, RESET, STOP, SOLVE) to Core 1.
 *       • telemetryQueue: Posts outgoing state/event changes from Core 1 to Core 0.
 *   - Automatic power-on puzzle start (works 100% offline even if Wi-Fi/Broker is dead).
 *   - Piezo Knock Sensor with dynamic debouncing & timing tolerance verification.
 *   - 3-Pin Relay Module actuation (Solenoid / Maglock) on puzzle completion.
 *   - I2C 16x2 LCD Display with I2C bus timeout protection against noise lockups.
 *
 * ---------------------------------------------------------------------------------
 * 3-PIN RELAY MODULE WIRING GUIDE:
 * ---------------------------------------------------------------------------------
 * 1. Logic Side (ESP32 -> 3-Pin Relay Module):
 *      [Relay Module]          [ESP32]
 *      VCC / +          --->   VIN (5V from USB) or external 5V
 *      GND / -          --->   GND (Common Ground)
 *      IN / SIG / S     --->   GPIO 25 (RELAY_PIN)
 *
 * 2. High-Power Lock Side (Screw Terminals: COM, NO, NC):
 *    Option A: 12V Solenoid Cabinet Lock (Fail-Secure - Locked when unpowered)
 *      12V Power Supply (+)   --->   Relay COM
 *      Relay NO (Normally Open) ---> Solenoid Lock (+)
 *      12V Power Supply (-)   --->   Solenoid Lock (-)
 *
 *    Option B: 12V Electromagnetic Lock / Maglock (Fail-Safe - Locked when powered)
 *      12V Power Supply (+)   --->   Relay COM
 *      Relay NC (Normally Closed) -> Maglock (+)
 *      12V Power Supply (-)   --->   Maglock (-)
 *
 * 3. SENSORS & BUTTONS:
 *      Piezo Sensor (+) --->   GPIO 34 (Analog In ADC1, 1M pull-down resistor to GND)
 *      Piezo Sensor (-) --->   GND
 *      Reset Button     --->   GPIO 14 (Active LOW to GND, internal pullup)
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 1

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =================================================================================
// 1. HARDWARE CONFIGURATION & PIN DEFINITIONS
// =================================================================================

// I2C LCD (Address 0x27, 16 cols, 2 rows)
LiquidCrystal_I2C lcd(0x27, 16, 2);
const int I2C_SDA_PIN     = 26;
const int I2C_SCL_PIN     = 27;

// Sensors & Actuators
const int PIEZO_PIN       = 34; // Analog input for Piezo knock sensor (ADC1)
const int RELAY_PIN       = 25; // 3-Pin Relay Signal Pin (IN / SIG / S)
const int RESET_BTN_PIN   = 14; // Physical manual reset button (active LOW)

// 3-Pin Relay Trigger Logic (Most standard 3-pin relay modules are ACTIVE-LOW)
const bool RELAY_ACTIVE_LOW = true;

// Auto-relock pulse duration for Solenoid locks (0 = stay unlocked until RESET/START)
const unsigned long AUTO_RELOCK_DELAY_MS = 0;
unsigned long unlockedAt = 0;

// Set to true to start listening immediately on boot
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

const int   MQTT_PORT_DEFAULT    = 1884;         // Default port (auto-discovered via mDNS)
int         activeMqttPort       = MQTT_PORT_DEFAULT;
IPAddress   activeMqttIP;
bool        serverDiscovered     = false;

const char* GAME_ID       = "game1";          // Matches "id" in games.json ("game1")
const char* ROOT_TOPIC    = "escaperoom";     // Matches "rootTopic" in games.json

// =================================================================================
// 3. PUZZLE PATTERN & TIMING SETTINGS
// =================================================================================
const int THRESHOLD        = 400;  // Piezo analog threshold (ESP32 ADC 0-4095; adjust after testing with recorder)
const int DEBOUNCE_TIME    = 10;   // Debounce to ignore piezo sensor ringing/echo (ms) — MUST match recorder
const int PATTERN_TIMEOUT  = 3000; // Silence period (> 3000ms) to signal end of knocking (ms)

// Distinct Timing Tolerances:
// Small Gap: 240 - 1300 ms (for quick consecutive knocks "-")
// Big Gap  : 1300 - 2800 ms (for phrase pauses "_")
const int SMALL_GAP_MIN    = 240;
const int SMALL_GAP_MAX    = 1300;
const int BIG_GAP_MIN      = 1300;
const int BIG_GAP_MAX      = 2800;

struct IntervalTolerance {
  int minMs;
  int maxMs;
  const char* typeName;
};

// Rhythm Pattern: knock-knock _ knock-knock-knock-knock _ knock
// 7 Knocks total -> 6 intervals
const IntervalTolerance expectedPattern[] = {
  {SMALL_GAP_MIN, SMALL_GAP_MAX, "Small Gap (-)"}, // Knock 1 -> Knock 2
  {BIG_GAP_MIN,   BIG_GAP_MAX,   "Big Gap (_)"},   // Knock 2 -> Knock 3
  {SMALL_GAP_MIN, SMALL_GAP_MAX, "Small Gap (-)"}, // Knock 3 -> Knock 4
  {SMALL_GAP_MIN, SMALL_GAP_MAX, "Small Gap (-)"}, // Knock 4 -> Knock 5
  {SMALL_GAP_MIN, SMALL_GAP_MAX, "Small Gap (-)"}, // Knock 5 -> Knock 6
  {BIG_GAP_MIN,   BIG_GAP_MAX,   "Big Gap (_)"}    // Knock 6 -> Knock 7
};
const int PATTERN_SIZE = sizeof(expectedPattern) / sizeof(expectedPattern[0]); // 6 intervals (7 knocks total)

// Knock State Variables (Exclusive to Core 1)
const int MAX_KNOCKS_BUFFER = 32;
unsigned long knockTimes[MAX_KNOCKS_BUFFER];
int knockCount = 0;
unsigned long lastKnock = 0;
bool waitingForPattern = false;

// =================================================================================
// 4. FREE-RTOS DUAL-CORE INTER-THREAD COMMUNICATION
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

// Game State (Core 1)
enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
volatile GameState currentState = READY;
volatile int attemptNumber = 0;
unsigned long stateTransitionTime = 0;

// Button Debounce State (Core 1)
bool lastButtonState = HIGH;
unsigned long lastButtonPressTime = 0;

// MQTT Objects & Topics (Core 0)
WiFiClient espClient;
PubSubClient mqtt(espClient);

char topicStatus[64];
char topicState[64];
char topicEvent[64];
char topicCmd[64];

unsigned long lastMqttRetry = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastMdnsRetry = 0;
bool wasWiFiConnected = false;
const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds

// Forward Declarations
void networkTask(void* pvParameters);
void setupWiFi();
void maintainWiFi();
bool discoverMQTTServer();
void maintainMQTT();
void handleCommand(String cmd);
void postState(GameState state);
void postEvent(const char* eventName);
void updateLcdDisplay();
void checkKnockInput();
void evaluatePattern();
void resetKnockState();
void lockDoor();
void unlockDoor();
void handleSuccess();
void handleFailure();

// =================================================================================
// 5. RELAY & ACTUATOR CONTROL (Core 1)
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
// 6. SUCCESS & FAILURE HANDLERS (Core 1)
// =================================================================================
void handleSuccess() {
  currentState = COMPLETED;
  resetKnockState();
  unlockDoor(); // Energize relay

  postState(COMPLETED);
  postEvent("COMPLETED");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PUZZLE SOLVED!");
  lcd.setCursor(0, 1);
  lcd.print("DOOR UNLOCKED");
}

void handleFailure() {
  currentState = FAILED;
  resetKnockState();
  postState(FAILED);
  postEvent("FAILED");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WRONG RHYTHM!");
  lcd.setCursor(0, 1);
  lcd.print("TRY AGAIN...");

  // Non-blocking 1.8s timer to return to STARTED
  stateTransitionTime = millis() + 1800;
}

// =================================================================================
// 7. THREAD-SAFE STATE & EVENT POSTING (Core 1 -> Core 0)
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

void handleCommand(String cmd) {
  if (cmd.equalsIgnoreCase("START") || cmd.equalsIgnoreCase("RESTART")) {
    attemptNumber++;
    currentState = STARTED;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    postState(STARTED);
    postEvent("STARTED");
    updateLcdDisplay();
  } 
  else if (cmd.equalsIgnoreCase("STOP")) {
    currentState = STOPPED;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    postState(STOPPED);
    postEvent("STOPPED");
    updateLcdDisplay();
  } 
  else if (cmd.equalsIgnoreCase("RESET")) {
    currentState = READY;
    attemptNumber = 0;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    postState(READY);
    postEvent("RESET");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
    Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
    handleSuccess();
  }
}

// =================================================================================
// 8. SETUP (Core 1)
// =================================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=======================================================");
  Serial.println("🚪 ESCAPE ROOM: KNOCK KNOCK PUZZLE (Dual-Core Engine)");
  Serial.println("=======================================================");

  // Initialize Relay (start locked) & Reset Button
  pinMode(RELAY_PIN, OUTPUT);
  lockDoor();

  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  // Initialize I2C Bus & LCD with bus timeout protection against inductive noise
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeOut(100);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("KNOCK PUZZLE");
  lcd.setCursor(0, 1);
  lcd.print("READY TO PLAY");

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
    1, // Low priority so Core 1 game loop is never preempted
    &networkTaskHandle,
    0  // Core 0
  );

  // Initialize game state on Core 1
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
// 9. MAIN GAME LOOP (Core 1 — 100% Non-Blocking & Pure Sensor Sampling)
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

  // 2. Physical Manual Reset Button check (Debounced)
  bool currentBtnState = digitalRead(RESET_BTN_PIN);
  if (currentBtnState == LOW && lastButtonState == HIGH) {
    if (now - lastButtonPressTime > 250) {
      lastButtonPressTime = now;
      Serial.println("🔘 Hardware Reset Pressed");
      handleCommand("RESET");
    }
  }
  lastButtonState = currentBtnState;

  // 3. Non-Blocking State Transition Check (for FAILED cooldown)
  if (stateTransitionTime > 0 && now >= stateTransitionTime) {
    stateTransitionTime = 0;
    if (currentState == FAILED) {
      resetKnockState();
      currentState = STARTED;
      postState(STARTED);
      updateLcdDisplay();
    }
  }

  // 4. PRIORITY #1: Continuous Knock Sensing & Pattern Logic
  if (currentState == STARTED || currentState == READY) {
    checkKnockInput();
  }

  // 5. Non-blocking Auto-Relock Timer check (for Solenoid locks)
  if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
    if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
      Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
      lockDoor();
    }
  }
}

// =================================================================================
// 10. KNOCK LOGIC & PATTERN EVALUATION (Core 1)
// =================================================================================
void resetKnockState() {
  knockCount = 0;
  waitingForPattern = false;
  lastKnock = 0;
}

void checkKnockInput() {
  int value = analogRead(PIEZO_PIN);
  unsigned long now = millis();

  // Detect Knock Pulse (debounce prevents piezo ringing from registering as multiple knocks)
  if (value > THRESHOLD && (now - lastKnock) > (unsigned long)DEBOUNCE_TIME) {
    lastKnock = now;

    if (currentState == READY) {
      currentState = STARTED;
      attemptNumber++;
      postState(STARTED);
      postEvent("STARTED");
    }

    if (knockCount < MAX_KNOCKS_BUFFER) {
      knockTimes[knockCount] = now;
    }
    knockCount++;

    waitingForPattern = true;

    Serial.printf("🥁 Knock %d recorded | Analog Value: %d\n", knockCount, value);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Listening...");
    lcd.setCursor(0, 1);
    if (knockCount <= PATTERN_SIZE + 1) {
      lcd.printf("Knock %d / %d", knockCount, PATTERN_SIZE + 1);
    } else {
      lcd.print("Extra knock!    ");
    }
  }

  // Check if pattern input has completed (silence timeout reached)
  if (waitingForPattern && (now - lastKnock) > PATTERN_TIMEOUT) {
    evaluatePattern();
  }
}

void evaluatePattern() {
  Serial.println("\n🔍 Evaluating Knock Pattern...");
  waitingForPattern = false;

  // 1. Verify Knock Count
  if (knockCount != PATTERN_SIZE + 1) {
    Serial.printf("❌ Wrong knock count: received %d, expected %d\n", knockCount, PATTERN_SIZE + 1);
    handleFailure();
    return;
  }

  // 2. Verify Timing Intervals Against Specific Gap Ranges
  int intervals[PATTERN_SIZE];
  for (int i = 0; i < PATTERN_SIZE; i++) {
    intervals[i] = knockTimes[i + 1] - knockTimes[i];
    int minAllowed = expectedPattern[i].minMs;
    int maxAllowed = expectedPattern[i].maxMs;

    Serial.printf("  Interval %d [%s]: %d ms (Allowed: %d - %d ms)\n",
                  i + 1, expectedPattern[i].typeName, intervals[i], minAllowed, maxAllowed);

    if (intervals[i] < minAllowed || intervals[i] > maxAllowed) {
      Serial.printf("❌ Interval %d out of bounds (%d ms not in [%d, %d] ms)\n",
                    i + 1, intervals[i], minAllowed, maxAllowed);
      handleFailure();
      return;
    }
  }

  // 3. Pattern Matched Successfully
  Serial.println("🎉 ACCESS GRANTED! Secret Knock Accepted!");
  handleSuccess();
}

// =================================================================================
// 11. LCD DISPLAY HELPER (Core 1)
// =================================================================================
void updateLcdDisplay() {
  lcd.clear();
  switch (currentState) {
    case READY:
      lcd.setCursor(0, 0);
      lcd.print("KNOCK PUZZLE    ");
      lcd.setCursor(0, 1);
      lcd.print("READY TO KNOCK  ");
      break;

    case STARTED:
      lcd.setCursor(0, 0);
      lcd.print("SECRET KNOCK    ");
      lcd.setCursor(0, 1);
      lcd.printf("ATTEMPT #%d      ", attemptNumber);
      break;

    case FAILED:
      lcd.setCursor(0, 0);
      lcd.print("WRONG RHYTHM!   ");
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
// 12. BACKGROUND NETWORK & MQTT TASK (Core 0 — Independent Thread)
// =================================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[16];
  unsigned int len = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
  memcpy(message, payload, len);
  message[len] = '\0';

  // Trim trailing whitespace/newlines
  for (int i = (int)len - 1; i >= 0 && (message[i] == ' ' || message[i] == '\r' || message[i] == '\n'); i--) {
    message[i] = '\0';
  }

  Serial.printf("📩 [MQTT Core 0] Command Received: %s\n", message);

  // Transfer command thread-safely to Core 1
  if (cmdQueue != NULL) {
    CommandMsg msg;
    strncpy(msg.cmd, message, sizeof(msg.cmd) - 1);
    msg.cmd[sizeof(msg.cmd) - 1] = '\0';
    xQueueSend(cmdQueue, &msg, 0);
  }
}

bool discoverMQTTServer() {
  Serial.println("\n🔍 [mDNS Core 0] Discovering Escape Room Control Server...");

  if (!MDNS.begin("ESP32-KnockProp")) {
    Serial.println("⚠️ [mDNS Core 0] Responder init failed, querying...");
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

    String clientId = "ESP32-KnockProp-" + String(GAME_ID);

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