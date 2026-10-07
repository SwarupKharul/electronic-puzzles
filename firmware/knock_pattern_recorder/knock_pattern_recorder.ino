/*
 * =================================================================================
 * 🥁 ESCAPE ROOM - KNOCK PATTERN RECORDER & SENSOR CALIBRATOR
 * =================================================================================
 * Purpose:
 *   1. Measures exact knock timings in milliseconds (ms) as you knock naturally.
 *   2. Displays real-time Analog Piezo Sensor peak values.
 *   3. Automatically generates ready-to-copy C++ code for `knock_knock_game.ino`.
 *
 * Compatibility:
 *   - ESP32 (Default pin: GPIO 34)
 *   - Arduino Uno / Nano / Mega (Default pin: A0)
 *
 * How to Use:
 *   1. Upload this sketch to your ESP32 or Arduino.
 *   2. Open Serial Monitor at 115200 baud.
 *   3. Knock your desired rhythm naturally:
 *      e.g. "knock-knock _ knock-knock-knock-knock _ knock"
 *   4. Wait ~1.8 seconds after the last knock.
 *   5. Copy the generated `expectedPattern[]` array directly into your main code!
 *
 * Commands in Serial Monitor:
 *   - 'm' : Toggle Live Sensor Stream (shows continuous analog values for tuning THRESHOLD)
 *   - 'r' : Clear/Reset buffer
 * =================================================================================
 */

// ---------------------------------------------------------------------------------
// 1. HARDWARE PIN DEFINITION
// ---------------------------------------------------------------------------------
const int PIEZO_PIN = 34; // ESP32 Analog Input Pin (matches knock_knock_game.ino)


// ---------------------------------------------------------------------------------
// 2. CALIBRATION & TIMING SETTINGS
// ---------------------------------------------------------------------------------
int THRESHOLD             = 500;   // Trigger threshold (ESP32 ADC is 0-4095; Uno is 0-1023)
const int DEBOUNCE_TIME   = 100;   // Minimum ms between knocks to prevent echo/re-trigger
const int SILENCE_TIMEOUT = 3200;  // Milliseconds of silence (> 3000ms) to finalize pattern recording
const int PEAK_WINDOW_MS  = 25;    // Window to measure maximum impact peak voltage

// ---------------------------------------------------------------------------------
// 3. RECORDING VARIABLES
// ---------------------------------------------------------------------------------
const int MAX_KNOCKS = 32;
unsigned long knockTimes[MAX_KNOCKS];
int knockPeaks[MAX_KNOCKS];
int knockCount = 0;
unsigned long lastKnockTime = 0;
bool recordingActive = false;
bool liveStreamMode = false;
unsigned long lastStreamPrint = 0;

void printInstructions() {
  Serial.println("\n=================================================================");
  Serial.println(" 🥁 ESCAPE ROOM KNOCK RHYTHM RECORDER & CALIBRATOR");
  Serial.println("=================================================================");
  Serial.printf(" 📌 Sensor Pin        : GPIO %d\n", PIEZO_PIN);
  Serial.printf(" ⚡ Detection Threshold: %d\n", THRESHOLD);
  Serial.printf(" ⏱️  Debounce Time     : %d ms\n", DEBOUNCE_TIME);
  Serial.printf(" ⏳ Silence Timeout   : %d ms\n", SILENCE_TIMEOUT);
  Serial.println("-----------------------------------------------------------------");
  Serial.println(" 🎯 INSTRUCTIONS:");
  Serial.println("   Knock your pattern naturally on the sensor surface:");
  Serial.println("   'knock-knock _ knock-knock-knock-knock _ knock'");
  Serial.println("   Wait > 3 seconds of silence, and the timings & C++ code will be printed!");
  Serial.println("   Type 'm' + Enter to toggle live analog sensor streaming.");
  Serial.println("=================================================================\n");
  Serial.println("Ready! Start knocking whenever you are ready...\n");
}

void printRecordedPattern() {
  Serial.println("\n=================================================================");
  Serial.println(" 🏁 RECORDING FINISHED! PATTERN SUMMARY");
  Serial.println("=================================================================");
  Serial.printf(" Total Knocks Recorded: %d\n\n", knockCount);

  if (knockCount < 2) {
    Serial.println("⚠️ Only 1 knock was detected. A pattern needs at least 2 knocks.");
    Serial.println("   Try again and knock your full rhythm!");
    Serial.println("=================================================================\n");
    return;
  }

  int intervals[knockCount - 1];

  Serial.println(" 📊 DETAILED BREAKDOWN:");
  Serial.println(" Knock # | Peak ADC Value | Interval from Prev Knock");
  Serial.println(" --------+----------------+-------------------------");
  Serial.printf("   #1    | %-14d | (Start)\n", knockPeaks[0]);

  for (int i = 0; i < knockCount - 1; i++) {
    intervals[i] = knockTimes[i + 1] - knockTimes[i];
    Serial.printf("   #%-4d | %-14d | Δt = %d ms\n", i + 2, knockPeaks[i + 1], intervals[i]);
  }

  Serial.println("\n-----------------------------------------------------------------");
  Serial.println(" 📋 COPY-PASTE READY CODE FOR knock_knock_game.ino:");
  Serial.println("-----------------------------------------------------------------");

  // Format array
  Serial.print("const int expectedPattern[] = {");
  for (int i = 0; i < knockCount - 1; i++) {
    Serial.print(intervals[i]);
    if (i < knockCount - 2) {
      Serial.print(", ");
    }
  }
  Serial.println("};");
  Serial.printf("const int PATTERN_SIZE = sizeof(expectedPattern) / sizeof(expectedPattern[0]); // %d intervals (%d knocks)\n",
                knockCount - 1, knockCount);
  Serial.println("-----------------------------------------------------------------");
  Serial.println("💡 TIP: Recommended Tolerance: const int TOLERANCE = 40; (+/- 40%)");
  Serial.println("=================================================================\n");
  Serial.println("Ready for another take! Knock again to record a new pattern...\n");
}

void setup() {
  Serial.begin(115200);
  delay(500);

#if defined(ESP32)
  analogReadResolution(12); // ESP32 12-bit ADC (0 - 4095)
#endif

  pinMode(PIEZO_PIN, INPUT);

  printInstructions();
}

void loop() {
  unsigned long now = millis();

  // 1. Check Serial input commands
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'm' || cmd == 'M') {
      liveStreamMode = !liveStreamMode;
      Serial.printf("\n📡 Live sensor stream: %s\n", liveStreamMode ? "ENABLED (Tap sensor to watch values)" : "DISABLED");
    } else if (cmd == 'r' || cmd == 'R') {
      knockCount = 0;
      recordingActive = false;
      Serial.println("\n🔄 Recording buffer reset.");
    }
  }

  // 2. Read Piezo Analog Value
  int rawValue = analogRead(PIEZO_PIN);

  // Live streaming mode (prints every 80ms)
  if (liveStreamMode && (now - lastStreamPrint > 80)) {
    lastStreamPrint = now;
    Serial.printf("📊 Piezo ADC: %4d | Threshold: %4d\n", rawValue, THRESHOLD);
  }

  // 3. Detect Knock Pulse
  if (rawValue > THRESHOLD && (now - lastKnockTime) > DEBOUNCE_TIME) {
    lastKnockTime = now;

    // Sample across a small peak window (25ms) to catch the maximum spike
    int peakValue = rawValue;
    unsigned long sampleStart = millis();
    while (millis() - sampleStart < PEAK_WINDOW_MS) {
      int s = analogRead(PIEZO_PIN);
      if (s > peakValue) peakValue = s;
    }

    if (knockCount < MAX_KNOCKS) {
      knockTimes[knockCount] = lastKnockTime;
      knockPeaks[knockCount] = peakValue;
    }
    knockCount++;
    recordingActive = true;

    // Immediate feedback for this knock
    if (knockCount == 1) {
      Serial.println("---------------------------------------------------------");
      Serial.printf("🥁 Knock #1 Detected! | Peak Value: %d | (Rhythm Started)\n", peakValue);
    } else {
      int interval = lastKnockTime - knockTimes[knockCount - 2];
      Serial.printf("🥁 Knock #%d Detected! | Peak Value: %d | Interval (Δt): %d ms\n",
                    knockCount, peakValue, interval);
    }
  }

  // 4. Check for pattern completion (silence timeout)
  if (recordingActive && (now - lastKnockTime) > SILENCE_TIMEOUT) {
    recordingActive = false;
    printRecordedPattern();
    knockCount = 0; // Reset for next take
  }
}
