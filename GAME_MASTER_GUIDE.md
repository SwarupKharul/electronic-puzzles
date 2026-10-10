# 🎮 Escape Room Game Master (GM) Operational Runbook & SOP

> **Field Guide & Standard Operating Procedures (SOP)**  
> *Production-Ready Guide for Game Masters, Room Hosts, and Tech Operators.*

---

## ⚡ 60-Second Emergency Cheat Sheet

| Situation | Game Master Immediate Action |
| :--- | :--- |
| **Players Stuck / Time Running Out** | Click **`⚡ Force Solve`** on the Game Card. Maglock/Servo unlocks instantly (<50ms). |
| **Tag Misread / Hardware Malfunction** | Click **`⚡ Force Solve`** to bypass the physical sensor without entering the room. |
| **Prop Shows "OFFLINE"** | Don't panic: **Props operate autonomously.** Even offline, players can solve it physically. To restore telemetry, power cycle the prop. |
| **No Sound / Audio Silent** | Click the glowing **`🔊 Enable Audio`** button in the header. (Browser autoplay restriction). |
| **Reset Room for Next Team** | Reset physical props (remove dolls / relock door) ➔ Click **`⟲ Reset All`** on Dashboard. |
| **Server / Laptop Restarted** | Simply re-run `./start.sh` (or `start.bat`). Props reconnect and re-sync in ~4 seconds automatically. |

---

## 🖥 Quick Access URLs

| **Device** | **URL** | **Audio Behavior** |
| :--- | :--- | :--- |
| **Main Control Laptop** | `http://localhost:3000` or `http://escaperoom.local:3000` | Full Web Audio (SFX, fanfares, music) |
| **GM Handheld Tablet / Phone** | `http://<laptop-ip>:3000/?audio=off` | Silent telemetry monitor (saves battery, no audio bleed) |
| **Single Prop Dedicated Screen** | `http://<laptop-ip>:3000/?game=game2` | Focused monitor for 4-RFID Dolls puzzle only |
| **Coins Prop Dedicated Screen** | `http://<laptop-ip>:3000/?game=game3` | Focused monitor for 4-RFID Coins puzzle only |

*(Note: `<laptop-ip>` is displayed in the server terminal upon launch, e.g. `192.168.1.9`).*

---

## 📋 Pre-Game Launch Checklist (Before Players Enter)

Execute this 5-minute routine before welcoming every player team:

```
[ ] 1. Boot Control Laptop & connect to Room Wi-Fi (5GHz or 2.4GHz).
[ ] 2. Launch Server: Double-click start.bat (Windows) or run ./start.sh (Linux/Mac).
[ ] 3. Open Browser Dashboard: Go to http://localhost:3000.
[ ] 4. Click 'Enable Audio' in the top header. Test with '🔊 success' button.
[ ] 5. Power ON all physical props in the rooms.
[ ] 6. Verify Dashboard: All props must display green 'LIVE' status pills within 5 seconds.
[ ] 7. Physical Room Reset:
       - Dolls Puzzle: All 4 dolls removed from slots; maglock/door closed and locked.
       - Coins Puzzle: All 4 coins removed from slots; compartment/door closed and locked.
       - Knock Puzzle: Piezo surface clear; door closed and locked.
[ ] 8. Verify LCD Displays:
       - Game 1 LCD: 'READY TO KNOCK' or 'ATTEMPT #1'
       - Game 2 LCD: 'DOLLS: 0/4 READY'
       - Game 3 LCD: 'COINS: 0/4 READY'
[ ] 9. Click '▶ Start' or '⟲ Reset' on dashboard to sync session clock.
```

---

## 🚨 Incident Response & Situation Playbook

### Situation 1: Players are Completely Stuck / Time is Almost Up
- **Symptom:** Players have exhausted hints, 55 minutes have passed, and frustration is mounting.
- **Action:**
  1. On the Game Card, click **`⚡ Force Solve`**.
  2. Confirm the prompt: *"Emergency Override: Force solve and unlock...?"*
  3. **Result:**
     - The relay energizes and solenoid/maglock unlocks immediately.
     - The servo arm rotates to 90° (unlocked).
     - The prop LCD displays `PUZZLE SOLVED! DOOR UNLOCKED`.
     - The dashboard plays the victory sound and marks the card as **SUCCESS**.

---

### Situation 2: Prop Shows Red "OFFLINE" During an Active Game
- **What it Means:** The laptop has not received telemetry or a heartbeat from the ESP32 in >10 seconds.
- **Root Causes:**
  1. Players kicked or tugged the power adapter / battery wire.
  2. Temporary Wi-Fi router reboot or channel congestion.
- **Critical Resilience Guarantee:**
  > **Props are 100% locally autonomous.** The ESP32 evaluates sensors and opens physical locks directly on hardware. Even if completely disconnected from Wi-Fi/laptop, **players can still solve the puzzle and the door will unlock!**
- **Action:**
  1. Check if players can still interact with the prop (check if LCD is lit).
  2. If LCD is lit and players are playing, **let them finish.** Do not disrupt the game.
  3. If physical power was cut:
     - Check the wall plug / battery pack.
     - Turn power switch OFF, wait 3 seconds, turn switch ON.
     - The prop boots, connects to Wi-Fi, and re-registers as **LIVE** in ~4 seconds.

---

### Situation 3: Players Placed All Dolls / Knocked Rhythm, But Nothing Unlocks
- **Symptom:** Players insist they solved the puzzle, but the door remains locked.
- **Investigation:**
  - **Dolls Puzzle (Game 2):**
    - Check the prop LCD display. It shows live placement: `D1:OK D2:OK D3:-- D4:OK`.
    - If one slot shows `--` or `?`, the doll's RFID tag is slightly off-center or lifted.
    - Ask players over intercom: *"Make sure all 4 dolls are seated squarely on their pedestals."*
  - **Coins Puzzle (Game 3):**
    - Check the prop LCD display. It shows slot state: `1:O 2:O 3:. 4:O`.
    - Remember: evaluation triggers **only when all 4 coins are placed (4/4 present)**.
    - If LCD shows `WRONG ORDER! TRY AGAIN...`, coins are placed in the wrong slots.
    - Ask players over intercom: *"Ensure all 4 coins are seated firmly in their designated slots."*
  - **Knock Puzzle (Game 1):**
    - The pattern expects rhythmic knocks: `knock-knock _ knock-knock-knock-knock _ knock` (7 knocks total).
    - **Tolerances:**
      - **Small Gap (`-`):** 240 ms – 1300 ms (quick consecutive knocks)
      - **Big Gap (`_`):** 1300 ms – 2800 ms (phrase pauses)
      - **Pattern End:** > 3000 ms of silence triggers final evaluation and unlocks the door.
    - If players knock erratically or with the wrong count, LCD will flash `TRY AGAIN`.
- **Fail-Safe Resolution:**
  - If players placed dolls in the correct thematic positions but an RFID tag is worn or damaged, **do not break their immersion.**
  - Click **`⚡ Force Solve`** from the control desk. The door unlocks, audio triggers, and the players feel the rush of success.

---

### Situation 4: Accidental Button Click ("STOP" or "RESET" by Mistake)
- **Symptom:** The Game Master accidentally clicked `⏹ Stop` or `⟲ Reset` during an active run.
- **Action:**
  - Click **`▶ Start`** or **`↻ Restart`** immediately.
  - The prop resets its evaluation flags, re-scans physical slots immediately, and returns to the active `STARTED` state without locking players out.

---

### Situation 5: Control Laptop Crashed, Went to Sleep, or Broker Restarted
- **What Happens:**
  - Browser dashboard disconnects (`❌ Server Disconnected` banner).
- **Action:**
  1. Restart the server laptop or terminal (`./start.sh` or `start.bat`).
  2. Open `http://localhost:3000`.
  3. The embedded Aedes broker boots on TCP port 1884.
  4. Both ESP32 props automatically detect the broker via mDNS and reconnect within 5 seconds.
  5. Props re-publish their current state (`STARTED` / `COMPLETED` / `READY`), so the dashboard restores exact room state without losing progress!

---

### Situation 6: Audio is Silent on Control Room Speakers
- **Investigation:**
  1. Look at the top bar: Does it say **`🔊 Enable Audio`**?
     - Modern browsers block sound until a user interacts with the page. Click **`Enable Audio`** once per session.
  2. Check the volume slider next to the Mute button.
  3. Check the laptop's physical OS volume and HDMI/Aux cable connection.
  4. Click the **`🔊 success`** or **`🔊 start`** test button on any Game Card to verify speaker output.

---

## 🔄 Turnaround & Room Reset Procedure (Between Games)

Follow this 2-minute reset procedure between booking groups:

1. **Step 1: Physical Props**
   - **Game 1 (Knock Prop):** Close puzzle door/box firmly. Verify magnetic lock engages.
   - **Game 2 (Dolls Prop):** Remove all 4 dolls from the pedestals and return them to their hidden starting locations. Close and latch the prop door/compartment.
   - **Game 3 (Coins Prop):** Remove all 4 coins from the slots and return them to their hidden puzzle locations. Close and latch coin compartment/drawer.
2. **Step 2: Dashboard Reset**
   - Click the header button: **`⟲ Reset All`**.
   - Confirm the dialog.
3. **Step 3: Verification**
   - Both cards should show State: **`READY`** (Attempt #1).
   - Both cards must show green **`LIVE`** badges.
   - Relock mechanisms should be engaged (maglocks magnetized, servo at 0°).
4. **Step 4: Ready**
   - Welcome the next group into the briefing room!

---

## 🎛 Dashboard Control Button & State Reference

### Prop State Lifecycle

```
    [ BOOT / RESET ] ──► READY (Armed, Timer at 00:00, Door Locked)
                           │
                           ▼ (First doll placed / knock heard / GM clicks Start)
                        STARTED (Active play, Timer running)
                           │
                           ├──► FAILED (Wrong pattern buzzer, 2s auto-revert to STARTED)
                           │
                           ▼ (Correct dolls / rhythm matched / Force Solve)
                        COMPLETED (Solved! Door Unlocked, Victory Chime, Solve Time Stored)
```

| State | Badge Color | Meaning |
| :--- | :--- | :--- |
| **`READY`** | Blue | Prop is armed and waiting for players. (Timer at 00:00, Door locked). |
| **`STARTED`** | Blue (Active) | Game attempt is in progress. (Timer ticking). |
| **`COMPLETED`** | Green | Puzzle solved! Door/compartment is unlocked. (Solve time recorded). |
| **`FAILED`** | Red | Temporary wrong attempt cooldown (automatically returns to STARTED). |
| **`STOPPED`** | Amber | Prop paused/overridden by Game Master. |

| Button | Color | MQTT Payload | Action |
| :--- | :--- | :--- | :--- |
| **`▶ Start`** | Blue | `START` | Starts the attempt timer, increments attempt counter, begins active listening. |
| **`↻ Restart`** | Orange | `RESTART` | Increments attempt counter, clears errors, re-scans physical sensors fresh. |
| **`⏹ Stop`** | Red | `STOP` | Suspends puzzle evaluation and locks actuator. |
| **`⟲ Reset`** | Gray | `RESET` | Restores prop to **`READY`** state; locks door and sets servo to 0°. |
| **`⚡ Force Solve`**| Emerald | `SOLVE` | **Emergency Override:** Unlocks relay and moves servo to 90° immediately. |
| **`🩺 Debug`** | Slate | Local UI | Expands telemetry drawer showing live heartbeat count and signal latency. |

---

## 📞 Hardware Emergency Support

If a prop experiences physical hardware failure:
- **Physical Reset Button:** Every prop has a physical push-button on its electronics enclosure (GPIO 14). Pressing it triggers an instant hardware-level re-scan and reset.
- **Direct Solenoid Mechanical Bypass:** All magnetic and solenoid door strikes must have a mechanical keyhole or emergency manual pull-pin as required by life safety codes.
