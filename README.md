# 🔐 Escape Room Control System (Production Architecture)

> **Centralized Real-Time Control & Telemetry Orchestrator for Escape Rooms**  
> High-performance, fault-tolerant IoT architecture connecting ESP32 hardware props to a centralized control desk, mobile tablets, and automated sound engines.

---

## 🏗 System Architecture & Data Flow

```
                      ┌────────────────────────────────────────────────────────┐
                      │              CONTROL LAPTOP / SERVER HOST              │
                      │                                                        │
                      │  ┌──────────────────────┐    ┌──────────────────────┐  │
                      │  │ Embedded MQTT Broker │    │  Express REST API &  │  │
                      │  │     (Aedes TCP       │◄───┤    Static Server     │  │
                      │  │   Port 1883/1884)    │    │     (Port 3000)      │  │
                      │  └──────────┬───────────┘    └──────────┬───────────┘  │
                      │             │                           │              │
                      │      WS     │ (QoS 1)                   │ Static Assets│
                      │    Stream   ▼                           ▼ & REST Cmds  │
                      │  ┌──────────────────────────────────────────────────┐  │
                      │  │   Browser Control Dashboard (React + Web Audio)  │  │
                      │  │  • Real-time prop cards • Telemetry telemetry   │  │
                      │  │  • Remote force solve   • Zero-latency audio     │  │
                      │  └──────────────────────────────────────────────────┘  │
                      └───────────────────────────┬────────────────────────────┘
                                                  │ Wi-Fi 2.4 GHz
                                 ┌────────────────┼────────────────┐
                                 ▼                ▼                ▼
                          ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
                          │ Game 1 ESP32│  │ Game 2 ESP32│  │ Game N ESP32│
                          │ Knock Sensor│  │ 4-RFID Dolls│  │ Laser Maze  │
                          │   + Relay   │  │+Servo+Relay │  │   + Relay   │
                          └─────────────┘  └─────────────┘  └─────────────┘
```

---

## 🛡 Production Resilience & Fault-Tolerance Guarantees

This system is engineered specifically for live commercial escape rooms where delays, network jitter, or crashes break immersion.

### 1. 100% Local Hardware Autonomy (Fail-Safe Offline Operation)
- **Zero Network Dependency for Solving:** Sensor scanning (RFID SPI, Piezo ADC), rhythm evaluation, and physical lock actuation (Relay energize, Servo actuation) happen **directly on the ESP32 hardware loop**.
- **Network Failure Immunity:** If Wi-Fi drops, the laptop goes to sleep, or the router restarts, **players can still solve the puzzle physically and the door will unlock immediately (<10ms)**.
- **Non-Blocking Architecture:** Network calls never block the main sensor polling loop (`loop()` runs smoothly at >50Hz).

### 2. High-Frequency Real-Time Telemetry & Watchdogs
- **3-Second Heartbeat:** Every prop sends an MQTT telemetry heartbeat packet every `3000ms`.
- **10-Second Physical Watchdog:** If a prop loses power or its connection is cut, the server watchdog detects 3 consecutive missed pings within 10 seconds:
  - Immediately marks the prop as `OFFLINE` in the registry.
  - Automatically **prunes and tears down stale TCP sockets** to prevent ghost connections.
  - Broadcasts a retained `offline` message to all connected dashboards.
- **Clean Startup State:** On server boot, the broker initializes and broadcasts retained `offline` states so no stale `online` ghost status can persist from previous sessions.

### 3. Dual-Channel Command Dispatch (Failover Redundancy)
- **Primary Path (WebSocket MQTT):** Commands (`START`, `RESTART`, `STOP`, `RESET`, `SOLVE`) dispatch instantly over WebSocket with QoS 1 acknowledgment.
- **Failover Backup (HTTP REST):** If browser WebSocket momentarily drops or reconnects, the dashboard automatically routes commands via `POST /api/games/:id/cmd`. The backend broker dispatches the MQTT command directly to the hardware. Zero dropped commands.

### 4. Emergency Remote Override (`⚡ Force Solve`)
- Accessible on every Game Card.
- Sends an instant `SOLVE` override command that triggers the ESP32's `handleSuccess()` handler, energizes the lock relay, moves the servo, and triggers victory fanfare in <50ms.

### 5. Zero-Configuration Networking (mDNS / Bonjour)
- The server advertises `_mqtt._tcp` and `_http._tcp` under `escaperoom.local`.
- ESP32 props automatically detect the server's IP address and active port without requiring static IPs or hardcoded IP changes in firmware.

---

## 📁 Clean Repository Structure

```
├── audio/                      # Audio assets organized by game ID
│   ├── game1/                  # Knock prop sound effects (start, failed, success, etc.)
│   ├── game2/                  # Dolls puzzle sound effects
│   └── game3/                  # Coins puzzle sound effects
├── dist/                       # Production-compiled React frontend (built by Vite)
├── firmware/                   # Arduino / ESP32 C++ firmware sketches
│   ├── knock_knock_game/       # Game 1: Secret rhythm piezo sensor prop
│   │   └── knock_knock_game.ino
│   ├── rfid_dolls/             # Game 2: 4-RFID dolls placement prop
│   │   └── rfid_dolls.ino
│   ├── rfid_coins/             # Game 3: 4-RFID coins placement prop
│   │   └── rfid_coins.ino
│   ├── game_esp32_simulator/   # Hardware simulator sketch for bare ESP32s
│   │   └── game_esp32_simulator.ino
│   └── game_esp32_mqtt.ino     # Boilerplate template for building new props
├── src/                        # Dashboard React source code
│   ├── components/             # Modular UI components (GameCard, EventLog, ComputerLinks)
│   ├── App.jsx                 # Main application controller
│   ├── audio.js                # Web Audio buffer engine + synth fallback
│   ├── index.css               # Clean modern glassmorphic styles
│   ├── main.jsx                # React DOM entry point
│   └── useEscapeRoom.js        # Core MQTT real-time hook with fail-safe watchdog
├── games.json                  # Single source of truth for room game configuration
├── index.js                    # Production orchestrator (Aedes MQTT, Express, mDNS, Watchdogs)
├── test-simulator.js           # CLI software simulator for full room stress-testing
├── test-simulator.bat          # 1-Click hardware simulator for Windows
├── start.sh                    # 1-Click launcher for Linux / macOS
├── start.bat                   # 1-Click launcher for Windows (auto-detects Node.js, builds & runs)
├── create-desktop-shortcut.bat # 1-Click Windows desktop shortcut creator
├── WINDOWS_SETUP.md            # Comprehensive Windows setup guide & driver instructions
├── GAME_MASTER_GUIDE.md        # Standard Operating Procedures & Situation Playbook
└── README.md                   # System Architecture & Technical Documentation
```

---

## ⚡ Quick Start Guide

### 🪟 On Windows (Any PC / Laptop)
1. Double-click **[`start.bat`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/start.bat)**
   - *Auto-detects/installs Node.js LTS, configures Windows Firewall, installs packages, and opens your browser.*
2. (Optional) Double-click **[`create-desktop-shortcut.bat`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/create-desktop-shortcut.bat)** to create an icon on your desktop.
3. For full driver details, COM port troubleshooting, and tablet setup: see **[WINDOWS_SETUP.md](file:///home/swarup/Downloads/escaperoommaster/claude/v2/WINDOWS_SETUP.md)**.

### 🐧 On Linux / macOS
```bash
./start.sh
```
*Installs dependencies, builds the frontend bundle, and launches the server.*

The console will display:
```
=======================================================
 🔐 ESCAPE ROOM CONTROL CENTER (PRODUCTION READY)
-------------------------------------------------------
  🌐 Dashboard URL      : http://192.168.1.9:3000
  📡 mDNS Host URL      : http://escaperoom.local:3000
  ⚡ ESP32 Broker Host  : escaperoom.local / 192.168.1.9
  🔌 ESP32 Broker Port  : 1884 (TCP)
  💬 WebSocket Endpoint : ws://192.168.1.9:3000/mqtt
  🔍 Healthcheck API    : http://192.168.1.9:3000/api/status
=======================================================
```

---

### 2. Multi-Screen & Mobile Tablet URLs

- **Control Room Host Laptop:** Open `http://localhost:3000` (Click **`🔊 Enable Audio`**).
- **Handheld GM Tablet:** Open `http://<laptop-ip>:3000/?audio=off` (Silent monitor).
- **Individual Prop Dedicated Screen:** Open `http://<laptop-ip>:3000/?game=game2`.

---

### 3. Flash ESP32 Prop Firmware

#### Arduino IDE Required Libraries:
Install via **Tools ➔ Manage Libraries...**:
1. **`PubSubClient`** (by Nick O'Leary)
2. **`ArduinoJson`** (by Benoit Blanchon - v6 or v7)
3. **`LiquidCrystal_I2C`** (by Frank de Brabander)
4. **`ESP32Servo`** (by Kevin Harrington - for Game 2)
5. **`MFRC522`** (by GithubCommunity - for Game 2)

#### Configure & Flash:
1. Open `firmware/knock_knock_game/knock_knock_game.ino` or `firmware/rfid_dolls/rfid_dolls.ino`.
2. Update Wi-Fi credentials:
   ```cpp
   const char* WIFI_SSID = "Your_WiFi_SSID";
   const char* WIFI_PASS = "Your_WiFi_Password";
   ```
3. Flash the ESP32. It automatically locates the server via mDNS and connects!

---

### 4. Software Simulation Mode (Test Without Hardware)

You can simulate any prop, event, or failure state without physical hardware:

```bash
# Interactive REPL:
node test-simulator.js

# Instant CLI Commands:
node test-simulator.js game3 SUCCESS    # Trigger victory on Coins prop
node test-simulator.js game2 SUCCESS    # Trigger victory on Dolls prop
node test-simulator.js game1 FAILED     # Trigger failed rhythm on Knock prop
node test-simulator.js game3 offline    # Test offline detection
node test-simulator.js all RESET        # Reset all props to READY
```

---

## 🛡 Linux Firewall Setup (`ufw` / `iptables`)

If tablets or ESP32s cannot reach the laptop over Wi-Fi, ensure local ports are accessible:

```bash
# Allow Web Dashboard & WebSockets (Port 3000)
sudo ufw allow 3000/tcp
sudo iptables -I INPUT -p tcp --dport 3000 -j ACCEPT

# Allow ESP32 MQTT Broker (Port 1884 / 1883)
sudo ufw allow 1884/tcp
sudo iptables -I INPUT -p tcp --dport 1884 -j ACCEPT
```

---

## 🦟 MQTT Broker & Eclipse Mosquitto (Optional Setup)

The Escape Room Master server has an **embedded zero-config MQTT broker (`Aedes`)** built directly into Node.js (running on Port 1883/1884). Installing Mosquitto is **completely optional**.

However, if you wish to run a dedicated standalone **Eclipse Mosquitto** broker or use `mosquitto_sub` / `mosquitto_pub` CLI tools to sniff live prop traffic:

### Linux / Ubuntu / Pop!_OS
```bash
# 1. Install Mosquitto broker and CLI client utilities
sudo apt update && sudo apt install -y mosquitto mosquitto-clients

# 2. Configure Mosquitto 2.0+ to allow remote ESP32s on local Wi-Fi
sudo bash -c 'cat <<EOF > /etc/mosquitto/conf.d/default.conf
listener 1883 0.0.0.0
allow_anonymous true
EOF'

# 3. Restart and enable the service
sudo systemctl restart mosquitto
sudo systemctl enable mosquitto
```

### Running with Pre-Configured Config:
A production config is included in the project root:
```bash
mosquitto -c mosquitto.conf -v
```

### Windows 10 & 11
- Install via Windows Package Manager: `winget install EclipseFoundation.Mosquitto`
- Or use the official installer. See complete step-by-step instructions in [**WINDOWS_SETUP.md**](file:///home/swarup/Downloads/escaperoommaster/claude/v2/WINDOWS_SETUP.md).

### 🔍 Live Sniffing & CLI Control
```bash
# Live monitor all room telemetry and heartbeats:
mosquitto_sub -h localhost -p 1883 -t "escaperoom/#" -v

# Manually trigger Game 1 (Knock) remote solve:
mosquitto_pub -h localhost -p 1883 -t "escaperoom/game1/cmd" -m "SOLVE"

# Manually trigger Game 2 (Dolls) remote reset:
mosquitto_pub -h localhost -p 1883 -t "escaperoom/game2/cmd" -m "RESET"
```

---

## 📖 Game Master Operational Guide

For day-to-day operations, situation handling, clue guidelines, and room turnaround checklists, refer to the companion manual:  
👉 **[GAME_MASTER_GUIDE.md](file:///home/swarup/Downloads/escaperoommaster/claude/v2/GAME_MASTER_GUIDE.md)**
