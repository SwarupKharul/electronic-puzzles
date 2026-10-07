# 🪟 Windows Setup & Deployment Guide

> **Escape Room Master Control System — Production Deployment on Windows**  
> Complete setup guide for fresh Windows 10 & 11 PCs, laptops, and tablets.

---

## ⚡ Quick Start (Setup in 60 Seconds)

If you just moved this folder to a new Windows machine:

1. **Double-click [`start.bat`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/start.bat)** in this folder.
   - **Node.js Check**: If Node.js is not yet installed, `start.bat` will automatically attempt to install it via Windows Package Manager (`winget`) or open the official download page.
   - **Dependencies**: Automatically runs `npm install` on first launch.
   - **Frontend**: Automatically builds the React dashboard if needed.
   - **Firewall**: Automatically adds Windows Firewall rules for Ports `3000`, `1883`, and `1884`.
   - **Browser**: Automatically opens `http://localhost:3000` in your default browser.
2. *(Optional)* **Double-click [`create-desktop-shortcut.bat`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/create-desktop-shortcut.bat)** to create a one-click desktop icon for daily operations.

---

## 📋 Manual Prerequisites (If Not Using Auto-Installer)

1. **Node.js LTS (v18 or v20+)**:
   - Download from [https://nodejs.org](https://nodejs.org) (select the **LTS** version).
   - In the installer, **ensure the checkbox "Add to PATH" is checked** (default).
   - Once installed, open Command Prompt and verify:
     ```cmd
     node -v
     npm -v
     ```

---

## 🌐 Network Configuration (Allowing Tablets & ESP32s)

For Game Master tablets and ESP32 props to connect over Wi-Fi, Windows needs your local network set to **Private** (not "Public").

### 1. Set Wi-Fi to "Private Network" in Windows
1. Click the Wi-Fi icon in your Windows taskbar.
2. Click **Properties** for your connected Wi-Fi network.
3. Under **Network profile type**, select **Private network**.  
   *(If set to "Public network", Windows blocks all incoming connections from other devices).*

### 2. Finding the Laptop's IP Address
When `start.bat` runs, the server console displays your IP address:
```text
🌐 Dashboard URL      : http://192.168.1.45:3000
📡 mDNS Host URL      : http://escaperoom.local:3000
⚡ ESP32 Broker Host  : escaperoom.local / 192.168.1.45
🔌 ESP32 Broker Port  : 1883 or 1884 (TCP)
```

Alternatively, open Command Prompt and type:
```cmd
ipconfig
```
Look for **IPv4 Address** under your Wi-Fi or Ethernet adapter (e.g. `192.168.1.45`).

### 3. Accessing from Tablets / Phones
- **Game Master Tablet (with audio)**: Open `http://<laptop-ip>:3000`
- **Silent Monitor Screen (no sound)**: Open `http://<laptop-ip>:3000/?audio=off`
- **Dedicated Prop Monitor**: Open `http://<laptop-ip>:3000/?game=game1`

---

## 🔌 Connecting & Flashing ESP32 Microcontrollers on Windows

When connecting ESP32 development boards to a Windows computer via micro-USB or USB-C:

### 1. USB-to-UART Drivers
Most ESP32 boards use one of two USB-serial chips:
- **CH340 / CH341** (very common on clone dev boards):
  - If Windows doesn't assign a COM port, download the WCH CH340 driver: [wch.cn driver](http://www.wch-ic.com/downloads/CH341SER_EXE.html).
- **CP2102 / CP2104** (Silicon Labs):
  - Download Silicon Labs CP210x VCP drivers from: [silabs.com VCP drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

To check: Press `Windows Key + X` ➔ Select **Device Manager** ➔ Expand **Ports (COM & LPT)**. You should see something like `Silicon Labs CP210x (COM3)` or `USB-SERIAL CH340 (COM4)`.

### 2. Arduino IDE Setup (Windows)
1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. Go to **File ➔ Preferences**.
3. In **Additional boards manager URLs**, paste:
   ```text
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Go to **Tools ➔ Board ➔ Boards Manager...**, search for `esp32` by **Espressif Systems**, and click **Install**.
5. Install required libraries (**Tools ➔ Manage Libraries...**):
   - `PubSubClient` (by Nick O'Leary)
   - `ArduinoJson` (by Benoit Blanchon, v6 or v7)
   - `LiquidCrystal_I2C` (by Frank de Brabander)
   - `ESP32Servo` (by Kevin Harrington — for RFID dolls servo)
   - `MFRC522` (by GithubCommunity — for RFID dolls RC522 readers)

### 3. Uploading Firmware
1. Open the sketch:
   - Knock puzzle: [`firmware/knock_knock_game/knock_knock_game.ino`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/knock_knock_game/knock_knock_game.ino)
   - RFID dolls: [`firmware/rfid_dolls/rfid_dolls.ino`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/firmware/rfid_dolls/rfid_dolls.ino)
2. In `Tools`:
   - **Board**: `DOIT ESP32 DEVKIT V1` (or generic `ESP32 Dev Module`)
   - **Port**: Select the COM port detected in Device Manager (e.g., `COM3`)
   - **Upload Speed**: `921600` (or `115200` if upload fails)
3. Update Wi-Fi SSID and Password at the top of the sketch.
4. Click **Upload** (Arrow button).
   *(If the terminal says "Connecting.......", hold down the **BOOT** button on the ESP32 for 2 seconds until uploading begins).*

---

## 🎮 Testing Without Hardware (Windows Simulator)

You don't need ESP32 hardware connected to test or train Game Masters!

1. Start the server using `start.bat`.
2. Double-click [`test-simulator.bat`](file:///home/swarup/Downloads/escaperoommaster/claude/v2/test-simulator.bat).
3. The interactive simulator will connect to the server and appear on the web dashboard.
4. Type commands into the simulator prompt:
   - `1` ➔ Trigger Knock knock puzzle
   - `2` ➔ Trigger Dolls puzzle
   - `sim` ➔ Start automatic stress-testing loop
   - `q` ➔ Exit simulator

---

## ❓ Windows Troubleshooting & FAQs

### Q: "Port 3000 is already in use"
**Fix**: Another program or previous instance of Node is running.
1. Open Command Prompt and run:
   ```cmd
   netstat -ano | findstr :3000
   ```
2. Note the PID (the number at the right) and kill it:
   ```cmd
   taskkill /PID <PID_NUMBER> /F
   ```
   Or simply restart your computer.

### Q: Browser says "Audio autoplay was blocked"
**Fix**: Modern browsers require user interaction before playing audio. Click the large **🔊 Enable Audio** button in the top navigation bar of the dashboard on startup.

### Q: Tablets cannot open `http://<laptop-ip>:3000`
**Checklist**:
1. Are the tablet and laptop on the **exact same Wi-Fi router**? (Some routers separate 2.4 GHz and 5 GHz networks or have "AP Isolation" enabled).
2. Check Windows Firewall: Make sure your Wi-Fi connection is set to **Private** network (see Section above).
3. In `start.bat`, the firewall rule `EscapeRoom-Control-Ports` was automatically created. You can verify it in Windows Defender Firewall with Advanced Security ➔ Inbound Rules.

### Q: What if Wi-Fi drops or the laptop turns off during a game?
**The hardware works 100% offline!**
- Both the Knock Knock puzzle and the RFID Dolls puzzle run their detection loops entirely on the ESP32.
- If Wi-Fi is severed or the laptop is shut down, the magnetic locks, relays, and servos will still open immediately when players solve the puzzles correctly.
