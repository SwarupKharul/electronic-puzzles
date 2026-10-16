# 🪙 Game 3: 4-RFID Coins Positional Puzzle (`rfid_coins.ino`)

A production-ready dual-core FreeRTOS firmware for a 4-station RFID coins placement prop. Players place 4 physical coins (adorned with 13.56 MHz RFID stickers) into 4 designated slots. The puzzle evaluates the pattern **only when all 4 coins are placed (4/4 present)**.

---

## 🛠️ Hardware Wiring Guide

### 1. 4x MFRC522 RFID Readers (Shared SPI Bus)
All 4 RC522 readers share the SPI data and clock lines on the ESP32:

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

| Reader # | Role / Coin Slot | ESP32 GPIO | Header Location |
| :--- | :--- | :--- | :--- |
| **Reader 1** | Coin Slot 1 | `GPIO 15` (SS1) | Right Header |
| **Reader 2** | Coin Slot 2 | `GPIO 4` (SS2) | Right Header |
| **Reader 3** | Coin Slot 3 | `GPIO 16` (SS3) | Right Header |
| **Reader 4** | Coin Slot 4 | `GPIO 17` (SS4) | Right Header |

### 3. Actuator & Controls (Production Setup)
*Simplified production build: No servo motors, no LCD, no piezo buzzer.*

| Component | ESP32 GPIO | Purpose / Notes |
| :--- | :--- | :--- |
| **Relay Module (SIG)** | `GPIO 5` | 12V Solenoid / Maglock lock (Active-LOW, Right Header) |
| **Relay Module (VCC)** | `VIN` | 5V Power (from USB or external 5V) |
| **Relay Module (GND)** | `GND` | Common Ground |
| **Manual Reset Button**| `GPIO 14` | Optional Active-LOW push button connected to GND |

> [!NOTE]
> **Pin Relocation:** The relay signal pin is placed on `GPIO 5` (on the right header of the ESP32), aligning neatly with the SPI/RFID connections for cleaner wiring and shorter jumper traces.

### 4. 12V Lock Wiring (Relay Terminal Side)
- **12V Solenoid Cabinet Lock (Fail-Secure - Locked when unpowered):**
  - `12V Power (+)` ➔ Relay `COM`
  - Relay `NO` (Normally Open) ➔ Solenoid `(+)`
  - `12V Power (-)` ➔ Solenoid `(-)`
- **12V Electromagnetic Lock / Maglock (Fail-Safe - Locked when powered):**
  - `12V Power (+)` ➔ Relay `COM`
  - Relay `NC` (Normally Closed) ➔ Maglock `(+)`
  - `12V Power (-)` ➔ Maglock `(-)`

---

## ⚙️ Game Rules & Detection Logic

1. **Positional Evaluation:**
   - Players can place the 4 coins in any sequence or order of arrival.
   - The pattern is evaluated **only when all 4 coins are placed (4/4 present)**.
2. **Success:**
   - When Slot 1 holds Coin 1, Slot 2 holds Coin 2, Slot 3 holds Coin 3, and Slot 4 holds Coin 4:
     - The relay energizes to unlock the door/compartment.
     - ESP32 publishes `COMPLETED` over MQTT.
     - Escape Room Master server plays victory audio (`audio/game3/success.ogg`).
3. **Failure:**
   - If all 4 coins are placed but any coin is in the wrong position:
     - ESP32 publishes `FAILED` over MQTT.
     - Escape Room Master server plays failure audio (`audio/game3/failed.ogg`).
     - A 2-second cooldown triggers, allowing players to lift and swap coins.

---

## 🏷️ Setting Up Your Coin RFID UIDs

1. Upload the sketch to your ESP32.
2. Open the Arduino **Serial Monitor** at `115200 baud`.
3. Tap each of your 4 coins on the readers. Note the hexadecimal UID output in the log:
   ```text
   📍 Slot 1 [COIN 1 (SLOT 1)]: Coin INSERTED! UID: 04 11 22 33 44 55 66
   ```
4. Copy the UIDs into lines 135–145 of `rfid_coins.ino`:
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
- `STOP` ➔ Pauses game and locks relay.
- `RESET` ➔ Returns to standby READY state and locks relay.
- `SOLVE` / `OVERRIDE` ➔ Remotely unlocks relay and marks game completed.
