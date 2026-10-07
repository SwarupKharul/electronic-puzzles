/*
 * =================================================================================
 * ESCAPE ROOM - KNOCK KNOCK RHYTHM PUZZLE PROP (Production ESP32 Firmware)
 * =================================================================================
 * Features:
 *   - 100% Non-Blocking State Machine (Zero blocking delays or while-loops)
 *   - Piezo Knock Sensor with dynamic debouncing & timing tolerance verification
 *   - Automatic live tracking & failure cooldown without freezing MQTT / buttons
 *   - Unified SUCCESS / COMPLETE state handling (reliable instant solve)
 *   - 3-Pin Relay Module actuation (Solenoid / Maglock) on puzzle completion
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic
 *   - Non-blocking Wi-Fi auto-reconnection & mDNS Zero-IP MQTT broker discovery
 *   - I2C 16x2 LCD Display with I2C bus timeout protection against noise lockups
 *   - Glitch-free non-blocking hardware reset button debounce
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT)
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
 *      Piezo Sensor (+) --->   GPIO 34 (Analog In, 1M pull-down resistor to GND)
 *      Piezo Sensor (-) --->   GND
 *      Reset Button     --->   GPIO 14 (Active LOW to GND, internal pullup)
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 15

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESPmDNS.h>

// =================================================================================
// 1. HARDWARE CONFIGURATION & PIN DEFINITIONS
// =================================================================================

// I2C LCD (Address 0x27, 16 cols, 2 rows)
LiquidCrystal_I2C lcd(0x27, 16, 2);
const int I2C_SDA_PIN     = 26;
const int I2C_SCL_PIN     = 27;

// Sensors & Actuators
const int PIEZO_PIN       = 34; // Analog input for Piezo knock sensor
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
// const char* WIFI_SSID     = "Airtel_anjo_4056";
// const char* WIFI_PASS     = "air38409";

const char* WIFI_SSID     = "operations_404";
const char* WIFI_PASS     = "Mytplink2020";

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
const int DEBOUNCE_TIME    = 10;  // Debounce to ignore piezo sensor ringing/echo (ms) — MUST match recorder
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

// Knock State Variables
const int MAX_KNOCKS_BUFFER = 32;
unsigned long knockTimes[MAX_KNOCKS_BUFFER];
int knockCount = 0;
unsigned long lastKnock = 0;
bool waitingForPattern = false;

// =================================================================================
// 4. SYSTEM STATE & MQTT OBJECTS
// =================================================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);

enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
GameState currentState = READY;

// Non-blocking timer for state transitions (replaces blocking delays)
unsigned long stateTransitionTime = 0;

int attemptNumber = 0;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds (fail-proof real-time link integrity)

// Button Debounce State
bool lastButtonState = HIGH;
unsigned long lastButtonPressTime = 0;

// MQTT Topics & Connection Tracking
char topicStatus[64];
char topicState[64];
char topicEvent[64];
char topicCmd[64];

unsigned long lastMqttRetry = 0;
unsigned long lastWiFiRetry = 0;
int mqttFailCount = 0;
const int MAX_MQTT_ATTEMPTS = 3;     // Max 3 connection attempts before giving up to prevent blocking main thread
bool mqttOfflineMode = false;       // Set to true after 3 failed attempts (runs 100% offline with zero latency)

// Forward Declarations
void setupWiFi();
void maintainWiFi();
bool discoverMQTTServer();
void maintainMQTT();
void publishStatus(const char* status);
void publishState(GameState state);
void publishEvent(const char* eventName);
void handleCommand(String cmd);
void updateLcdDisplay();
void checkKnockInput();
void evaluatePattern();
void resetKnockState();
void lockDoor();
void unlockDoor();
void handleSuccess();
void handleFailure();

// =================================================================================
// 5. RELAY & ACTUATOR CONTROL
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
// 6. SUCCESS & FAILURE HANDLERS (Non-Blocking & Unified)
// =================================================================================
void handleSuccess() {
  currentState = COMPLETED;
  unlockDoor(); // Energize relay

  publishState(COMPLETED);
  publishEvent("COMPLETED");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PUZZLE SOLVED!");
  lcd.setCursor(0, 1);
  lcd.print("DOOR UNLOCKED");
}

void handleFailure() {
  currentState = FAILED;
  publishState(FAILED);
  publishEvent("FAILED");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WRONG RHYTHM!");
  lcd.setCursor(0, 1);
  lcd.print("TRY AGAIN...");

  // Non-blocking 1.8s timer to return to STARTED
  stateTransitionTime = millis() + 1800;
}

// =================================================================================
// 7. MQTT INCOMING COMMAND HANDLER
// =================================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.printf("📩 [MQTT] Command Received: %s\n", message.c_str());
  handleCommand(message);
}

void handleCommand(String cmd) {
  if (cmd.equalsIgnoreCase("START") || cmd.equalsIgnoreCase("RESTART")) {
    attemptNumber++;
    currentState = STARTED;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    publishState(STARTED);
    publishEvent("STARTED");
    updateLcdDisplay();
  } 
  else if (cmd.equalsIgnoreCase("STOP")) {
    currentState = STOPPED;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    publishState(STOPPED);
    publishEvent("STOPPED");
    updateLcdDisplay();
  } 
  else if (cmd.equalsIgnoreCase("RESET")) {
    currentState = READY;
    attemptNumber = 0;
    stateTransitionTime = 0;
    lockDoor();
    resetKnockState();
    mqttOfflineMode = false;
    mqttFailCount = 0;
    publishState(READY);
    publishEvent("RESET");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
    Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
    handleSuccess();
  }
}

// =================================================================================
// 8. SETUP
// =================================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==========================================");
  Serial.println("🚪 Escape Room: Knock Knock Puzzle Initializing...");
  Serial.println("==========================================");

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
  lcd.print("BOOTING...");

  // Construct MQTT Topics
  snprintf(topicStatus, sizeof(topicStatus), "%s/%s/status", ROOT_TOPIC, GAME_ID);
  snprintf(topicState,  sizeof(topicState),  "%s/%s/state",  ROOT_TOPIC, GAME_ID);
  snprintf(topicEvent,  sizeof(topicEvent),  "%s/%s/event",  ROOT_TOPIC, GAME_ID);
  snprintf(topicCmd,    sizeof(topicCmd),    "%s/%s/cmd",    ROOT_TOPIC, GAME_ID);

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15); // 15s keepalive ensures fast broker detection
  mqtt.setSocketTimeout(1); // 1s socket timeout prevents network connection stalls

  setupWiFi();
  mqtt.setCallback(mqttCallback);

  if (AUTO_START_ON_BOOT) {
    attemptNumber = 1;
    currentState = STARTED;
    publishState(STARTED);
    publishEvent("STARTED");
  } else {
    currentState = READY;
    publishState(READY);
    publishEvent("READY");
  }

  updateLcdDisplay();
}

// =================================================================================
// 9. MAIN LOOP (100% Non-Blocking & Sensor-Prioritized)
// =================================================================================
void loop() {
  unsigned long now = millis();

  // 1. PRIORITY #1: Handle Knock Sensing & Pattern Logic FIRST (Zero Latency)
  if (currentState == STARTED || currentState == READY) {
    checkKnockInput();
  }

  // 2. Maintain Network & MQTT (Skips completely if player is knocking or in offline mode)
  maintainMQTT();

  // 3. Periodic Heartbeat to Server (Only if connected)
  if (mqtt.connected() && (now - lastHeartbeat > HEARTBEAT_INTERVAL)) {
    lastHeartbeat = now;
    publishEvent("HEARTBEAT");
  }

  // 4. Non-Blocking Physical Manual Reset Button check
  bool currentBtnState = digitalRead(RESET_BTN_PIN);
  if (currentBtnState == LOW && lastButtonState == HIGH) {
    if (now - lastButtonPressTime > 250) {
      lastButtonPressTime = now;
      Serial.println("🔘 Hardware Reset Pressed");
      // Allow fresh MQTT attempts on manual reset
      mqttOfflineMode = false;
      mqttFailCount = 0;
      handleCommand("RESET");
    }
  }
  lastButtonState = currentBtnState;

  // 5. Non-Blocking State Transition Check (for FAILED cooldown)
  if (stateTransitionTime > 0 && now >= stateTransitionTime) {
    stateTransitionTime = 0;
    if (currentState == FAILED) {
      resetKnockState();
      currentState = STARTED;
      publishState(STARTED);
      updateLcdDisplay();
    }
  }

  // 6. Non-blocking Auto-Relock Timer check (for Solenoid locks)
  if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
    if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
      Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
      lockDoor();
    }
  }
}

// =================================================================================
// 10. KNOCK LOGIC & PATTERN EVALUATION
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
      publishState(STARTED);
      publishEvent("STARTED");
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
// 11. LCD DISPLAY HELPER
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
// 12. NETWORK & MQTT COMMUNICATIONS (With Automatic mDNS Discovery)
// =================================================================================

bool discoverMQTTServer() {
  Serial.println("\n🔍 [mDNS] Discovering Escape Room Control Server...");

  if (!MDNS.begin("ESP32-KnockProp")) {
    Serial.println("⚠️ [mDNS] Responder init failed, proceeding with queries...");
  } else {
    Serial.println("📡 [mDNS] Responder active ('ESP32-KnockProp.local')");
  }

  // 1. Try DNS-SD Service Discovery
  Serial.println("  1️⃣ Scanning for '_mqtt._tcp' service on local network...");
  int n = MDNS.queryService("mqtt", "tcp");
  if (n > 0) {
    activeMqttIP = MDNS.address(0);
    activeMqttPort = MDNS.port(0);
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS] Discovered MQTT Service via DNS-SD!\n");
    Serial.printf("     Broker IP: %s\n", activeMqttIP.toString().c_str());
    Serial.printf("     Broker Port: %d\n", activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 2. Try resolving 'escaperoom.local'
  Serial.printf("  2️⃣ Querying mDNS host '%s.local'...\n", MDNS_HOST_ESCAPEROOM);
  activeMqttIP = MDNS.queryHost(MDNS_HOST_ESCAPEROOM);
  if (activeMqttIP != IPAddress(0, 0, 0, 0)) {
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS] Resolved '%s.local' -> %s (Port: %d)\n", MDNS_HOST_ESCAPEROOM, activeMqttIP.toString().c_str(), activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 3. Try resolving laptop OS hostname 'pop-os.local'
  Serial.printf("  3️⃣ Querying mDNS host '%s.local'...\n", MDNS_HOST_LAPTOP);
  activeMqttIP = MDNS.queryHost(MDNS_HOST_LAPTOP);
  if (activeMqttIP != IPAddress(0, 0, 0, 0)) {
    serverDiscovered = true;
    Serial.printf("  ✅ [mDNS] Resolved '%s.local' -> %s (Port: %d)\n", MDNS_HOST_LAPTOP, activeMqttIP.toString().c_str(), activeMqttPort);
    mqtt.setServer(activeMqttIP, activeMqttPort);
    return true;
  }

  // 4. Fallback to hardcoded IP if router blocks multicast packets
  Serial.printf("  ⚠️ [mDNS] Discovery timed out. Using fallback IP: %s:%d\n", MQTT_SERVER_FALLBACK, activeMqttPort);
  activeMqttIP.fromString(MQTT_SERVER_FALLBACK);
  mqtt.setServer(activeMqttIP, activeMqttPort);
  return false;
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("📶 Connecting to Wi-Fi");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 15) {
    delay(200);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi Connected!");
    Serial.print("   Prop IP Address: ");
    Serial.println(WiFi.localIP());

    discoverMQTTServer();
  } else {
    Serial.println("\n⚠️ Wi-Fi Timeout. Entering 100% standalone offline mode.");
    mqttOfflineMode = true;
  }
}

void maintainWiFi() {
  if (mqttOfflineMode) return; // Do not interrupt sensor loop when in standalone offline mode

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiRetry > 15000) {
      lastWiFiRetry = now;
      Serial.println("📶 [Wi-Fi] Connection lost. Attempting auto-reconnect...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}

void maintainMQTT() {
  // CRITICAL: If player is actively knocking, pause ALL network calls to guarantee zero latency!
  if (waitingForPattern || knockCount > 0) return;

  // If already failed 3 times or Wi-Fi is off, stay offline without blocking the main thread
  if (mqttOfflineMode) return;

  maintainWiFi();

  if (WiFi.status() != WL_CONNECTED) return;

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttRetry > 5000) {
    lastMqttRetry = now;
    mqttFailCount++;

    Serial.printf("🔌 [MQTT Attempt %d/%d] Connecting to %s:%d...\n", 
                  mqttFailCount, MAX_MQTT_ATTEMPTS,
                  activeMqttIP.toString().c_str(), activeMqttPort);

    String clientId = "ESP32-KnockProp-" + String(GAME_ID);

    // Connect with Last Will & Testament (LWT)
    if (mqtt.connect(clientId.c_str(), topicStatus, 1, true, "offline")) {
      Serial.println("✅ Connected to MQTT Broker!");
      mqttFailCount = 0;
      publishStatus("online");
      mqtt.subscribe(topicCmd);
      publishState(currentState);
    } else {
      Serial.printf("⚠️ MQTT Connection Failed (rc=%d).\n", mqtt.state());

      if (mqttFailCount >= MAX_MQTT_ATTEMPTS) {
        mqttOfflineMode = true;
        Serial.println("🛑 [MQTT] Failed 3 attempts. Stopping network retries permanently!");
        Serial.println("🎮 100% STANDALONE OFFLINE MODE: Knock detection given full priority.");
      }
    }
  }
}

void publishStatus(const char* status) {
  if (mqtt.connected()) {
    mqtt.publish(topicStatus, status, true);
  }
}

void publishState(GameState state) {
  if (!mqtt.connected()) return;
  const char* stateStr = "READY";
  switch (state) {
    case READY:     stateStr = "READY";     break;
    case STARTED:   stateStr = "STARTED";   break;
    case FAILED:    stateStr = "FAILED";    break;
    case COMPLETED: stateStr = "COMPLETED"; break;
    case STOPPED:   stateStr = "STOPPED";   break;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"attempt\":%d}", stateStr, attemptNumber);
  mqtt.publish(topicState, buf, true);
}

void publishEvent(const char* eventName) {
  if (!mqtt.connected()) return;
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"event\":\"%s\",\"attempt\":%d}", eventName, attemptNumber);
  mqtt.publish(topicEvent, buf, false);
}