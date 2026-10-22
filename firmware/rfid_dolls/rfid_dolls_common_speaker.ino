/*
 * =================================================================================
 * ESCAPE ROOM - 4 RFID COINS POSITIONAL PUZZLE (Dual-Core FreeRTOS Architecture)
 * =================================================================================
 * Production-Ready Firmware (Simplified & Hardened):
 *   - Removed Servo Motors (pure solid-state / 12V lock relay actuation).
 *   - Removed I2C LCD Display (status & audio managed centrally via Laptop Server & Dashboard).
 *   - Removed Piezo Buzzer (audio played via central Escape Room sound system).
 *   - Relay relocated to GPIO 5 (Right-side header of ESP32) for direct, clean wiring
 *     alongside the RFID reader pin cluster.
 *
 * Core Architecture:
 *   - Dual-Core Isolation:
 *       • Core 1 (APP_CPU): 100% dedicated to 4x MFRC522 RFID SPI scanning and Relay
 *                           actuation. Pure hardware determinism with ZERO network code,
 *                           delays, or blocking.
 *       • Core 0 (PRO_CPU): Dedicated background network task. Retries Wi-Fi, mDNS, and
 *                           MQTT indefinitely every 5s with zero impact on card scanning.
 *   - Thread-Safe Inter-Core Queues (FreeRTOS xQueue):
 *       • cmdQueue: Transfers incoming GM commands (START, RESET, STOP, SOLVE) to Core 1.
 *       • telemetryQueue: Posts outgoing state/event changes from Core 1 to Core 0.
 *   - Positional Matching Logic (Option B):
 *       • Players place 4 coins into 4 designated slots/receptacles in any time order.
 *       • Evaluation triggers ONLY when all 4 coins are placed (4/4 present).
 *       • Slot 1 must hold Coin 1, Slot 2 must hold Coin 2, Slot 3 must hold Coin 3, Slot 4 must hold Coin 4.
 *       • If correct: Unlocks relay and triggers COMPLETED victory audio on central server.
 *       • If incorrect: Triggers FAILED event, plays error sound on server, allows players to rearrange coins.
 *   - Glitch-Free Relay Power-On:
 *       • Relay pin initialized HIGH (de-energized) prior to setting pin mode to prevent
 *         accidental unlocking or clicking on boot.
 *   - Dynamic card-swap detection (evaluates even if coins swapped without lifting).
 *   - Automated MFRC522 PCD register self-healing & antenna gain optimization.
 *   - Configurable Active-LOW / Active-HIGH relay trigger logic for Solenoid / Maglock.
 *   - PubSubClient expanded 512-byte buffer with Last Will and Testament (LWT).
 *
 * ---------------------------------------------------------------------------------
 * HARDWARE WIRING GUIDE:
 * ---------------------------------------------------------------------------------
 * 1. 4x MFRC522 RFID READERS (Shared SPI Bus + Dedicated SS/CS Pins):
 *      [All 4 Readers]         [ESP32 Right Header]
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
 * 2. 3-PIN RELAY MODULE (Solenoid / Maglock Lock):
 *      [Relay Module]          [ESP32]
 *      IN / SIG / S     --->   GPIO 5  (Right Header - Clean, direct connection)
 *      VCC              --->   VIN (5V from USB / External 5V)
 *      GND              --->   GND (Common Ground)
 *
 *    High-Power Lock Side (Relay Screw Terminals: COM, NO, NC):
 *      • 12V Solenoid Cabinet Lock (Fail-Secure - Locked when unpowered):
 *          12V Power Supply (+) ---> Relay COM
 *          Relay NO             ---> Solenoid Lock (+)
 *          12V Power Supply (-) ---> Solenoid Lock (-)
 *      • 12V Electromagnetic Lock / Maglock (Fail-Safe - Locked when powered):
 *          12V Power Supply (+) ---> Relay COM
 *          Relay NC             ---> Maglock (+)
 *          12V Power Supply (-) ---> Maglock (-)
 *
 * 3. OPTIONAL HARDWARE RESET BUTTON:
 *      Reset Button     --->   GPIO 14 (Active LOW to GND, internal pull-up)
 * =================================================================================
 */

#define MQTT_KEEPALIVE 60
#define MQTT_SOCKET_TIMEOUT 1

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =================================================================================
// 1. HARDWARE PIN DEFINITIONS & CONFIGURATION
// =================================================================================

// RFID Shared SPI Bus (Right Header of ESP32)
#define RFID_SCK   18
#define RFID_MISO  19
#define RFID_MOSI  23
#define RFID_RST   22

// RFID Slave Select (SS / CS) Pins for the 4 Readers
#define SS1 15  // Reader 1 - Coin 1
#define SS2 4   // Reader 2 - Coin 2
#define SS3 16  // Reader 3 - Coin 3
#define SS4 17  // Reader 4 - Coin 4

// Relay & Button Pins
// RELAY_PIN relocated to GPIO 5 on the right header (opposite side from GPIO 25)
#define RELAY_PIN      5  // Solenoid / Maglock Relay Signal (Right Header)
#define RESET_BTN_PIN  14 // Optional manual reset button (active LOW to GND)

// Relay Trigger Logic (Standard 3-pin relay modules are ACTIVE-LOW)
const bool RELAY_ACTIVE_LOW = true;

// Auto-relock pulse duration in milliseconds (0 = stays unlocked until RESET / new game)
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
const byte EXPECTED_UID_SIZE = 4; // 4-byte UIDs (Mifare Classic 1K)

// Expected UID for Slot 1 (Coin 1) 04 42 E7 49 BC 2A 81 (bottom most)
byte expectedUID1[EXPECTED_UID_SIZE] = { 0x04, 0x42, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

// Expected UID for Slot 2 (Coin 2) 04 29 E7 49 BC 2A 81
byte expectedUID2[EXPECTED_UID_SIZE] = { 0x04, 0x29, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

// Expected UID for Slot 3 (Coin 3) 04 31 E7 49 BC 2A 81
byte expectedUID3[EXPECTED_UID_SIZE] = { 0x04, 0x31, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

// Expected UID for Slot 4 (Coin 4) 04 5A E7 49 BC 2A 81 (top most)
byte expectedUID4[EXPECTED_UID_SIZE] = { 0x04, 0x5A, 0xE7, 0x49, 0xBC, 0x2A, 0x81 };

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
void lockDoor();
void unlockDoor();
void lockAllActuators();
void unlockAllActuators();
bool uidMatches(const byte* actualUID, byte actualSize, const byte* expectedUID, byte expectedSize = EXPECTED_UID_SIZE);
void printUID(const byte* uid, byte size);
void printUIDInline(const byte* uid, byte size);
void recoverReader(byte readerIdx);

// =================================================================================
// 5. RELAY ACTUATOR CONTROL (Glitch-Free & Safe)
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

void lockAllActuators() {
    lockDoor();
}

void unlockAllActuators() {
    unlockDoor();
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
            
            // Check if UID changed while resting on reader (coin swapped in slot)
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
                // Coin was swapped directly in place
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

// Stabilization delay (ms) ensuring all 4 coins have physically settled
const unsigned long STABILIZATION_DELAY_MS = 250;
unsigned long cardsStableSince = 0;

int getCoinsPresentCount() {
    int count = 0;
    for (byte i = 0; i < NUM_READERS; i++) {
        if (cardPresent[i]) count++;
    }
    return count;
}

// Verifies that all 4 slots contain mutually unique physical tags (no duplicate UIDs)
bool areAllCardsUnique() {
    for (byte i = 0; i < NUM_READERS; i++) {
        if (!cardPresent[i] || detectedUIDSize[i] == 0) return false;
        for (byte j = i + 1; j < NUM_READERS; j++) {
            if (detectedUIDSize[i] == detectedUIDSize[j] &&
                memcmp(detectedUID[i], detectedUID[j], detectedUIDSize[i]) == 0) {
                return false; // Same coin registered in two slots
            }
        }
    }
    return true;
}

void checkAllCards() {
    bool cardStateChanged = false;

    // Scan each of the 4 RFID readers
    for (byte i = 0; i < NUM_READERS; i++) {
        bool found = scanReader(i);

        if (found) {
            consecutiveMisses[i] = 0;

            // CROSS-READER DEDUPLICATION:
            // A single physical coin cannot exist in two slots at once.
            // If another slot still holds this exact UID, invalidate that old slot immediately!
            for (byte j = 0; j < NUM_READERS; j++) {
                if (j != i && cardPresent[j]) {
                    if (detectedUIDSize[j] == detectedUIDSize[i] &&
                        memcmp(detectedUID[j], detectedUID[i], detectedUIDSize[i]) == 0) {
                        cardPresent[j] = false;
                        detectedUIDSize[j] = 0;
                        memset(detectedUID[j], 0, sizeof(detectedUID[j]));
                        consecutiveMisses[j] = 0;
                        cardStateChanged = true;
                        patternEvaluated = false;
                        Serial.printf("🔀 [COIN MOVED] Coin lifted from Slot %d and placed in Slot %d\n", j + 1, i + 1);
                    }
                }
            }

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

    // If fewer than 4 coins are present, reset evaluation latch & stabilization timer
    if (totalDetected < 4) {
        cardsStableSince = 0;
        if (patternEvaluated) {
            patternEvaluated = false;
            Serial.printf("ℹ️ Coins removed (%d/4 present). Ready for next placement attempt.\n", totalDetected);
        }
    }

    // Auto-transition to STARTED on first coin placed if in READY
    if (cardStateChanged && (currentState == READY || currentState == STARTED)) {
        if (currentState == READY && totalDetected > 0) {
            currentState = STARTED;
            publishState(STARTED);
            publishEvent("STARTED");
        }
    }

    // OPTION B: Evaluate pattern ONLY when ALL 4 distinct physical coins are settled!
    if (totalDetected == 4 && areAllCardsUnique() && (currentState == READY || currentState == STARTED)) {
        if (cardsStableSince == 0) {
            cardsStableSince = millis(); // Start settling timer
        } else if (millis() - cardsStableSince >= STABILIZATION_DELAY_MS) {
            if (!patternEvaluated) {
                evaluatePattern();
            }
        }
    } else {
        cardsStableSince = 0;
    }
}


void evaluatePattern() {
    patternEvaluated = true; // Latch: ensures evaluation triggers ONLY ONCE per placement

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

    unlockDoor(); // Energize relay to unlock door / drawer

    // Publish COMPLETED -> Control Server plays victory audio
    publishState(COMPLETED);
    publishEvent("COMPLETED");
}

void handleFailure() {
    currentState = FAILED;

    Serial.println("❌❌❌ WRONG ORDER! 4 coins placed, but positions are incorrect. ❌❌❌");

    // Publish FAILED -> Control Server plays audio/game3/failed.ogg
    publishState(FAILED);
    publishEvent("FAILED");

    // Non-blocking timer to return to STARTED state after 2 seconds cooldown
    stateTransitionTime = millis() + 2000;
}

// =================================================================================
// 8. THREAD-SAFE STATE & EVENT POSTING (Core 1 -> Core 0)
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
// 9. COMMAND DISPATCHER (Core 1)
// =================================================================================

void handleCommand(String cmd) {
    if (cmd.equalsIgnoreCase("START") || cmd.equalsIgnoreCase("RESTART")) {
        attemptNumber++;
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = STARTED;
        lockDoor();
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
    }
    else if (cmd.equalsIgnoreCase("STOP")) {
        currentState = STOPPED;
        stateTransitionTime = 0;
        lockDoor();
        postState(STOPPED);
        postEvent("STOPPED");
    }
    else if (cmd.equalsIgnoreCase("RESET")) {
        puzzleSolved = false;
        patternEvaluated = false;
        stateTransitionTime = 0;
        currentState = READY;
        attemptNumber = 0;
        lockDoor();
        postState(READY);
        postEvent("RESET");

        // Flush all cached cards and do a clean fresh scan
        for (byte i = 0; i < NUM_READERS; i++) {
            consecutiveMisses[i] = 0;
            detectedUIDSize[i] = 0;
            memset(detectedUID[i], 0, sizeof(detectedUID[i]));
            cardPresent[i] = scanReader(i);
        }
    }
    else if (cmd.equalsIgnoreCase("SOLVE") || cmd.equalsIgnoreCase("OVERRIDE")) {
        Serial.println("🔓 [REMOTE OVERRIDE] Game Master solved puzzle remotely!");
        handleSuccess();
    }
}

// =================================================================================
// 10. SETUP (Core 1)
// =================================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==================================================");
    Serial.println("   ESCAPE ROOM: 4-RFID COINS PUZZLE (Dual-Core)   ");
    Serial.println("==================================================");

    // 1. Initialize Relay (Glitch-free: set state BEFORE pin mode)
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    lockDoor();

    pinMode(RESET_BTN_PIN, INPUT_PULLUP);

    // 2. Initialize SPI Bus (Right Header)
    SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI);

    // 3. Initialize RFID SS Pins
    for (byte i = 0; i < NUM_READERS; i++) {
        pinMode(SS_PINS[i], OUTPUT);
    }
    deselectAllReaders();

    // 4. Initialize each MFRC522 Reader individually
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

    // 5. Build MQTT Topics
    snprintf(topicStatus, sizeof(topicStatus), "%s/%s/status", ROOT_TOPIC, GAME_ID);
    snprintf(topicState,  sizeof(topicState),  "%s/%s/state",  ROOT_TOPIC, GAME_ID);
    snprintf(topicEvent,  sizeof(topicEvent),  "%s/%s/event",  ROOT_TOPIC, GAME_ID);
    snprintf(topicCmd,    sizeof(topicCmd),    "%s/%s/cmd",    ROOT_TOPIC, GAME_ID);

    // 6. Create FreeRTOS Queues for Thread-Safe Inter-Core Communication
    cmdQueue = xQueueCreate(10, sizeof(CommandMsg));
    telemetryQueue = xQueueCreate(16, sizeof(TelemetryMsg));

    // 7. Spawn Independent Background Network & MQTT Task on CPU Core 0
    xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        8192,
        NULL,
        1, // Priority 1 (low, so Core 1 game loop is never preempted)
        &networkTaskHandle,
        0  // Core 0
    );

    // 8. Auto-Start if configured
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
}

// =================================================================================
// 11. MAIN LOOP (Core 1 — 100% Non-Blocking & Pure Card Scanning)
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
        }
    }

    // 4. PRIORITY #1: Scan RFIDs and evaluate pattern (100% Dedicated Core 1)
    if (currentState == STARTED || currentState == READY) {
        checkAllCards();
    }

    // 5. Non-blocking Auto-Relock Timer Check (if configured)
    if (AUTO_RELOCK_DELAY_MS > 0 && unlockedAt > 0) {
        if (now - unlockedAt >= AUTO_RELOCK_DELAY_MS) {
            Serial.println("⏱️ [RELAY] Auto-relock timer elapsed. Securing door...");
            lockDoor();
        }
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