# 🪙 Game 3: 4-RFID Coins Positional Puzzle (`rfid_coins.ino`)

A dual-core FreeRTOS firmware for a 4-station RFID coins placement prop. Players place 4 physical coins (adorned with 13.56 MHz RFID stickers) into 4 designated slots. The puzzle evaluates the pattern **only when all 4 coins are placed (4/4 present)**.

---

## 🛠️ Hardware Wiring Guide

### 1. 4x MFRC522 RFID Readers (Shared SPI Bus)
All 4 RC522 readers share the SPI data and clock lines:

| MFRC522 Pin | ESP32 GPIO | Description / Notes |
| :--- | :--- | :--- |
| **SCK** | `GPIO 18` | Shared Clock line for all 4 readers |
| **MISO** | `GPIO 19` | Shared Master-In-Slave-Out line |
| **MOSI** | `GPIO 23` | Shared Master-Out-Slave-In line |
| **RST** | `GPIO 22` | Shared Reset line for all 4 readers |
| **3.3V** | `3V3` | **Strictly 3.3V power!** Do NOT connect 5V to RC522. |
| **GND** | `GND` | Common Ground |

### 2. Dedicated Reader Select (SDA / SS / CS) Pins
Each reader has its own unique Slave Select pin:

| Reader # | Role / Coin Slot | ESP32 GPIO |
| :--- | :--- | :--- |
| **Reader 1** | Coin Slot 1 | `GPIO 15` (SS1) |
| **Reader 2** | Coin Slot 2 | `GPIO 4` (SS2) |
| **Reader 3** | Coin Slot 3 | `GPIO 16` (SS3) |
| **Reader 4** | Coin Slot 4 | `GPIO 17` (SS4) |

### 3. Actuators & Indicators
| Component | ESP32 GPIO | Purpose / Notes |
| :--- | :--- | :--- |
| **Relay Module (SIG)** | `GPIO 25` | 12V Solenoid / Maglock lock (Active-LOW) |
| **Servo 1 (SIG)** | `GPIO 13` | Primary lock latch (0° locked ➔ 90° unlocked) |
| **Servo 2 (SIG)** | `GPIO 33` | Secondary actuator (coin drawer / compartment) |
| **Servo 3 (SIG)** | `GPIO 12` | Tertiary actuator (trapdoor / reveal) |
| **I2C LCD (SDA)** | `GPIO 26` | 16x2 LCD Data line (Address `0x27`) |
| **I2C LCD (SCL)** | `GPIO 27` | 16x2 LCD Clock line |
| **Piezo Buzzer** | `GPIO 32` | Success chirp & error buzz audio feedback |
| **Manual Reset Button**| `GPIO 14` | Active-LOW button connected to GND |

> [!TIP]
> **Power Supply for Servos:** Servos draw high peak current when moving. Power the 3 servos from an **external 5V 2A-3A power supply** (with common GND shared with the ESP32), rather than the ESP32's onboard 5V pin.

---

## ⚙️ Game Rules & Detection Logic

1. **Option B Evaluation:**
   - Players can place the 4 coins in any sequence or order of arrival.
   - The LCD displays real-time slot state: `1:O 2:. 3:O 4:. | Placed: 2/4 Coins`.
   - The pattern is evaluated **only when all 4 coins are placed (4/4 present)**.
2. **Success:**
   - If Slot 1 holds Coin 1, Slot 2 holds Coin 2, Slot 3 holds Coin 3, and Slot 4 holds Coin 4:
     - Relays energize and servos rotate to 90°.
     - Control server receives `COMPLETED` and plays victory audio.
     - LCD displays `PUZZLE SOLVED! / COINS ACCEPTED`.
3. **Failure:**
   - If any coin is in the wrong position:
     - Buzzer sounds a low failure buzz.
     - Control server receives `FAILED` and plays error sound.
     - LCD displays `WRONG ORDER! / TRY AGAIN...`.
     - Players can lift and swap coins to find the correct configuration.

---

## 🏷️ Setting Up Your Coin RFID UIDs

1. Upload the sketch to your ESP32.
2. Open the Arduino **Serial Monitor** at `115200 baud`.
3. Tap each of your 4 coins on the readers. Note the hexadecimal UID output in the log:
   ```text
   📍 Slot 1 [COIN 1 (SLOT 1)]: Coin INSERTED! UID: 04 11 22 33 44 55 66
   ```
4. Copy the UIDs into lines 157–167 of `rfid_coins.ino`:
   ```cpp
   byte expectedUID1[EXPECTED_UID_SIZE] = { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
   byte expectedUID2[EXPECTED_UID_SIZE] = { 0x04, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
   byte expectedUID3[EXPECTED_UID_SIZE] = { 0x04, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
   byte expectedUID4[EXPECTED_UID_SIZE] = { 0x04, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };
   ```
5. Re-flash the ESP32.

---

## 📡 Remote Game Master Commands
The prop listens on MQTT topic `escaperoom/game3/cmd`:
- `START` / `RESTART` ➔ Resets attempts and begins puzzle loop.
- `STOP` ➔ Pauses game and locks all actuators.
- `RESET` ➔ Returns to standby READY state and locks actuators.
- `SOLVE` / `OVERRIDE` ➔ Remotely unlocks door and marks game completed.
