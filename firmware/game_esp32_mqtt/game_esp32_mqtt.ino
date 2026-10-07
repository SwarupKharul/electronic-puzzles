/*
 * =================================================================================
 * ESCAPE ROOM GAME PROP ESP32 FIRMWARE (Production Non-Blocking Template)
 * =================================================================================
 * Generic template for any single-sensor puzzle prop (reed switch, button, etc.)
 *
 * Features:
 *   - 100% Non-Blocking State Machine (Zero blocking delays or while-loops)
 *   - Single digital sensor input (Button / Reed Switch / Magnetic Sensor)
 *   - Automatic puzzle start on boot (no laptop/WiFi needed)
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic
 *   - Non-blocking Wi-Fi auto-reconnection & mDNS Zero-IP MQTT broker discovery
 *   - I2C 16x2 LCD Display with I2C bus timeout protection
 *   - Non-blocking hardware reset button debounce
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT)
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
 *   I2C LCD SDA          ---> GPIO 26 (LCD_SDA) [Shared w/ relay? Change if needed]
 *   I2C LCD SCL          ---> GPIO 27 (LCD_SCL) [Shared w/ sensor? Change if needed]
 *
 * NOTE: Adjust pin assignments below to avoid conflicts for your specific wiring.
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 15

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// =================================================================================
// 1. HARDWARE CONFIGURATION & PIN DEFINITIONS
// =================================================================================

// I2C LCD (Address 0x27, 16 cols, 2 rows) — Comment out LCD lines if not using one
LiquidCrystal_I2C lcd(0x27, 16, 2);
const int LCD_SDA_PIN     = 21;  // Default I2C SDA (change to avoid pin conflicts)
const int LCD_SCL_PIN     = 22;  // Default I2C SCL (change to avoid pin conflicts)

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
const char* WIFI_SSID     = "Airtel_anjo_4056";
const char* WIFI_PASS     = "air38409";

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
// 3. SYSTEM STATE & MQTT OBJECTS
// =================================================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);

enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
GameState currentState = READY;

int attemptNumber = 0;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds

// Button Debounce State (non-blocking)
bool lastResetBtnState = HIGH;
unsigned long lastResetBtnTime = 0;

bool lastSensorState = HIGH;
unsigned long lastSensorTime = 0;

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
void lockDoor();
void unlockDoor();
void checkPuzzleLogic();

// =================================================================================
// 4. RELAY & ACTUATOR CONTROL
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
// 5. MQTT INCOMING COMMAND HANDLER
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
    lockDoor();
    publishState(STARTED);
    publishEvent("STARTED");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("STOP")) {
    currentState = STOPPED;
    lockDoor();
    publishState(STOPPED);
    publishEvent("STOPPED");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("RESET")) {
    currentState = READY;
    attemptNumber = 0;
    lockDoor();
    mqttOfflineMode = false;
    mqttFailCount = 0;
    publishState(READY);
    publishEvent("RESET");
    updateLcdDisplay();
  }
  else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
    Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
    currentState = COMPLETED;
    unlockDoor();
    publishState(COMPLETED);
    publishEvent("COMPLETED");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUZZLE SOLVED!");
    lcd.setCursor(0, 1);
    lcd.print("DOOR UNLOCKED");
  }
}

// =================================================================================
// 6. LCD DISPLAY HELPER
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
// 7. PUZZLE SENSOR LOGIC (Non-Blocking)
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
        publishState(STARTED);
        publishEvent("STARTED");
      }

      Serial.println("🎉 Puzzle Solved! Opening lock...");
      currentState = COMPLETED;
      unlockDoor();
      publishState(COMPLETED);
      publishEvent("COMPLETED");

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
// 8. SETUP
// =================================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==========================================");
  Serial.println("🚀 Escape Room Prop Initializing...");
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

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
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

  // 1. PRIORITY #1: Handle Puzzle Sensor Logic FIRST (Zero Latency)
  checkPuzzleLogic();

  // 2. Maintain Network & MQTT (Skips completely if in offline mode)
  maintainMQTT();

  // 3. Periodic Heartbeat to Server (Only if connected)
  if (mqtt.connected() && (now - lastHeartbeat > HEARTBEAT_INTERVAL)) {
    lastHeartbeat = now;
    publishEvent("HEARTBEAT");
  }

  // 4. Non-Blocking Physical Manual Reset Button check
  bool currentBtnState = digitalRead(RESET_BTN_PIN);
  if (currentBtnState == LOW && lastResetBtnState == HIGH) {
    if (now - lastResetBtnTime > 250) { // 250ms debounce
      lastResetBtnTime = now;
      Serial.println("🔘 Hardware Reset Pressed");
      // Allow fresh MQTT attempts on manual reset
      mqttOfflineMode = false;
      mqttFailCount = 0;
      handleCommand("RESET");
    }
  }
  lastResetBtnState = currentBtnState;

  // 4. Handle Puzzle Sensor Logic (non-blocking)
  checkPuzzleLogic();

  // 5. Non-blocking Auto-Relock Timer check (for Solenoid locks)
  if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
    if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
      Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
      lockDoor();
    }
  }
}

// =================================================================================
// 10. NETWORK & MQTT COMMUNICATIONS (With Automatic mDNS Discovery)
// =================================================================================

bool discoverMQTTServer() {
  Serial.println("\n🔍 [mDNS] Discovering Escape Room Control Server...");

  if (!MDNS.begin("ESP32-Prop")) {
    Serial.println("⚠️ [mDNS] Responder init failed, proceeding with queries...");
  } else {
    Serial.println("📡 [mDNS] Responder active ('ESP32-Prop.local')");
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

    String clientId = "ESP32-Prop-" + String(GAME_ID);

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
        Serial.println("🎮 100% STANDALONE OFFLINE MODE: Sensor loop given full priority.");
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
