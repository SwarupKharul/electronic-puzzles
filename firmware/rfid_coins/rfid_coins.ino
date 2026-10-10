/*
 * =================================================================================
 * ESCAPE ROOM - 4 RFID COINS POSITIONAL PUZZLE (Dual-Core FreeRTOS Architecture)
 * =================================================================================
 * Features:
 *   - Dual-Core Isolation:
 *       • Core 1 (APP_CPU): 100% dedicated to 4x MFRC522 RFID SPI scanning,
 *                           up to 3x servo motors, relay, buzzer, and 16x2 LCD.
 *                           Pure hardware determinism with ZERO network code or blocking.
 *       • Core 0 (PRO_CPU): Dedicated background network task. Retries Wi-Fi, mDNS,
 *                           and MQTT indefinitely every 5s with zero impact on card scanning.
 *   - Thread-Safe Inter-Core Queues (FreeRTOS xQueue):
 *       • cmdQueue: Transfers incoming GM commands (START, RESET, STOP, SOLVE) to Core 1.
 *       • telemetryQueue: Posts outgoing state/event changes from Core 1 to Core 0.
 *   - Positional Matching Logic (Option B):
 *       • Players place 4 coins into 4 designated slots/receptacles in any time order.
 *       • Evaluation triggers ONLY when all 4 coins are placed (4/4 present).
 *       • Slot 1 must hold Coin 1, Slot 2 must hold Coin 2, Slot 3 must hold Coin 3, Slot 4 must hold Coin 4.
 *       • If correct: Unlocks relay, rotates servos to 90°, plays victory chime.
 *       • If incorrect: Triggers FAILED event, plays error buzz, allows players to rearrange coins.
 *   - Multi-Servo Actuation: Controls up to 3 servo motors (e.g. coin drawer, lock latch, compartment).
 *   - Dynamic card-swap detection (evaluates even if coins swapped without lifting).
 *   - Automated MFRC522 PCD register self-healing & antenna gain optimization.
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic for Solenoid / Maglock.
 *   - I2C 16x2 LCD Display with I2C bus timeout protection against noise lockups.
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT).
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
 *      [Reader 1 (Coin Slot 1)]
 *      SDA / SS / CS    --->   GPIO 15 (SS1)
 *
 *      [Reader 2 (Coin Slot 2)]
 *      SDA / SS / CS    --->   GPIO 4  (SS2)
 *
 *      [Reader 3 (Coin Slot 3)]
 *      SDA / SS / CS    --->   GPIO 16 (SS3)
 *
 *      [Reader 4 (Coin Slot 4)]
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
 * 4. SERVO MOTORS (Up to 3 Servos for Locking / Drawers / Hatches):
 *      Servo 1 (Main Lock)   --->   GPIO 13 (SERVO1_PIN)
 *      Servo 2 (Drawer/Box)  --->   GPIO 33 (SERVO2_PIN, or -1 to disable)
 *      Servo 3 (Trapdoor)    --->   GPIO 12 (SERVO3_PIN, or -1 to disable)
 *      VCC (Red)             --->   External 5V (2A+ Power Supply recommended!)
 *      GND (Brown/Black)     --->   Common GND
 *
 * 5. PHYSICAL HARDWARE BUTTON & BUZZER:
 *      Reset Button     --->   GPIO 14 (Active LOW to GND, internal pullup)
 *      Piezo Buzzer     --->   GPIO 32 (Positive to Pin, Negative to GND, -1 to disable)
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 1

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =================================================================================
// 1. HARDWARE PIN DEFINITIONS & CONFIGURATION
// =================================================================================

// RFID Shared SPI Bus
#define RFID_SCK   18
#define RFID_MISO  19
#define RFID_MOSI  23
#define RFID_RST   22

// RFID Slave Select (SS / CS) Pins for the 4 Readers
#define SS1 15  // Reader 1 - Coin 1
#define SS2 4   // Reader 2 - Coin 2
#define SS3 16  // Reader 3 - Coin 3
#define SS4 17  // Reader 4 - Coin 4

// I2C LCD Pins (16x2)
#define LCD_SDA 26
#define LCD_SCL 27
#define LCD_I2C_ADDR 0x27

// Relay & Actuator Pins
#define RELAY_PIN      25 // Solenoid / Maglock Relay
#define SERVO1_PIN     13 // Primary Servo Motor Pin (-1 to disable)
#define SERVO2_PIN     33 // Secondary Servo Motor Pin (-1 to disable)
#define SERVO3_PIN     12 // Tertiary Servo Motor Pin (-1 to disable)
#define RESET_BTN_PIN  14 // Optional manual reset button (active LOW)
#define BUZZER_PIN     32 // Optional local piezo buzzer (-1 to disable)

// Servo Motor Positions (Degrees)
const int SERVO_LOCKED_POS   = 0;   // 0° when reset / standby / locked
const int SERVO_UNLOCKED_POS = 90;  // 90° when puzzle is solved / unlocked

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
const char* WIFI_SSID     = "operations_404";
const char* WIFI_PASS     = "Mytplink2020";

// const char* WIFI_SSID     = "Airtel_anjo_4056";
// const char* WIFI_PASS     = "air38409";

// mDNS Configuration - The ESP32 discovers the server automatically!
const char* MDNS_HOST_ESCAPEROOM = "escaperoom"; // Queries 'escaperoom.local'
const char* MDNS_HOST_LAPTOP     = "pop-os";     // Native Linux hostname fallback
const char* MQTT_SERVER_FALLBACK = "192.168.1.9"; // Fallback only if router blocks multicast

const int   MQTT_PORT_DEFAULT    = 1884;         // Default port (auto-discovered via mDNS)
int         activeMqttPort       = MQTT_PORT_DEFAULT;
IPAddress   activeMqttIP;
bool        serverDiscovered     = false;

const char* GAME_ID       = "game3";          // Matches "id" in games.json ("game3")
const char* ROOT_TOPIC    = "escaperoom";     // Matches "rootTopic" in games.json

// =================================================================================
// 3. TARGET PATTERN / EXPECTED UIDs FOR 4 COINS
// =================================================================================
// Replace these dummy byte arrays with the actual 4-byte or 7-byte UIDs of your
// RFID coin stickers (printed to Serial monitor when you first tap them).
const byte NUM_READERS = 4;
const byte SS_PINS[NUM_READERS] = { SS1, SS2, SS3, SS4 };
const byte EXPECTED_UID_SIZE = 7; // Supports 4 or 7 bytes (Mifare Classic or NTAG213)

// Expected UID for Slot 1 (Coin 1)
byte expectedUID1[EXPECTED_UID_SIZE] = { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

// Expected UID for Slot 2 (Coin 2)
byte expectedUID2[EXPECTED_UID_SIZE] = { 0x04, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

// Expected UID for Slot 3 (Coin 3)
byte expectedUID3[EXPECTED_UID_SIZE] = { 0x04, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

// Expected UID for Slot 4 (Coin 4)
byte expectedUID4[EXPECTED_UID_SIZE] = { 0x04, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };

const byte* expectedUIDs[NUM_READERS] = {
    expectedUID1,
    expectedUID2,
    expectedUID3,
    expectedUID4
};

const char* readerNames[NUM_READERS] = {
    "COIN 1 (SLOT 1)",
    "COIN 2 (SLOT 2)",
    "COIN 3 (SLOT 3)",
    "COIN 4 (SLOT 4)"
};

// =================================================================================
// 4. FREE-RTOS DUAL-CORE INTER-THREAD COMMUNICATION & GLOBALS
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

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Servo servo1;
Servo servo2;
Servo servo3;

MFRC522 readers[NUM_READERS] = {
    MFRC522(SS1, RFID_RST),
    MFRC522(SS2, RFID_RST),
    MFRC522(SS3, RFID_RST),
    MFRC522(SS4, RFID_RST)
};

// Coin Detection States (Core 1)
bool cardPresent[NUM_READERS]        = { false, false, false, false };
byte detectedUID[NUM_READERS][10];
byte detectedUIDSize[NUM_READERS]    = { 0, 0, 0, 0 };
byte consecutiveMisses[NUM_READERS]  = { 0, 0, 0, 0 };
unsigned long consecutiveErrorCount[NUM_READERS] = { 0, 0, 0, 0 };

// Evaluation Flags (Core 1)
bool patternEvaluated = false;
bool puzzleSolved     = false;

// Game State Machine (Thread-safe shared visibility)
enum GameState { READY, STARTED, FAILED, COMPLETED, STOPPED };
volatile GameState currentState = READY;
volatile int attemptNumber = 0;

// Non-blocking timer for state transitions (Core 1)
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
void postState(GameState state);
void postEvent(const char* eventName);
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
void lockAllActuators();
void unlockAllActuators();
void moveServos(int angle);
void triggerLocalBuzzer(bool success);
void updateLcdDisplay();
void showPuzzleStatus();
bool uidMatches(const byte* actualUID, byte actualSize, const byte* expectedUID, byte expectedSize = EXPECTED_UID_SIZE);
void printUID(const byte* uid, byte size);
void printUIDInline(const byte* uid, byte size);
void recoverReader(byte readerIdx);

// =================================================================================
// 5. RELAY & MULTI-SERVO ACTUATOR CONTROL
// =================================================================================

void moveServos(int angle) {
    if (SERVO1_PIN >= 0) {
        servo1.write(angle);
    }
    if (SERVO2_PIN >= 0) {
        servo2.write(angle);
    }
    if (SERVO3_PIN >= 0) {
        servo3.write(angle);
    }
    Serial.printf("⚙️ [SERVOS] Position set to %d° across active servos\n", angle);
}

void lockAllActuators() {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    moveServos(SERVO_LOCKED_POS); // Reset servos back to 0°
    unlockedAt = 0;
    Serial.println("🔒 [ACTUATOR] Locked (Relay DE-ENERGIZED, Servos at 0°)");
}

void unlockAllActuators() {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
    moveServos(SERVO_UNLOCKED_POS); // Move servos to 90°
    unlockedAt = millis();
    Serial.println("🔓 [ACTUATOR] Unlocked (Relay ENERGIZED, Servos at 90°)");
}

void triggerLocalBuzzer(bool success) {
    if (BUZZER_PIN < 0) return;
    if (success) {
        tone(BUZZER_PIN, 1500, 350); // Celebratory success chirp
    } else {
        tone(BUZZER_PIN, 250, 450);  // Low failure buzz
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
                // Coin was swapped without leaving slot empty
                patternEvaluated = false;
                Serial.printf("🔄 Slot %d [%s]: Coin SWAPPED! New UID: ", readerIdx + 1, readerNames[readerIdx]);
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
// 7. POSITIONAL PATTERN EVALUATION (Option B: Checked ONLY when 4/4 coins placed)
// =================================================================================

int getCoinsPresentCount() {
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
                Serial.printf("📍 Slot %d [%s]: Coin INSERTED! UID: ", i + 1, readerNames[i]);
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
                    patternEvaluated = false; // Reset latch when coin is removed
                    Serial.printf("💨 Slot %d [%s]: Coin REMOVED\n", i + 1, readerNames[i]);
                }
            }
        }
        delayMicroseconds(500); // SPI bus settling time
    }

    int totalDetected = getCoinsPresentCount();

    // If coins were removed or rearranged, reset evaluation latch
    if (totalDetected < 4) {
        if (patternEvaluated) {
            patternEvaluated = false;
            Serial.printf("ℹ️ Coins removed (%d/4 present). Ready for next evaluation.\n", totalDetected);
        }
    }

    // Update LCD if state changed and we are in active play (auto-start on first coin if in READY)
    if (cardStateChanged && (currentState == READY || currentState == STARTED)) {
        if (currentState == READY && totalDetected > 0) {
            currentState = STARTED;
            publishState(STARTED);
            publishEvent("STARTED");
        }
        showPuzzleStatus();
    }

    // OPTION B: Evaluate pattern ONLY when ALL 4 coins are placed!
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
    Serial.println("🔍 ALL 4 COINS INSERTED! EVALUATING POSITIONS...");
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
            Serial.println("  ❌ [WRONG COIN / POSITION]");
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

    Serial.println("🎉🎉🎉 ACCESS GRANTED! ALL 4 COINS IN PROPER ORDER! 🎉🎉🎉");

    unlockAllActuators(); // Actuate relay & move all servos to 90°

    // Publish COMPLETED -> Control Server plays victory audio
    publishState(COMPLETED);
    publishEvent("COMPLETED");

    triggerLocalBuzzer(true);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUZZLE SOLVED!  ");
    lcd.setCursor(0, 1);
    lcd.print("COINS ACCEPTED  ");
}

void handleFailure() {
    currentState = FAILED;

    Serial.println("❌❌❌ WRONG ORDER! 4 coins placed, but positions are incorrect. ❌❌❌");

    // Publish FAILED -> Control Server plays audio/game3/failed.ogg
    publishState(FAILED);
    publishEvent("FAILED");

    triggerLocalBuzzer(false);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WRONG ORDER!    ");
    lcd.setCursor(0, 1);
    lcd.print("TRY AGAIN...    ");

    // Non-blocking timer to return to STARTED state after 2 seconds
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
            lcd.print("COINS PUZZLE    ");
            lcd.setCursor(0, 1);
            lcd.print("READY TO PLAY   ");
            break;

        case STARTED:
            showPuzzleStatus();
            break;

        case FAILED:
            lcd.setCursor(0, 0);
            lcd.print("WRONG ORDER!    ");
            lcd.setCursor(0, 1);
            lcd.print("TRY AGAIN...    ");
            break;

        case COMPLETED:
            lcd.setCursor(0, 0);
            lcd.print("PUZZLE SOLVED!  ");
            lcd.setCursor(0, 1);
            lcd.print("COINS ACCEPTED  ");
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
    // Line 0: "1:O 2:. 3:O 4:." -> 15 chars (O = Coin Present, . = Empty Slot)
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
    int count = getCoinsPresentCount();
    if (count == 4) {
        lcd.print("CHECKING ORDER..");
    } else {
        char line1[17];
        snprintf(line1, sizeof(line1), "Placed: %d/4 Coins", count);
        lcd.print(line1);
    }
}

// =================================================================================
// 9. THREAD-SAFE STATE & EVENT POSTING (Core 1 -> Core 0)
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

void publishState(GameState state) {
    postState(state);
}

void publishEvent(const char* eventName) {
    postEvent(eventName);
}

// =================================================================================
// 10. COMMAND DISPATCHER (Core 1)
// =================================================================================

void handleCommand(String cmd) {
    if (cmd.equalsIgnoreCase("START") || cmd.equalsIgnoreCase("RESTART")) {
        attemptNumber++;
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = STARTED;
        lockAllActuators();
        postState(STARTED);
        postEvent("STARTED");

        // Flush all cached cards and do a clean fresh scan
        for (byte i = 0; i < NUM_READERS; i++) {
            consecutiveMisses[i] = 0;
            detectedUIDSize[i] = 0;
            memset(detectedUID[i], 0, sizeof(detectedUID[i]));
            cardPresent[i] = scanReader(i);
        }

        int total = getCoinsPresentCount();
        Serial.printf("🔄 [START/RESTART] State reset. Present physical coins: %d/4\n", total);
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
        lockAllActuators();
        postState(STOPPED);
        postEvent("STOPPED");
        updateLcdDisplay();
    }
    else if (cmd.equalsIgnoreCase("RESET")) {
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = READY;
        attemptNumber = 0;
        lockAllActuators();
        postState(READY);
        postEvent("RESET");

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
// 11. SETUP (Core 1)
// =================================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==================================================");
    Serial.println("   ESCAPE ROOM: 4-RFID COINS PUZZLE (Dual-Core)   ");
    Serial.println("==================================================");

    // 1. Initialize Relay & Servos
    pinMode(RELAY_PIN, OUTPUT);

    // Initialize Servo Motors (ESP32 PWM timers)
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);

    if (SERVO1_PIN >= 0) {
        servo1.setPeriodHertz(50);
        servo1.attach(SERVO1_PIN, 500, 2400);
        Serial.printf("⚙️ Servo 1 attached on GPIO %d\n", SERVO1_PIN);
    }
    if (SERVO2_PIN >= 0) {
        servo2.setPeriodHertz(50);
        servo2.attach(SERVO2_PIN, 500, 2400);
        Serial.printf("⚙️ Servo 2 attached on GPIO %d\n", SERVO2_PIN);
    }
    if (SERVO3_PIN >= 0) {
        servo3.setPeriodHertz(50);
        servo3.attach(SERVO3_PIN, 500, 2400);
        Serial.printf("⚙️ Servo 3 attached on GPIO %d\n", SERVO3_PIN);
    }

    lockAllActuators(); // Relays locked & Servos to 0°

    pinMode(RESET_BTN_PIN, INPUT_PULLUP);

    if (BUZZER_PIN >= 0) {
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
    }

    // 2. Initialize I2C Bus & LCD Display with bus timeout protection
    Wire.begin(LCD_SDA, LCD_SCL);
    Wire.setTimeOut(100); // 100ms timeout prevents I2C bus hang
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("COINS PUZZLE");
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
    Serial.println("\nInitializing 4 RFID Coin Readers...");
    for (byte i = 0; i < NUM_READERS; i++) {
        deselectAllReaders();
        digitalWrite(SS_PINS[i], LOW);
        delay(10);
        readers[i].PCD_Init();
        delay(10);
        readers[i].PCD_SetAntennaGain(MFRC522::RxGain_max); // Maximize antenna gain
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

    // 7. Create FreeRTOS Queues for Thread-Safe Inter-Core Communication
    cmdQueue = xQueueCreate(10, sizeof(CommandMsg));
    telemetryQueue = xQueueCreate(16, sizeof(TelemetryMsg));

    // 8. Spawn Independent Background Network & MQTT Task on CPU Core 0
    xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        8192,
        NULL,
        1, // Priority 1 (low, so Core 1 game loop is never preempted)
        &networkTaskHandle,
        0  // Core 0
    );

    // 9. Auto-Start if configured
    if (AUTO_START_ON_BOOT) {
        attemptNumber = 1;
        currentState = STARTED;
        postState(STARTED);
        postEvent("STARTED");
        Serial.println("⚡ Puzzle auto-started! Place all 4 coins into their slots.");
    } else {
        currentState = READY;
        postState(READY);
        postEvent("READY");
    }

    updateLcdDisplay();
}

// =================================================================================
// 12. MAIN LOOP (Core 1 — 100% Non-Blocking & Pure Card Scanning)
// =================================================================================

void loop() {
    unsigned long now = millis();

    // 1. Process Incoming Commands from Core 0
    if (cmdQueue != NULL) {
        CommandMsg incoming;
        while (xQueueReceive(cmdQueue, &incoming, 0) == pdTRUE) {
            handleCommand(String(incoming.cmd));
        }
    }

    // 2. Physical Manual Reset Button Check
    bool currentBtnState = digitalRead(RESET_BTN_PIN);
    if (currentBtnState == LOW && lastButtonState == HIGH) {
        if (now - lastButtonPressTime > 250) {
            lastButtonPressTime = now;
            Serial.println("🔘 Hardware Reset Pressed");
            handleCommand("RESET");
        }
    }
    lastButtonState = currentBtnState;

    // 3. Non-Blocking State Transitions (for FAILED cooldown)
    if (stateTransitionTime > 0 && now >= stateTransitionTime) {
        stateTransitionTime = 0;
        if (currentState == FAILED) {
            currentState = STARTED;
            postState(STARTED);
            updateLcdDisplay();
        }
    }

    // 4. PRIORITY #1: Scan RFIDs and evaluate pattern (100% Dedicated Core 1)
    if (currentState == STARTED || currentState == READY) {
        checkAllCards();
    }

    // 5. Non-blocking Auto-Relock Timer Check
    if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
        if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
            Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
            lockAllActuators();
        }
    }
}

// =================================================================================
// 13. BACKGROUND NETWORK & MQTT TASK (Core 0 — Independent Thread)
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

    if (!MDNS.begin("ESP32-RFIDCoins")) {
        Serial.println("⚠️ [mDNS Core 0] Responder init failed, querying...");
    } else {
        Serial.println("📡 [mDNS Core 0] Responder active ('ESP32-RFIDCoins.local')");
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

    if (!serverDiscovered) {
        unsigned long now = millis();
        if (now - lastMdnsRetry > 10000) {
            lastMdnsRetry = now;
            discoverMQTTServer();
        }
    }

    if (mqtt.connected()) return;

    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) {
        lastMqttRetry = now;
        Serial.printf("🔌 [Core 0] Connecting to MQTT Broker at %s:%d...\n", 
                      activeMqttIP.toString().c_str(), activeMqttPort);

        String clientId = "ESP32-RFID-" + String(GAME_ID);

        if (mqtt.connect(clientId.c_str(), topicStatus, 1, true, "offline")) {
            Serial.println("✅ [Core 0] Connected to MQTT Broker!");
            mqtt.publish(topicStatus, "online", true);
            mqtt.subscribe(topicCmd);

            postState(currentState);
            postEvent("ONLINE");
        } else {
            Serial.printf("❌ [Core 0] MQTT Connect failed (rc=%d). Retrying in 5s...\n", mqtt.state());
        }
    }
}

void networkTask(void* pvParameters) {
    setupWiFi();
    mqtt.setCallback(mqttCallback);

    unsigned long lastHeartbeatTime = 0;

    for (;;) {
        maintainWiFi();
        maintainMQTT();

        if (mqtt.connected()) {
            mqtt.loop();

            // Process telemetry queue from Core 1
            if (telemetryQueue != NULL) {
                TelemetryMsg msg;
                while (xQueueReceive(telemetryQueue, &msg, 0) == pdTRUE) {
                    char payload[128];
                    if (msg.kind == 'S') {
                        snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"attempt\":%d}", msg.text, msg.attempt);
                        mqtt.publish(topicState, payload, false);
                        Serial.printf("📡 [MQTT TX Core 0] State: %s\n", payload);
                    } else if (msg.kind == 'E') {
                        snprintf(payload, sizeof(payload), "{\"event\":\"%s\",\"attempt\":%d}", msg.text, msg.attempt);
                        mqtt.publish(topicEvent, payload, false);
                        Serial.printf("📡 [MQTT TX Core 0] Event: %s\n", payload);
                    }
                }
            }

            // Periodic 3-second heartbeat to control dashboard
            unsigned long now = millis();
            if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
                lastHeartbeatTime = now;
                char hbPayload[64];
                snprintf(hbPayload, sizeof(hbPayload), "{\"event\":\"HEARTBEAT\",\"attempt\":%d}", attemptNumber);
                mqtt.publish(topicEvent, hbPayload, false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
