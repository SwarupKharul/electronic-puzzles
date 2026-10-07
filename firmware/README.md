# ⚡ Escape Room Hardware Firmware

> Production-ready, non-blocking ESP32 C++ firmware sketches for physical escape room props.  
> Designed for 100% offline autonomy (hardware works even if Wi-Fi drops) with automatic mDNS discovery of the central laptop/broker.

---

## 📁 Firmware Directory Structure

| Directory / File | Description | Target Hardware |
| :--- | :--- | :--- |
| [`knock_knock_game/`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/knock_knock_game/knock_knock_game.ino) | **Game 1**: Secret knock rhythm sensor with piezo transducer, I2C LCD, and 12V lock relay. | ESP32 + Piezo Sensor |
| [`rfid_dolls/`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/rfid_dolls/rfid_dolls.ino) | **Game 2**: 4-station RFID puzzle. Players place 4 matching dolls on RFID pedestals. Includes Servo + Relay. | ESP32 + 4x RC522 Readers |
| [`knock_pattern_recorder/`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/knock_pattern_recorder/knock_pattern_recorder.ino) | Calibration utility to record custom knock intervals via Serial Monitor. | ESP32 + Piezo Sensor |
| [`game_esp32_mqtt/`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/game_esp32_mqtt/game_esp32_mqtt.ino) | Clean boilerplate template for building any new custom IoT escape room prop. | Generic ESP32 |
| [`game_esp32_simulator/`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/game_esp32_simulator/game_esp32_simulator.ino) | Bare ESP32 simulator sketch for hardware stress-testing without sensors connected. | Generic ESP32 |

---

## 🛠 Arduino IDE Required Libraries

Install all 5 libraries using **Arduino IDE ➔ Tools ➔ Manage Libraries...**:

1. **`PubSubClient`** (by Nick O'Leary) — MQTT client library
2. **`ArduinoJson`** (by Benoit Blanchon, v6 or v7) — JSON telemetry serialization
3. **`LiquidCrystal_I2C`** (by Frank de Brabander) — 16x2 I2C character LCD display
4. **`ESP32Servo`** (by Kevin Harrington) — PWM servo control on ESP32 (for Game 2)
5. **`MFRC522`** (by GithubCommunity) — RC522 RFID reader SPI interface (for Game 2)

---

## 📌 Game 1: Secret Knock Rhythm Sensor (`knock_knock_game.ino`)

### Wiring & Pinout Table
| Component | ESP32 GPIO | Description / Notes |
| :--- | :--- | :--- |
| **Piezo Sensor (+)** | `GPIO 34` | Analog input (ADC1). Connect 1MΩ resistor in parallel with piezo to GND. |
| **Piezo Sensor (-)** | `GND` | Common ground. |
| **12V Lock Relay (IN)** | `GPIO 25` | Active-LOW relay module signal pin. |
| **I2C LCD (SDA)** | `GPIO 26` | 16x2 LCD display data line. |
| **I2C LCD (SCL)** | `GPIO 27` | 16x2 LCD display clock line. |
| **Manual Reset Button** | `GPIO 14` | Active-LOW momentary push button to GND (internal pull-up). |
| **Power (VCC)** | `VIN` or `5V` | 5V supply for LCD and Relay board. |
| **Ground (GND)** | `GND` | Common ground between ESP32, sensors, and power supplies. |

---

## 📌 Game 2: 4-RFID Dolls Placement (`rfid_dolls.ino`)

### Shared SPI Bus Wiring (All 4 Readers)
All 4 RC522 RFID readers share the same SPI bus lines:
| RFID Pin | ESP32 GPIO | Notes |
| :--- | :--- | :--- |
| **SCK** | `GPIO 18` | Shared across all 4 readers |
| **MISO** | `GPIO 19` | Shared across all 4 readers |
| **MOSI** | `GPIO 23` | Shared across all 4 readers |
| **RST** | `GPIO 22` | Shared reset line across all 4 readers |
| **3.3V** | `3V3` | **Do NOT use 5V!** RC522 is strictly 3.3V logic and power. |
| **GND** | `GND` | Common ground |

### Individual Reader Select (SS / CS) Pins
Each reader has its own dedicated Chip Select pin:
| Reader # | Pedestal / Role | ESP32 GPIO |
| :--- | :--- | :--- |
| **Reader 1** | Doll 1 (Blue) | `GPIO 15` |
| **Reader 2** | Doll 2 (Orange) | `GPIO 4` |
| **Reader 3** | Doll 3 (Yellow) | `GPIO 16` |
| **Reader 4** | Doll 4 (Red) | `GPIO 17` |

### Actuators & Display Wiring
| Component | ESP32 GPIO | Notes |
| :--- | :--- | :--- |
| **Servo Motor (SIG)** | `GPIO 13` | 0° = Locked / Standby, 180° = Unlocked |
| **Lock Relay (IN)** | `GPIO 25` | Active-LOW relay signal pin |
| **I2C LCD (SDA)** | `GPIO 26` | 16x2 LCD display (Address `0x27`) |
| **I2C LCD (SCL)** | `GPIO 27` | 16x2 LCD clock line |
| **Piezo Buzzer** | `GPIO 32` | Local feedback beeps on RFID card scans |
| **Reset Button** | `GPIO 14` | Optional manual override button |

---

## 🚀 Flashing ESP32 Props

1. Connect the ESP32 to your computer using a USB data cable.
2. Open the `.ino` sketch in Arduino IDE.
3. In **Tools**:
   - **Board**: `DOIT ESP32 DEVKIT V1` (or generic `ESP32 Dev Module`)
   - **Upload Speed**: `921600` (or `115200` if upload fails)
   - **Flash Frequency**: `80MHz`
   - **Port**: Select your detected COM port (e.g. `COM3` on Windows, `/dev/ttyUSB0` on Linux)
4. Update Wi-Fi SSID and Password at the top of the sketch:
   ```cpp
   const char* WIFI_SSID = "Your_WiFi_Name";
   const char* WIFI_PASS = "Your_WiFi_Password";
   ```
5. Click **Upload**. (If the console hangs on `Connecting.......`, hold the **BOOT** button on the ESP32 for 2 seconds).
