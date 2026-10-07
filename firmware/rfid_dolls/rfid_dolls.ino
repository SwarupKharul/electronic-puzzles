/*
 * =================================================================================
 * ESCAPE ROOM - 4 RFID DOLLS PUZZLE (Production ESP32 Firmware)
 * =================================================================================
 * Features:
 *   - Supports 4x MFRC522 RFID Readers on a shared SPI bus
 *   - 100% Non-Blocking State Machine (Zero delay locks, responsive to MQTT & buttons)
 *   - Automatic live tracking & debounce of all 4 RFID slots in all states
 *   - Dynamic card-swap detection (evaluates even if cards swapped without lifting)
 *   - Automated MFRC522 PCD register self-healing & antenna gain optimization
 *   - Non-blocking Wi-Fi auto-reconnect & mDNS Zero-IP MQTT broker discovery
 *   - Servo motor actuation (0° reset/locked -> 180° solved/unlocked)
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic for Solenoid / Maglock
 *   - I2C 16x2 LCD Display with I2C bus timeout protection against noise lockups
 *   - Non-blocking hardware reset button debouncing
 *   - Remote Control via MQTT (START, RESTART, STOP, RESET, SOLVE / OVERRIDE)
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT)
 *
 * ---------------------------------------------------------------------------------
 * WIRING GUIDE:
 * ---------------------------------------------------------------------------------
 * 1. MFRC522 RFID READERS (Shared SPI Bus + Dedicated SS/CS Pins):
 *      [All 4 Readers]         [ESP32]
 *      SCK              --->   GPIO 18
 *      MISO             --->   GPIO 19
 *      MOSI             --->   GPIO 23
 *      RST              --->   GPIO 22
 *      3.3V             --->   3.3V (Ensure clean, sufficient current supply!)
 *      GND              --->   GND
 *
 *      [Reader 1 (Doll 1 - Blue)]
 *      SDA / SS / CS    --->   GPIO 15 (SS1)
 *
 *      [Reader 2 (Doll 2 - Orange)]
 *      SDA / SS / CS    --->   GPIO 4  (SS2)
 *
 *      [Reader 3 (Doll 3 - Yellow)]
 *      SDA / SS / CS    --->   GPIO 16 (SS3)
 *
 *      [Reader 4 (Doll 4 - Red)]
 *      SDA / SS / CS    --->   GPIO 17 (SS4)
 *
 * 2. I2C 16x2 LCD DISPLAY:
 *      VCC              --->   VIN (5V) or 3.3V
 *      GND              --->   GND
 *      SDA              --->   GPIO 26
 *      SCL              --->   GPIO 27
 *
 * 3. 3-PIN RELAY MODULE (Maglock / Solenoid Lock):
 *      VCC              --->   VIN (5V)
 *      GND              --->   GND
 *      IN / SIG / S     --->   GPIO 25 (RELAY_PIN)
 *
 * 4. SERVO MOTOR (Mechanism / Lock Actuator):
 *      VCC (Red)        --->   VIN (5V External / Board 5V)
 *      GND (Brown/Black)--->   GND (Common Ground)
 *      PWM (Orange/Yel) --->   GPIO 13 (SERVO_PIN)
 *
 * 5. PHYSICAL HARDWARE BUTTON & BUZZER:
 *      Reset Button     --->   GPIO 14 (Active LOW to GND, internal pullup)
 *      Piezo Buzzer     --->   GPIO 32 (Positive to Pin, Negative to GND, -1 to disable)
 * =================================================================================
 */

#define MQTT_KEEPALIVE 15
#define MQTT_SOCKET_TIMEOUT 15

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>

// =================================================================================
// 1. HARDWARE PIN DEFINITIONS & CONFIGURATION
// =================================================================================

// RFID Shared SPI Bus
#define RFID_SCK   18
#define RFID_MISO  19
#define RFID_MOSI  23
#define RFID_RST   22

// RFID Slave Select (SS / CS) Pins for the 4 Readers
#define SS1 15  // Reader 1 - Doll 1 (Blue)
#define SS2 4   // Reader 2 - Doll 2 (Orange)
#define SS3 16  // Reader 3 - Doll 3 (Yellow)
#define SS4 17  // Reader 4 - Doll 4 (Red)

// I2C LCD Pins (16x2)
#define LCD_SDA 26
#define LCD_SCL 27
#define LCD_I2C_ADDR 0x27

// Relay & Actuator Pins
#define RELAY_PIN     25 // Solenoid / Maglock Relay
#define SERVO_PIN     13 // Servo Motor Signal Pin (-1 to disable)
#define RESET_BTN_PIN 14 // Optional manual reset button (active LOW)
#define BUZZER_PIN    32 // Optional local piezo buzzer (-1 to disable)

// Servo Motor Positions (Degrees)
const int SERVO_LOCKED_POS   = 0;   // 0° when reset / standby / locked
const int SERVO_UNLOCKED_POS = 180; // 180° when puzzle is solved / unlocked

// Relay Trigger Logic (Most 3-pin relay modules are ACTIVE-LOW)
const bool RELAY_ACTIVE_LOW = true;

// Auto-relock pulse duration in milliseconds (0 = stays unlocked until RESET)
const unsigned long AUTO_RELOCK_DELAY_MS = 0;
unsigned long unlockedAt = 0;

// Set to true to start scanning immediately upon powering up without waiting for laptop START command
const bool AUTO_START_ON_BOOT = true;

// =================================================================================
// 2. NETWORK & mDNS / MQTT CONFIGURATION (ZERO-IP SETUP)
// =================================================================================
const char* WIFI_SSID     = "Airtel_anjo_4056";
const char* WIFI_PASS     = "air38409";

// const char* WIFI_SSID     = "operations_404";
// const char* WIFI_PASS     = "Mytplink2020";

// mDNS Configuration - The ESP32 discovers the server automatically!
const char* MDNS_HOST_ESCAPEROOM = "escaperoom"; // Will query 'escaperoom.local'
const char* MDNS_HOST_LAPTOP     = "pop-os";     // Native Linux hostname fallback
const char* MQTT_SERVER_FALLBACK = "192.168.1.9"; // Fallback only if router blocks multicast

const int   MQTT_PORT_DEFAULT    = 1884;         // Default port (auto-discovered via mDNS)
int         activeMqttPort       = MQTT_PORT_DEFAULT;
IPAddress   activeMqttIP;
bool        serverDiscovered     = false;

const char* GAME_ID       = "game2";          // Matches "id" in games.json ("game2")
const char* ROOT_TOPIC    = "escaperoom";     // Matches "rootTopic" in games.json

// =================================================================================
// 3. TARGET PATTERN / EXPECTED UIDs (CONFIGURABLE)
// =================================================================================

const byte NUM_READERS = 4;
const byte SS_PINS[NUM_READERS] = { SS1, SS2, SS3, SS4 };
const byte EXPECTED_UID_SIZE = 7;

// Expected UID for Slot 1 (Doll 1 - Blue)
byte expectedUID1[EXPECTED_UID_SIZE] = { 0x04, 0xC4, 0xE4, 0x49, 0xBC, 0x2A, 0x81 };

// Expected UID for Slot 2 (Doll 2 - Orange)
byte expectedUID2[EXPECTED_UID_SIZE] = { 0x04, 0x7C, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

// Expected UID for Slot 3 (Doll 3 - Yellow)
byte expectedUID3[EXPECTED_UID_SIZE] = { 0x04, 0xC8, 0xBB, 0x7A, 0xC1, 0x2A, 0x81 };

// Expected UID for Slot 4 (Doll 4 - Red)
byte expectedUID4[EXPECTED_UID_SIZE] = { 0x04, 0x66, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

const byte* expectedUIDs[NUM_READERS] = {
    expectedUID1,
    expectedUID2,
    expectedUID3,
    expectedUID4
};

const char* readerNames[NUM_READERS] = {
    "DOLL 1 (BLUE)",
    "DOLL 2 (ORANGE)",
    "DOLL 3 (YELLOW)",
    "DOLL 4 (RED)"
};

// =================================================================================
// 4. GLOBAL OBJECTS & STATE VARIABLES
// =================================================================================

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Servo puzzleServo;

MFRC522 readers[NUM_READERS] = {
    MFRC522(SS1, RFID_RST),
    MFRC522(SS2, RFID_RST),
    MFRC522(SS3, RFID_RST),
    MFRC522(SS4, RFID_RST)
};

// Card Detection States
bool cardPresent[NUM_READERS]        = { false, false, false, false };
byte detectedUID[NUM_READERS][10];
byte detectedUIDSize[NUM_READERS]    = { 0, 0, 0, 0 };
byte consecutiveMisses[NUM_READERS]  = { 0, 0, 0, 0 };
unsigned long consecutiveErrorCount[NUM_READERS] = { 0, 0, 0, 0 };

// Evaluation Flags
bool patternEvaluated = false;
bool puzzleSolved     = false;

// Game State Machine
enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
GameState currentState = READY;

// Non-blocking timer for state transitions (replaces blocking delay)
unsigned long stateTransitionTime = 0;

int attemptNumber = 0;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 3000; // 3 seconds (fail-proof real-time link integrity)

// Button Debounce State
bool lastButtonState = HIGH;
unsigned long lastButtonPressTime = 0;

// MQTT Client & Topics
WiFiClient espClient;
PubSubClient mqtt(espClient);

char topicStatus[64];
char topicState[64];
char topicEvent[64];
char topicCmd[64];

unsigned long lastMqttRetry = 0;
unsigned long lastWiFiRetry = 0;
int mqttFailCount = 0;

// Forward Declarations
void setupWiFi();
void maintainWiFi();
bool discoverMQTTServer();
void maintainMQTT();
void publishStatus(const char* status);
void publishState(GameState state);
void publishEvent(const char* eventName);
void handleCommand(String cmd);
void deselectAllReaders();
void selectReader(byte index);
bool scanReader(byte index);
void checkAllCards();
void evaluatePattern();
void handleSuccess();
void handleFailure();
void lockDoor();
void unlockDoor();
void moveServo(int angle);
void triggerLocalBuzzer(bool success);
void updateLcdDisplay();
void showPuzzleStatus();
bool uidMatches(const byte* actualUID, byte actualSize, const byte* expectedUID, byte expectedSize = EXPECTED_UID_SIZE);
void printUID(const byte* uid, byte size);
void printUIDInline(const byte* uid, byte size);
void recoverReader(byte readerIdx);

// =================================================================================
// 5. RELAY & ACTUATOR CONTROL (Relay + Servo Motor)
// =================================================================================

void moveServo(int angle) {
    if (SERVO_PIN >= 0) {
        puzzleServo.write(angle);
        Serial.printf("⚙️ [SERVO] Position set to %d°\n", angle);
    }
}

void lockDoor() {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    moveServo(SERVO_LOCKED_POS); // Reset servo back to 0°
    unlockedAt = 0;
    Serial.println("🔒 [ACTUATOR] Locked (Relay DE-ENERGIZED, Servo at 0°)");
}

void unlockDoor() {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
    moveServo(SERVO_UNLOCKED_POS); // Move servo to 180° on puzzle solve
    unlockedAt = millis();
    Serial.println("🔓 [ACTUATOR] Unlocked (Relay ENERGIZED, Servo at 180°)");
}

void triggerLocalBuzzer(bool success) {
    if (BUZZER_PIN < 0) return;
    // Non-blocking: tone() duration parameter handles auto-stop without delay()
    if (success) {
        tone(BUZZER_PIN, 1400, 300); // Single celebratory tone (no blocking delay)
    } else {
        tone(BUZZER_PIN, 250, 400);  // Low failure buzz
    }
}

// =================================================================================
// 6. RFID HARDWARE HELPERS & RECOVERY
// =================================================================================

void deselectAllReaders() {
    for (byte i = 0; i < NUM_READERS; i++) {
        digitalWrite(SS_PINS[i], HIGH);
    }
}

void selectReader(byte index) {
    deselectAllReaders();
    digitalWrite(SS_PINS[index], LOW);
    delayMicroseconds(50);
}

void recoverReader(byte readerIdx) {
    selectReader(readerIdx);
    readers[readerIdx].PCD_Init();
    readers[readerIdx].PCD_SetAntennaGain(MFRC522::RxGain_max);
    deselectAllReaders();
    consecutiveErrorCount[readerIdx] = 0;
    Serial.printf("🔄 [RFID] Reader %d self-healed & re-initialized\n", readerIdx + 1);
}

void printUID(const byte* uid, byte size) {
    for (byte i = 0; i < size; i++) {
        if (uid[i] < 0x10) Serial.print("0");
        Serial.print(uid[i], HEX);
        if (i < size - 1) Serial.print(" ");
    }
    Serial.println();
}

void printUIDInline(const byte* uid, byte size) {
    for (byte i = 0; i < size; i++) {
        if (uid[i] < 0x10) Serial.print("0");
        Serial.print(uid[i], HEX);
        if (i < size - 1) Serial.print(" ");
    }
}

bool uidMatches(const byte* actualUID, byte actualSize, const byte* expectedUID, byte expectedSize) {
    if (actualSize != expectedSize) {
        return false;
    }
    for (byte i = 0; i < expectedSize; i++) {
        if (actualUID[i] != expectedUID[i]) {
            return false;
        }
    }
    return true;
}

// Scans an individual RFID reader using PICC_WakeupA
bool scanReader(byte readerIdx) {
    selectReader(readerIdx);

    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);

    // Reset registers to clear any bus collisions
    readers[readerIdx].PCD_WriteRegister(readers[readerIdx].TxModeReg, 0x00);
    readers[readerIdx].PCD_WriteRegister(readers[readerIdx].RxModeReg, 0x00);
    readers[readerIdx].PCD_WriteRegister(readers[readerIdx].ModWidthReg, 0x26);

    // Wake up card in RF field (with quick retry to prevent RF jitter misses)
    MFRC522::StatusCode status = readers[readerIdx].PICC_WakeupA(bufferATQA, &bufferSize);
    if (status != MFRC522::STATUS_OK) {
        delayMicroseconds(1000);
        status = readers[readerIdx].PICC_WakeupA(bufferATQA, &bufferSize);
    }

    bool cardFound = false;
    if (status == MFRC522::STATUS_OK) {
        consecutiveErrorCount[readerIdx] = 0;
        if (readers[readerIdx].PICC_ReadCardSerial()) {
            cardFound = true;
            
            // Check if UID changed while resting on reader (card swapped)
            bool uidChanged = (detectedUIDSize[readerIdx] != readers[readerIdx].uid.size);
            if (!uidChanged) {
                for (byte b = 0; b < readers[readerIdx].uid.size; b++) {
                    if (detectedUID[readerIdx][b] != readers[readerIdx].uid.uidByte[b]) {
                        uidChanged = true;
                        break;
                    }
                }
            }

            detectedUIDSize[readerIdx] = readers[readerIdx].uid.size;
            for (byte b = 0; b < readers[readerIdx].uid.size; b++) {
                detectedUID[readerIdx][b] = readers[readerIdx].uid.uidByte[b];
            }

            if (uidChanged && cardPresent[readerIdx]) {
                // Card was swapped without leaving reader empty
                patternEvaluated = false;
                Serial.printf("🔄 Reader %d [%s]: Card SWAPPED! New UID: ", readerIdx + 1, readerNames[readerIdx]);
                printUID(detectedUID[readerIdx], detectedUIDSize[readerIdx]);
            }

            readers[readerIdx].PICC_HaltA();
            readers[readerIdx].PCD_StopCrypto1();
        }
    } else {
        consecutiveErrorCount[readerIdx]++;
        // If reader fails 50 consecutive scans, perform soft re-init
        if (consecutiveErrorCount[readerIdx] > 50) {
            recoverReader(readerIdx);
        }
    }

    deselectAllReaders();
    return cardFound;
}

// =================================================================================
// 7. PATTERN EVALUATION & CORE LOGIC
// =================================================================================

int getCardsPresentCount() {
    int count = 0;
    for (byte i = 0; i < NUM_READERS; i++) {
        if (cardPresent[i]) count++;
    }
    return count;
}

void checkAllCards() {
    bool cardStateChanged = false;

    // Scan each of the 4 RFID readers
    for (byte i = 0; i < NUM_READERS; i++) {
        bool found = scanReader(i);

        if (found) {
            consecutiveMisses[i] = 0;
            if (!cardPresent[i]) {
                cardPresent[i] = true;
                cardStateChanged = true;
                patternEvaluated = false; // Reset latch so pattern can evaluate
                Serial.printf("📍 Reader %d [%s]: Card PLACED! UID: ", i + 1, readerNames[i]);
                printUID(detectedUID[i], detectedUIDSize[i]);
            }
        } else {
            if (cardPresent[i]) {
                consecutiveMisses[i]++;
                // Require 2 consecutive misses to prevent RF flicker
                if (consecutiveMisses[i] >= 2) {
                    cardPresent[i] = false;
                    detectedUIDSize[i] = 0;
                    memset(detectedUID[i], 0, sizeof(detectedUID[i]));
                    cardStateChanged = true;
                    patternEvaluated = false; // Reset latch when card is removed
                    Serial.printf("💨 Reader %d [%s]: Card REMOVED\n", i + 1, readerNames[i]);
                }
            }
        }
        delayMicroseconds(500); // Minimal SPI bus settling time between reader switches (was 5ms)
    }

    int totalDetected = getCardsPresentCount();

    // If cards were lifted or rearranged, ensure latch is cleared
    if (totalDetected < 4) {
        if (patternEvaluated) {
            patternEvaluated = false;
            Serial.printf("ℹ️ Cards removed (%d/4 present). Ready for next evaluation.\n", totalDetected);
        }
    }

    // Update LCD if state changed and we are in active play (auto-start on first placement if in READY)
    if (cardStateChanged && (currentState == READY || currentState == STARTED)) {
        if (currentState == READY && totalDetected > 0) {
            currentState = STARTED;
            publishState(STARTED);
            publishEvent("STARTED");
        }
        showPuzzleStatus();
    }

    // CRITICAL REQUIREMENT:
    // Check pattern ONLY when ALL 4 RFIDs have detected a card!
    if (totalDetected == 4 && (currentState == READY || currentState == STARTED)) {
        if (!patternEvaluated) {
            evaluatePattern();
        }
    }
}

void evaluatePattern() {
    patternEvaluated = true; // Latch: ensures audio feedback triggers ONLY ONCE per placement

    Serial.println();
    Serial.println("==================================================");
    Serial.println("🔍 ALL 4 CARDS DETECTED! EVALUATING PATTERN...");
    Serial.println("==================================================");

    bool patternMatches = true;

    for (byte i = 0; i < NUM_READERS; i++) {
        Serial.printf("  Slot %d [%s]\n", i + 1, readerNames[i]);
        Serial.print("    -> Detected UID: ");
        printUIDInline(detectedUID[i], detectedUIDSize[i]);
        Serial.print(" | Expected: ");
        printUIDInline(expectedUIDs[i], EXPECTED_UID_SIZE);

        if (uidMatches(detectedUID[i], detectedUIDSize[i], expectedUIDs[i], EXPECTED_UID_SIZE)) {
            Serial.println("  ✅ [MATCH]");
        } else {
            Serial.println("  ❌ [WRONG CARD]");
            patternMatches = false;
        }
    }
    Serial.println("--------------------------------------------------");

    if (patternMatches) {
        handleSuccess();
    } else {
        handleFailure();
    }
}

void handleSuccess() {
    puzzleSolved = true;
    currentState = COMPLETED;

    Serial.println("🎉🎉🎉 ACCESS GRANTED! ALL 4 DOLLS IN CORRECT PATTERN! 🎉🎉🎉");

    unlockDoor(); // Actuate relay & move servo to 180°

    // Publish COMPLETED -> Control Server plays victory audio
    publishState(COMPLETED);
    publishEvent("COMPLETED");

    triggerLocalBuzzer(true);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUZZLE SOLVED!");
    lcd.setCursor(0, 1);
    lcd.print("DOOR UNLOCKED");
}

void handleFailure() {
    currentState = FAILED;

    Serial.println("❌❌❌ WRONG PATTERN! 4 cards placed, but sequence is incorrect. ❌❌❌");

    // Publish FAILED -> Control Server plays audio/<game_id>/failed.mp3 or failed.ogg
    publishState(FAILED);
    publishEvent("FAILED");

    triggerLocalBuzzer(false);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WRONG PATTERN!");
    lcd.setCursor(0, 1);
    lcd.print("TRY AGAIN...");

    // Non-blocking timer to return to STARTED state (2 seconds)
    stateTransitionTime = millis() + 2000;
}

// =================================================================================
// 8. LCD DISPLAY HANDLERS
// =================================================================================

void updateLcdDisplay() {
    lcd.clear();
    switch (currentState) {
        case READY:
            lcd.setCursor(0, 0);
            lcd.print("DOLLS PUZZLE    ");
            lcd.setCursor(0, 1);
            lcd.print("READY TO PLAY   ");
            break;

        case STARTED:
            showPuzzleStatus();
            break;

        case FAILED:
            lcd.setCursor(0, 0);
            lcd.print("WRONG PATTERN!  ");
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

void showPuzzleStatus() {
    // Line 0: "1:O 2:. 3:O 4:." -> 15 chars (O = Card Present, . = Empty Slot)
    lcd.setCursor(0, 0);
    char line0[17];
    snprintf(line0, sizeof(line0), "1:%c 2:%c 3:%c 4:%c  ",
             cardPresent[0] ? 'O' : '.',
             cardPresent[1] ? 'O' : '.',
             cardPresent[2] ? 'O' : '.',
             cardPresent[3] ? 'O' : '.');
    lcd.print(line0);

    // Line 1: Real-time placement count
    lcd.setCursor(0, 1);
    int count = getCardsPresentCount();
    if (count == 4) {
        lcd.print("CHECKING PATTERN");
    } else {
        char line1[17];
        snprintf(line1, sizeof(line1), "Placed: %d/4 Dolls", count);
        lcd.print(line1);
    }
}

// =================================================================================
// 9. MQTT & REMOTE COMMANDS
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
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = STARTED;
        lockDoor();
        publishState(STARTED);
        publishEvent("STARTED");

        // CRITICAL: Flush all cached cards and do a clean fresh scan of current physical reality
        for (byte i = 0; i < NUM_READERS; i++) {
            consecutiveMisses[i] = 0;
            detectedUIDSize[i] = 0;
            memset(detectedUID[i], 0, sizeof(detectedUID[i]));
            cardPresent[i] = scanReader(i);
        }

        int total = getCardsPresentCount();
        Serial.printf("🔄 [START/RESTART] State reset. Present physical dolls: %d/4\n", total);
        if (total < 4) {
            patternEvaluated = false;
        } else {
            evaluatePattern();
        }
        updateLcdDisplay();
    }
    else if (cmd.equalsIgnoreCase("STOP")) {
        currentState = STOPPED;
        stateTransitionTime = 0;
        lockDoor();
        publishState(STOPPED);
        publishEvent("STOPPED");
        updateLcdDisplay();
    }
    else if (cmd.equalsIgnoreCase("RESET")) {
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = READY;
        attemptNumber = 0;
        lockDoor();
        publishState(READY);
        publishEvent("RESET");

        // Flush all cached cards and do a clean fresh scan
        for (byte i = 0; i < NUM_READERS; i++) {
            consecutiveMisses[i] = 0;
            detectedUIDSize[i] = 0;
            memset(detectedUID[i], 0, sizeof(detectedUID[i]));
            cardPresent[i] = scanReader(i);
        }
        updateLcdDisplay();
    }
    else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
        Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
        handleSuccess();
    }
}

// =================================================================================
// 10. NETWORK & MQTT COMMUNICATIONS (With Automatic mDNS Discovery)
// =================================================================================

bool discoverMQTTServer() {
    Serial.println("\n🔍 [mDNS] Discovering Escape Room Control Server...");

    if (!MDNS.begin("ESP32-RFIDDolls")) {
        Serial.println("⚠️ [mDNS] Responder init failed, proceeding with queries...");
    } else {
        Serial.println("📡 [mDNS] Responder active ('ESP32-RFIDDolls.local')");
    }

    // 1. Try DNS-SD Service Discovery (Discovers both IP AND active Port automatically!)
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
    while (WiFi.status() != WL_CONNECTED && retries < 25) {
        delay(300);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Wi-Fi Connected!");
        Serial.print("   Prop IP Address: ");
        Serial.println(WiFi.localIP());

        discoverMQTTServer();
    } else {
        Serial.println("\n⚠️ Wi-Fi Timeout. Continuing in standalone/offline mode.");
    }
}

void maintainWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        unsigned long now = millis();
        if (now - lastWiFiRetry > 10000) {
            lastWiFiRetry = now;
            Serial.println("📶 [Wi-Fi] Connection lost. Attempting auto-reconnect...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        }
    }
}

void maintainMQTT() {
    maintainWiFi();

    if (WiFi.status() != WL_CONNECTED) return;

    if (mqtt.connected()) {
        mqtt.loop();
        return;
    }

    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) {
        lastMqttRetry = now;
        Serial.printf("🔌 Connecting to MQTT Broker at %s:%d...\n", 
                      activeMqttIP.toString().c_str(), activeMqttPort);

        String clientId = "ESP32-RFID-" + String(GAME_ID);

        // Connect with Last Will & Testament (LWT)
        if (mqtt.connect(clientId.c_str(), topicStatus, 1, true, "offline")) {
            Serial.println("✅ Connected to MQTT Broker!");
            mqttFailCount = 0;
            publishStatus("online");
            mqtt.subscribe(topicCmd);
            publishState(currentState);
        } else {
            Serial.printf("⚠️ MQTT Failed (rc=%d). Retrying in 5 seconds...\n", mqtt.state());
            mqttFailCount++;

            // If failed 3 times, re-run mDNS discovery
            if (mqttFailCount >= 3) {
                Serial.println("🔄 Re-checking mDNS in case server IP or network changed...");
                discoverMQTTServer();
                mqttFailCount = 0;
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

// =================================================================================
// 11. SETUP
// =================================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==================================================");
    Serial.println("   ESCAPE ROOM: 4-RFID DOLLS PUZZLE INITIALIZING  ");
    Serial.println("==================================================");

    // 1. Initialize Relay, Servo & Reset Button
    pinMode(RELAY_PIN, OUTPUT);

    // Initialize Servo Motor (ESP32 PWM)
    if (SERVO_PIN >= 0) {
        ESP32PWM::allocateTimer(0);
        ESP32PWM::allocateTimer(1);
        ESP32PWM::allocateTimer(2);
        ESP32PWM::allocateTimer(3);
        puzzleServo.setPeriodHertz(50);             // Standard 50Hz servo
        puzzleServo.attach(SERVO_PIN, 500, 2400);   // Standard 500us-2400us pulses for 0-180°
    }

    lockDoor(); // Initializes Relay to locked & Servo to 0°

    pinMode(RESET_BTN_PIN, INPUT_PULLUP);

    if (BUZZER_PIN >= 0) {
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
    }

    // 2. Initialize I2C Bus & LCD Display with bus timeout protection
    Wire.begin(LCD_SDA, LCD_SCL);
    Wire.setTimeOut(100); // 100ms timeout prevents I2C bus hanging on inductive spike noise
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("DOLLS PUZZLE");
    lcd.setCursor(0, 1);
    lcd.print("BOOTING...");

    // 3. Initialize SPI Bus
    SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI);

    // 4. Initialize RFID SS Pins
    for (byte i = 0; i < NUM_READERS; i++) {
        pinMode(SS_PINS[i], OUTPUT);
    }
    deselectAllReaders();

    // 5. Initialize each MFRC522 Reader individually
    Serial.println("\nInitializing 4 RFID Readers...");
    for (byte i = 0; i < NUM_READERS; i++) {
        deselectAllReaders();
        digitalWrite(SS_PINS[i], LOW);
        delay(10);
        readers[i].PCD_Init();
        delay(10);
        readers[i].PCD_SetAntennaGain(MFRC522::RxGain_max); // Maximize antenna gain for prop housing
        delay(5);
        digitalWrite(SS_PINS[i], HIGH);
        delay(5);

        Serial.printf("  Reader %d [%s] ready on SS Pin %d\n", i + 1, readerNames[i], SS_PINS[i]);
    }
    Serial.println("All 4 RFID readers initialized successfully.\n");

    // 6. Build MQTT Topics
    snprintf(topicStatus, sizeof(topicStatus), "%s/%s/status", ROOT_TOPIC, GAME_ID);
    snprintf(topicState,  sizeof(topicState),  "%s/%s/state",  ROOT_TOPIC, GAME_ID);
    snprintf(topicEvent,  sizeof(topicEvent),  "%s/%s/event",  ROOT_TOPIC, GAME_ID);
    snprintf(topicCmd,    sizeof(topicCmd),    "%s/%s/cmd",    ROOT_TOPIC, GAME_ID);

    // Expand PubSubClient buffer to 512 bytes & set keepalive to 15s for prompt disconnect detection
    mqtt.setBufferSize(512);
    mqtt.setKeepAlive(15);
    mqtt.setSocketTimeout(15);

    // 7. Connect Network & MQTT
    setupWiFi();
    mqtt.setCallback(mqttCallback);

    // 8. Auto-Start if configured
    if (AUTO_START_ON_BOOT) {
        attemptNumber = 1;
        currentState = STARTED;
        publishState(STARTED);
        publishEvent("STARTED");
        Serial.println("⚡ Puzzle auto-started! Place all 4 dolls to evaluate.");
    } else {
        currentState = READY;
        publishState(READY);
        publishEvent("READY");
    }

    updateLcdDisplay();
}

// =================================================================================
// 12. MAIN LOOP (100% Non-Blocking)
// =================================================================================

void loop() {
    unsigned long now = millis();

    // 1. Maintain Network & MQTT
    maintainMQTT();

    // 2. Periodic Heartbeat to Server
    if (now - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = now;
        publishEvent("HEARTBEAT");
    }

    // 3. Non-Blocking Physical Reset Button Check (Glitch-Free Debounce)
    bool currentBtnState = digitalRead(RESET_BTN_PIN);
    if (currentBtnState == LOW && lastButtonState == HIGH) {
        if (now - lastButtonPressTime > 250) {
            lastButtonPressTime = now;
            Serial.println("🔘 Hardware Reset Pressed");
            handleCommand("RESET");
        }
    }
    lastButtonState = currentBtnState;

    // 4. Non-Blocking State Transitions (Replaces blocking delays)
    if (stateTransitionTime > 0 && now >= stateTransitionTime) {
        stateTransitionTime = 0;
        if (currentState == FAILED) {
            currentState = STARTED;
            publishState(STARTED);
            updateLcdDisplay();
        }
    }

    // 5. Scan RFIDs and evaluate pattern
    // Runs in all states to maintain real-time hardware status
    checkAllCards();

    // 6. Auto-Relock Timer Check (for Solenoid locks)
    if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
        if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
            Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
            lockDoor();
        }
    }
}